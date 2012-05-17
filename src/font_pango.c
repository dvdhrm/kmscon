/*
 * kmscon - Font Handling - Pango Backend
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Font Handling - Pango
 * This provides a font backend based on Pango library. It can draw any kind of
 * text we want so it is perfect for our console.
 */

#include <cairo.h>
#include <errno.h>
#include <glib.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "gl.h"
#include "log.h"
#include "misc.h"
#include "unicode.h"

#define LOG_SUBSYSTEM "font_pango"

enum glyph_type {
	GLYPH_INVALID,
	GLYPH_LAYOUT,
	GLYPH_STRING,
};

struct font_glyph {
	kmscon_symbol_t ch;
	unsigned int type;
	unsigned int width;
	unsigned int ascent;
	unsigned int descent;

	union {
		PangoLayout *layout;
		struct glyph_str {
			PangoFont *font;
			PangoGlyphString *str;
		} string;
	};
};

struct font_face {
	unsigned long ref;
	struct font_face *next;

	struct font_attr attr;

	unsigned int width;
	unsigned int height;
	PangoContext *ctx;
	struct kmscon_hashtable *glyphs;
};

static pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct font_face *manager__faces;
static PangoFontMap *manager__lib;

static void manager_lock()
{
	pthread_mutex_lock(&manager_mutex);
}

static void manager_unlock()
{
	pthread_mutex_unlock(&manager_mutex);
}

static void glyph_free(struct font_glyph *glyph)
{
	manager_lock();

	if (glyph->type == GLYPH_STRING) {
		g_object_unref(glyph->string.font);
		pango_glyph_string_free(glyph->string.str);
	} else if (glyph->type == GLYPH_LAYOUT) {
		g_object_unref(glyph->layout);
	}

	manager_unlock();

	free(glyph);
}

static int face_lookup(struct font_face *face, struct font_glyph **out,
			kmscon_symbol_t ch)
{
	struct font_glyph *glyph;
	PangoLayout *layout;
	PangoLayoutLine *line;
	PangoGlyphItem *tmp;
	PangoGlyphString *str;
	PangoRectangle rec;
	size_t len;
	const char *val;
	bool res;
	int ret;

	res = kmscon_hashtable_find(face->glyphs, (void**)&glyph,
					(void*)(long)ch);
	if (res) {
		*out = glyph;
		return 0;
	}

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;
	memset(glyph, 0, sizeof(*glyph));
	glyph->ch = ch;

	manager_lock();

	layout = pango_layout_new(face->ctx);
	val = kmscon_symbol_get_u8(ch, &len);
	pango_layout_set_text(layout, val, len);
	kmscon_symbol_free_u8(val);

	pango_layout_get_pixel_extents(layout, NULL, &rec);
	glyph->ascent = PANGO_PIXELS_CEIL(pango_layout_get_baseline(layout));
	glyph->descent = rec.height - glyph->ascent;
	glyph->width = rec.width;

	if (pango_layout_get_line_count(layout) != 1 || !glyph->width) {
		glyph->type = GLYPH_INVALID;
		g_object_unref(layout);
		goto unlock;
	}

	line = pango_layout_get_line_readonly(layout, 0);
	if (!line->runs || line->runs->next) {
		glyph->type = GLYPH_LAYOUT;
		glyph->layout = layout;
	} else {
		tmp = line->runs->data;
		str = pango_glyph_string_copy(tmp->glyphs);
		glyph->type = GLYPH_STRING;
		glyph->string.str = str;
		glyph->string.font = g_object_ref(tmp->item->analysis.font);
		g_object_unref(layout);
	}

unlock:
	manager_unlock();

	ret = kmscon_hashtable_insert(face->glyphs, (void*)(long)ch, glyph);
	if (ret) {
		glyph_free(glyph);
		return ret;
	}

	*out = glyph;
	return 0;
}

static int attr_cpy(struct font_attr *dest, const struct font_attr *src,
			bool alloc)
{
	memcpy(dest, src, sizeof(*dest));
	if (!dest->dpi)
		dest->dpi = 96;
	if (!dest->name)
		dest->name = "monospace";

	if (alloc) {
		dest->name = strdup(dest->name);
		if (!dest->name)
			return -ENOMEM;
	}

	return 0;
}

static void attr_clean(struct font_attr *dest)
{
	free((char*)dest->name);
}

static bool attr_equal(const struct font_attr *a1,
			const struct font_attr *a2)
{
	struct font_attr b1, b2;

	attr_cpy(&b1, a1, false);
	attr_cpy(&b2, a2, false);

	if (a1->points != a2->points)
		return false;
	if (a1->dpi != a2->dpi)
		return false;
	if (a1->bold != a2->bold)
		return false;
	if (a1->style != a2->style)
		return false;
	if (strcmp(a1->name, a2->name))
		return false;

	return true;
}

