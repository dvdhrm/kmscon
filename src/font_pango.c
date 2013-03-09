/*
 * kmscon - Pango font backend
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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

/**
 * SECTION:font_pango.c
 * @short_description: Pango font backend
 * @include: font.h
 *
 * The pango backend uses pango and freetype2 to render glyphs into memory
 * buffers. It uses a hashmap to cache all rendered glyphs of a single
 * font-face. Therefore, rendering should be very fast. Also, when loading a
 * glyph it pre-renders all common (mostly ASCII) characters, so it can measure
 * the font and return a valid font hight/width.
 *
 * This is a _full_ font backend, that is, it provides every feature you expect
 * from a font renderer. It does glyph substitution if a specific font face does
 * not provide a requested glyph, it does correct font loading, it does
 * italic/bold fonts correctly and more.
 * However, this also means it pulls in a lot of dependencies including glib,
 * pango, freetype2 and more.
 */

#include <errno.h>
#include <glib.h>
#include <pango/pango.h>
#include <pango/pangoft2.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "shl_dlist.h"
#include "shl_hashtable.h"
#include "shl_log.h"
#include "tsm_unicode.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "font_pango"

struct face {
	unsigned long ref;
	struct shl_dlist list;

	struct kmscon_font_attr attr;
	struct kmscon_font_attr real_attr;
	unsigned int baseline;
	PangoContext *ctx;
	pthread_mutex_t glyph_lock;
	struct shl_hashtable *glyphs;
};

static pthread_mutex_t manager_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long manager__refcnt;
static PangoFontMap *manager__lib;
static struct shl_dlist manager__list = SHL_DLIST_INIT(manager__list);

static void manager_lock()
{
	pthread_mutex_lock(&manager_mutex);
}

static void manager_unlock()
{
	pthread_mutex_unlock(&manager_mutex);
}

static int manager__ref()
{
	if (!manager__refcnt++) {
		manager__lib = pango_ft2_font_map_new();
		if (!manager__lib) {
			log_warn("cannot create font map");
			--manager__refcnt;
			return -EFAULT;
		}
	}

	return 0;
}

static void manager__unref()
{
	if (!--manager__refcnt) {
		g_object_unref(manager__lib);
		manager__lib = NULL;
	}
}