static int face__new(struct font_face **out, const struct font_attr *attr,
			bool absolute)
{
	struct font_face *face;
	int ret;
	cairo_font_options_t *opt;
	PangoFontDescription *desc;
	unsigned int style, weight;

	face = malloc(sizeof(*face));
	if (!face)
		return -ENOMEM;
	memset(face, 0, sizeof(*face));
	face->ref = 1;

	ret = attr_cpy(&face->attr, attr, true);
	if (ret)
		goto err_free;

	if (face->attr.bold)
		weight = PANGO_WEIGHT_BOLD;
	else
		weight = PANGO_WEIGHT_NORMAL;

	if (face->attr.style == FONT_ITALIC)
		style = PANGO_STYLE_ITALIC;
	else if (face->attr.style == FONT_OBLIQUE)
		style = PANGO_STYLE_OBLIQUE;
	else
		style = PANGO_STYLE_NORMAL;

	ret = kmscon_hashtable_new(&face->glyphs, kmscon_direct_hash,
					kmscon_direct_equal, NULL,
					(kmscon_free_cb)glyph_free);
	if (ret)
		goto err_attr;

	log_info("loading new font: %s", face->attr.name);

	face->ctx = pango_font_map_create_context(manager__lib);
	pango_context_set_base_dir(face->ctx, PANGO_DIRECTION_LTR);
	pango_context_set_language(face->ctx, pango_language_get_default());
	pango_cairo_context_set_resolution(face->ctx, face->attr.dpi);

	desc = pango_font_description_from_string(face->attr.name);
	if (absolute)
		pango_font_description_set_absolute_size(desc,
					PANGO_SCALE * face->attr.points);
	else
		pango_font_description_set_size(desc,
					PANGO_SCALE * face->attr.points);
	pango_font_description_set_weight(desc, weight);
	pango_font_description_set_style(desc, style);
	pango_font_description_set_variant(desc, PANGO_VARIANT_NORMAL);
	pango_font_description_set_stretch(desc, PANGO_STRETCH_NORMAL);
	pango_font_description_set_gravity(desc, PANGO_GRAVITY_SOUTH);
	pango_context_set_font_description(face->ctx, desc);
	pango_font_description_free(desc);

	if (!pango_cairo_context_get_font_options(face->ctx)) {
		opt = cairo_font_options_create();
		if (!opt) {
			log_err("cannot create cairo font options");
			ret = -EFAULT;
			goto err_ctx;
		}
		pango_cairo_context_set_font_options(face->ctx, opt);
		cairo_font_options_destroy(opt);
	}

	*out = face;
	return 0;

err_ctx:
	g_object_unref(face->ctx);
	kmscon_hashtable_free(face->glyphs);
err_attr:
	attr_clean(&face->attr);
err_free:
	free(face);
	return ret;
}

static void face__free(struct font_face *face)
{
	manager_unlock();
	kmscon_hashtable_free(face->glyphs);
	manager_lock();
	g_object_unref(face->ctx);
	attr_clean(&face->attr);
	free(face);
}

static int manager__init()
{
	if (!manager__lib) {
		manager__lib = pango_cairo_font_map_new();
		if (!manager__lib) {
			log_warn("cannot create font map");
			return -EFAULT;
		}
	}

	return 0;
}

static void manager__add(struct font_face *face)
{
	face->next = manager__faces;
	manager__faces = face;
}

static void manager__remove(struct font_face *face)
{
	struct font_face *iter;

	if (!manager__faces) {
		face->next = NULL;
	} else if (manager__faces == face) {
		manager__faces = face->next;
		face->next = NULL;
	} else {
		iter = manager__faces;
		for ( ; iter->next; iter = iter->next) {
			if (iter->next == face) {
				iter->next = face->next;
				face->next = NULL;
				break;
			}
		}
	}
}

static void face_unref(struct font_face *face)
{
	manager_lock();
	if (!--face->ref) {
		manager__remove(face);
		face__free(face);
	}
	manager_unlock();
}

/* Measure font width and height
 * We simply draw all ASCII characters and use the average width as default
 * character width. The height is the maximum ascent+descent.
 * This has the side effect that all ASCII characters are already cached and the
 * console will speed up.
 */
static int face_measure(struct font_face *face)
{
	unsigned int i, num, width, asc, desc;
	int ret;
	kmscon_symbol_t ch;
	struct font_glyph *glyph;

	num = 0;
	width = 0;
	asc = 0;
	desc = 0;
	for (i = 0x20; i < 0x7f; ++i) {
		ch = kmscon_symbol_make(i);
		ret = face_lookup(face, &glyph, ch);
		if (ret)
			continue;

		if (glyph->width > 0) {
			width += glyph->width;
			if (glyph->ascent > asc)
				asc = glyph->ascent;
			if (glyph->descent > desc)
				desc = glyph->descent;
			num++;
		}
	}

	if (!num)
		return -EFAULT;

	face->width = width / num;
	face->height = asc + desc;
	log_debug("width/height is %ux%u", face->width, face->height);

	return 0;
}

static int manager_get(struct font_face **out, const struct font_attr *attr,
			bool absolute)
{
	struct font_face *iter;
	int ret;

	manager_lock();

	ret = manager__init();
	if (ret)
		goto unlock;

	for (iter = manager__faces; iter; iter = iter->next) {
		if (attr_equal(&iter->attr, attr)) {
			iter->ref++;
			break;
		}
	}

	if (!iter) {
		ret = face__new(&iter, attr, absolute);
		if (ret)
			goto unlock;

		manager_unlock();
		ret = face_measure(iter);
		manager_lock();
		if (ret) {
			face__free(iter);
			goto unlock;
		}

		manager__add(iter);
	}

unlock:
	manager_unlock();

	if (!ret)
		*out = iter;
	return ret;
}

int font_buffer_new(struct font_buffer **out, unsigned int width,
			unsigned int height)
{
	struct font_buffer *buf;
	int ret;

	if (!out || !width || !height)
		return -EINVAL;

	buf = malloc(sizeof(*buf));
	if (!buf)
		return -ENOMEM;
	memset(buf, 0, sizeof(*buf));
	buf->width = width;
	buf->height = height;
	buf->stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
							width);
	if (buf->stride <= 0) {
		log_err("invalid cairo stride");
		ret = -EFAULT;
		goto err_free;
	}
	buf->data = malloc(buf->stride * buf->height);
	if (!buf->data) {
		ret = -ENOMEM;
		goto err_free;
	}

	*out = buf;
	return 0;

err_free:
	free(buf);
	return ret;
}

void font_buffer_free(struct font_buffer *buf)
{
	if (!buf)
		return;

	free(buf->data);
	free(buf);
}

struct font_screen {
	struct font_buffer *buf;
	struct gl_shader *shader;
	unsigned int tex;

	unsigned int cols;
	unsigned int rows;
	double advance_x;
	double advance_y;

	bool absolute;
	double scale_x;
	double scale_y;

	struct {
		struct font_face *normal;
		struct font_face *bold;
	} faces;

	cairo_surface_t *surface;
	cairo_t *cr;
};

static int screen_new(struct font_screen **out, struct font_buffer *buf,
			const struct font_attr *attr, bool absolute,
			unsigned int cols, unsigned int rows,
			struct gl_shader *shader)
{
	struct font_screen *screen;
	int ret;
	struct font_attr att;

	if (!out || !buf || !attr)
		return -EINVAL;
	if (absolute && (!cols || !rows))
		return -EINVAL;
	if (!buf->width || !buf->height || !buf->stride || !buf->data)
		return -EINVAL;

	log_debug("new screen with size %ux%u for table %ux%u",
			buf->width, buf->height, cols, rows);

	screen = malloc(sizeof(*screen));
	if (!screen)
		return -ENOMEM;
	memset(screen, 0, sizeof(*screen));
	screen->buf = buf;
	screen->shader = shader;
	screen->absolute = absolute;
	attr_cpy(&att, attr, false);
	att.bold = false;
	att.style = FONT_NORMAL;

	screen->surface = cairo_image_surface_create_for_data(
				(uint8_t*)screen->buf->data,
				CAIRO_FORMAT_ARGB32,
				screen->buf->width,
				screen->buf->height,
				screen->buf->stride);
	if (cairo_surface_status(screen->surface) != CAIRO_STATUS_SUCCESS) {
		ret = -EFAULT;
		goto err_surface;
	}

	screen->cr = cairo_create(screen->surface);
	if (cairo_status(screen->cr) != CAIRO_STATUS_SUCCESS) {
		ret = -EFAULT;
		goto err_cr;
	}