static int get_glyph(struct face *face, struct kmscon_glyph **out,
		     uint32_t id, const uint32_t *ch, size_t len)
{
	struct kmscon_glyph *glyph;
	PangoLayout *layout;
	PangoRectangle rec;
	PangoLayoutLine *line;
	FT_Bitmap bitmap;
	unsigned int cwidth;
	size_t ulen, cnt;
	char *val;
	bool res;
	int ret;

	if (!len)
		return -ERANGE;
	cwidth = tsm_ucs4_get_width(*ch);
	if (!cwidth)
		return -ERANGE;

	pthread_mutex_lock(&face->glyph_lock);
	res = shl_hashtable_find(face->glyphs, (void**)&glyph,
				 (void*)(long)id);
	pthread_mutex_unlock(&face->glyph_lock);
	if (res) {
		*out = glyph;
		return 0;
	}

	manager_lock();

	glyph = malloc(sizeof(*glyph));
	if (!glyph) {
		log_error("cannot allocate memory for new glyph");
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(glyph, 0, sizeof(*glyph));
	glyph->width = cwidth;

	layout = pango_layout_new(face->ctx);

	/* render one line only */
	pango_layout_set_height(layout, 0);

	/* no line spacing */
	pango_layout_set_spacing(layout, 0);

	val = tsm_ucs4_to_utf8_alloc(ch, len, &ulen);
	if (!val) {
		ret = -ERANGE;
		goto out_glyph;
	}
	pango_layout_set_text(layout, val, ulen);
	free(val);

	cnt = pango_layout_get_line_count(layout);
	if (cnt == 0) {
		ret = -ERANGE;
		goto out_glyph;
	}

	line = pango_layout_get_line_readonly(layout, 0);

	pango_layout_line_get_pixel_extents(line, NULL, &rec);
	glyph->buf.width = face->real_attr.width * cwidth;
	glyph->buf.height = face->real_attr.height;
	glyph->buf.stride = glyph->buf.width;
	glyph->buf.format = UTERM_FORMAT_GREY;

	if (!glyph->buf.width || !glyph->buf.height) {
		ret = -ERANGE;
		goto out_glyph;
	}

	glyph->buf.data = malloc(glyph->buf.height * glyph->buf.stride);
	if (!glyph->buf.data) {
		log_error("cannot allocate bitmap memory");
		ret = -ENOMEM;
		goto out_glyph;
	}
	memset(glyph->buf.data, 0, glyph->buf.height * glyph->buf.stride);

	bitmap.rows = glyph->buf.height;
	bitmap.width = glyph->buf.width;
	bitmap.pitch = glyph->buf.stride;
	bitmap.num_grays = 256;
	bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
	bitmap.buffer = glyph->buf.data;

	pango_ft2_render_layout_line(&bitmap, line, -rec.x, face->baseline);

	pthread_mutex_lock(&face->glyph_lock);
	ret = shl_hashtable_insert(face->glyphs, (void*)(long)id, glyph);
	pthread_mutex_unlock(&face->glyph_lock);
	if (ret) {
		log_error("cannot add glyph to hashtable");
		goto out_buffer;
	}

	*out = glyph;
	goto out_layout;

out_buffer:
	free(glyph->buf.data);
out_glyph:
	free(glyph);
out_layout:
	g_object_unref(layout);
out_unlock:
	manager_unlock();
	return ret;
}

static void free_glyph(void *data)
{
	struct kmscon_glyph *glyph = data;

	free(glyph->buf.data);
	free(glyph);
}

static int manager_get_face(struct face **out, struct kmscon_font_attr *attr)
{
	struct shl_dlist *iter;
	struct face *face, *f;
	PangoFontDescription *desc;
	PangoLayout *layout;
	PangoRectangle rec;
	int ret, num;
	const char *str;

	manager_lock();

	shl_dlist_for_each(iter, &manager__list) {
		face = shl_dlist_entry(iter, struct face, list);
		if (kmscon_font_attr_match(&face->attr, attr)) {
			++face->ref;
			*out = face;
			ret = 0;
			goto out_unlock;
		}
	}

	ret = manager__ref();
	if (ret)
		goto out_unlock;

	face = malloc(sizeof(*face));
	if (!face) {
		log_error("cannot allocate memory for new face");
		ret = -ENOMEM;
		goto err_manager;
	}
	memset(face, 0, sizeof(*face));
	face->ref = 1;
	memcpy(&face->attr, attr, sizeof(*attr));

	ret = pthread_mutex_init(&face->glyph_lock, NULL);
	if (ret) {
		log_error("cannot initialize glyph lock");
		goto err_free;
	}

	ret = shl_hashtable_new(&face->glyphs, shl_direct_hash,
				shl_direct_equal, NULL, free_glyph);
	if (ret) {
		log_error("cannot allocate hashtable");
		goto err_lock;
	}

	face->ctx = pango_font_map_create_context(manager__lib);
	pango_context_set_base_dir(face->ctx, PANGO_DIRECTION_LTR);
	pango_context_set_language(face->ctx, pango_language_get_default());

	desc = pango_font_description_from_string(attr->name);
	pango_font_description_set_absolute_size(desc,
					PANGO_SCALE * face->attr.height);
	pango_font_description_set_weight(desc,
			attr->bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
	pango_font_description_set_style(desc,
			attr->italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
	pango_font_description_set_variant(desc, PANGO_VARIANT_NORMAL);
	pango_font_description_set_stretch(desc, PANGO_STRETCH_NORMAL);
	pango_font_description_set_gravity(desc, PANGO_GRAVITY_SOUTH);
	pango_context_set_font_description(face->ctx, desc);
	pango_font_description_free(desc);

	/* measure font */
	layout = pango_layout_new(face->ctx);
	pango_layout_set_height(layout, 0);
	pango_layout_set_spacing(layout, 0);
	str = "abcdefghijklmnopqrstuvwxyz"
	      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	      "@!\"$%&/()=?\\}][{°^~+*#'<>|-_.:,;`´";
	num = strlen(str);
	pango_layout_set_text(layout, str, num);
	pango_layout_get_pixel_extents(layout, NULL, &rec);

	memcpy(&face->real_attr, &face->attr, sizeof(face->attr));
	face->real_attr.height = rec.height;
	face->real_attr.width = rec.width / num + 1;
	face->baseline = PANGO_PIXELS_CEIL(pango_layout_get_baseline(layout));
	g_object_unref(layout);

	kmscon_font_attr_normalize(&face->real_attr);
	if (!face->real_attr.height || !face->real_attr.width) {
		log_warning("invalid scaled font sizes");
		ret = -EFAULT;
		goto err_face;
	}

	/* The real metrics probably differ from the requested metrics so try
	 * again to find a suitable cached font. */
	shl_dlist_for_each(iter, &manager__list) {
		f = shl_dlist_entry(iter, struct face, list);
		if (kmscon_font_attr_match(&f->real_attr, &face->real_attr)) {
			++f->ref;
			*out = f;
			ret = 0;
			goto err_face;
		}
	}

	shl_dlist_link(&manager__list, &face->list);
	*out = face;
	ret = 0;
	goto out_unlock;

err_face:
	g_object_unref(face->ctx);
	shl_hashtable_free(face->glyphs);
err_lock:
	pthread_mutex_destroy(&face->glyph_lock);
err_free:
	free(face);
err_manager:
	manager__unref();
out_unlock:
	manager_unlock();
	return ret;
}

static void manager_put_face(struct face *face)
{
	manager_lock();

	if (!--face->ref) {
		shl_dlist_unlink(&face->list);
		shl_hashtable_free(face->glyphs);
		pthread_mutex_destroy(&face->glyph_lock);
		g_object_unref(face->ctx);
		free(face);
		manager__unref();
	}

	manager_unlock();
}

static int kmscon_font_pango_init(struct kmscon_font *out,
				  const struct kmscon_font_attr *attr)
{
	struct face *face = NULL;
	int ret;

	memcpy(&out->attr, attr, sizeof(*attr));
	kmscon_font_attr_normalize(&out->attr);

	log_debug("loading pango font %s", out->attr.name);

	ret = manager_get_face(&face, &out->attr);
	if (ret)
		return ret;
	memcpy(&out->attr, &face->real_attr, sizeof(out->attr));
	out->baseline = face->baseline;

	out->data = face;
	return 0;
}

static void kmscon_font_pango_destroy(struct kmscon_font *font)
{
	struct face *face;

	log_debug("unloading pango font");
	face = font->data;
	manager_put_face(face);
}

static int kmscon_font_pango_render(struct kmscon_font *font, uint32_t id,
				    const uint32_t *ch, size_t len,
				    const struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;
	int ret;

	ret = get_glyph(font->data, &glyph, id, ch, len);
	if (ret)
		return ret;

	*out = glyph;
	return 0;
}

static int kmscon_font_pango_render_empty(struct kmscon_font *font,
					  const struct kmscon_glyph **out)
{
	static const uint32_t empty_char = ' ';
	return kmscon_font_pango_render(font, empty_char, &empty_char, 1, out);
}

static int kmscon_font_pango_render_inval(struct kmscon_font *font,
					  const struct kmscon_glyph **out)
{
	static const uint32_t question_mark = '?';
	return kmscon_font_pango_render(font, question_mark, &question_mark, 1,
					out);
}

struct kmscon_font_ops kmscon_font_pango_ops = {
	.name = "pango",
	.owner = NULL,
	.init = kmscon_font_pango_init,
	.destroy = kmscon_font_pango_destroy,
	.render = kmscon_font_pango_render,
	.render_empty = kmscon_font_pango_render_empty,
	.render_inval = kmscon_font_pango_render_inval,
};