	if (screen->absolute) {
		screen->cols = cols;
		screen->rows = rows;
		att.points = screen->buf->height / screen->rows;

		ret = manager_get(&screen->faces.normal, &att, true);
		if (ret)
			goto err_cr;
		att.bold = true;
		ret = manager_get(&screen->faces.bold, &att, true);
		if (ret)
			goto err_normal;

		screen->scale_x = screen->faces.normal->width;
		screen->scale_x *= screen->cols;
		screen->scale_x = screen->buf->width / screen->scale_x;
		screen->scale_y = screen->faces.normal->height;
		screen->scale_y *= screen->rows;
		screen->scale_y = screen->buf->height / screen->scale_y;
	} else {
		ret = manager_get(&screen->faces.normal, &att, false);
		if (ret)
			goto err_cr;
		att.bold = true;
		ret = manager_get(&screen->faces.bold, &att, false);
		if (ret)
			goto err_normal;

		screen->cols = screen->buf->width /
						screen->faces.normal->width;
		screen->rows = screen->buf->height /
						screen->faces.normal->height;
	}

	screen->advance_x = screen->faces.normal->width;
	screen->advance_y = screen->faces.normal->height;

	screen->tex = gl_tex_new();
	gl_shader_ref(screen->shader);
	*out = screen;
	return 0;

err_normal:
	face_unref(screen->faces.normal);
err_cr:
	cairo_destroy(screen->cr);
err_surface:
	cairo_surface_destroy(screen->surface);
	free(screen);
	return ret;
}

int font_screen_new(struct font_screen **out, struct font_buffer *buf,
			const struct font_attr *attr,
			struct gl_shader *shader)
{
	return screen_new(out, buf, attr, false, 0, 0, shader);
}

int font_screen_new_fixed(struct font_screen **out, struct font_buffer *buf,
			const struct font_attr *attr,
			unsigned int cols, unsigned int rows,
			struct gl_shader *shader)
{
	return screen_new(out, buf, attr, true, cols, rows, shader);
}

void font_screen_free(struct font_screen *screen)
{
	if (!screen)
		return;

	log_debug("free screen");
	face_unref(screen->faces.bold);
	face_unref(screen->faces.normal);
	cairo_destroy(screen->cr);
	cairo_surface_destroy(screen->surface);
	gl_tex_free(screen->tex);
	gl_shader_unref(screen->shader);
	free(screen);
}

unsigned int font_screen_columns(struct font_screen *screen)
{
	return screen ? screen->cols : 0;
}

unsigned int font_screen_rows(struct font_screen *screen)
{
	return screen ? screen->rows : 0;
}

unsigned int font_screen_points(struct font_screen *screen)
{
	return screen ? screen->faces.normal->attr.points : 0;
}

unsigned int font_screen_width(struct font_screen *screen)
{
	return screen ? screen->buf->width : 0;
}

unsigned int font_screen_height(struct font_screen *screen)
{
	return screen ? screen->buf->height : 0;
}

int font_screen_draw_start(struct font_screen *screen)
{
	if (!screen)
		return -EINVAL;

	cairo_save(screen->cr);

	cairo_set_operator(screen->cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(screen->cr, 0, 0, 0, 0);
	cairo_paint(screen->cr);

	cairo_set_operator(screen->cr, CAIRO_OPERATOR_OVER);
	cairo_set_source_rgb(screen->cr, 1, 1, 1);

	if (screen->absolute)
		cairo_scale(screen->cr, screen->scale_x, screen->scale_y);

	return 0;
}

int font_screen_draw_char(struct font_screen *screen, kmscon_symbol_t ch,
				unsigned int cellx, unsigned int celly,
				unsigned int width, unsigned int height)
{
	struct font_glyph *glyph;
	int ret;

	if (!screen || !width || !height)
		return -EINVAL;

	ret = face_lookup(screen->faces.normal, &glyph, ch);
	if (ret)
		return ret;

	if (glyph->type == GLYPH_STRING) {
		cairo_move_to(screen->cr, cellx * screen->advance_x,
				celly * screen->advance_y + glyph->ascent);
		pango_cairo_show_glyph_string(screen->cr, glyph->string.font,
						glyph->string.str);
	} else if (glyph->type == GLYPH_LAYOUT) {
		cairo_move_to(screen->cr, cellx * screen->advance_x,
					celly * screen->advance_y);
		pango_cairo_update_layout(screen->cr, glyph->layout);
		pango_cairo_show_layout(screen->cr, glyph->layout);
	}

	return 0;
}

int font_screen_draw_perform(struct font_screen *screen, float *m)
{
	static const float ver[] = { -1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1 };
	static const float tex[] = { 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1 };

	if (!screen)
		return -EINVAL;

	gl_tex_load(screen->tex, screen->buf->width, screen->buf->stride,
			screen->buf->height, screen->buf->data);
	gl_shader_draw_tex(screen->shader, ver, tex, 6, screen->tex, m);
	cairo_restore(screen->cr);

	return 0;
}
