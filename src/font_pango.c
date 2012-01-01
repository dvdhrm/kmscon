/*
 * kmscon - Font Management - Pango backend
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
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
 * Pango Font Management
 * This is the font backend using the pango library in conjunction with cairo as
 * output. See glyph type for detailed information on the caching algorithms
 * used.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <cairo.h>
#include <glib.h>
#include <pango/pango.h>
#include <pango/pangocairo.h>
#include "font.h"
#include "log.h"
#include "unicode.h"

enum glyph_type {
	GLYPH_NONE,
	GLYPH_LAYOUT,
	GLYPH_STR,
};

struct kmscon_glyph {
	size_t ref;
	kmscon_symbol_t ch;
	unsigned int width;

	int type;

	union {
		struct layout {
			PangoLayout *layout;
		} layout;
		struct str {
			PangoFont *font;
			PangoGlyphString *str;
			uint32_t ascent;
		} str;
	} src;
};

struct kmscon_font_factory {
	unsigned long ref;
	struct kmscon_symbol_table *st;
};

struct kmscon_font {
	size_t ref;
	struct kmscon_symbol_table *st;

	unsigned int width;
	unsigned int height;
	GHashTable *glyphs;
	PangoContext *ctx;
};

static int kmscon_font_lookup(struct kmscon_font *font,
			kmscon_symbol_t key, struct kmscon_glyph **out);

/*
 * Glyphs
 * Glyphs are for internal use only! The outside world uses kmscon_char
 * objects in combination with kmscon_font to draw characters. Internally, we
 * cache a kmscon_glyph for every character that is drawn.
 * This allows us to speed up the drawing operations because most characters are
 * already cached.
 *
 * Glyphs are cached in a hash-table by each font. If a character is drawn, we
 * look it up in the hash-table (or create a new one if none is found) and draw
 * it to the framebuffer.
 * A glyph may use several ways to cache the glyph description:
 *   GLYPH_NONE:
 *     No information is currently attached so the glyph cannot be drawn.
 *   GLYPH_LAYOUT:
 *     The most basic drawing operation. This is the slowest of all but can draw
 *     any text you want. It uses a PangoLayout internally and recalculates the
 *     character sizes each time we draw them.
 */
static int kmscon_glyph_new(struct kmscon_glyph **out, kmscon_symbol_t ch)
{
	struct kmscon_glyph *glyph;

	if (!out)
		return -EINVAL;

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;

	memset(glyph, 0, sizeof(*glyph));
	glyph->ref = 1;
	glyph->type = GLYPH_NONE;
	glyph->ch = ch;

	*out = glyph;
	return 0;
}

/*
 * Reset internal glyph description. You must use kmscon_glyph_set() again to
 * attach new glyph descriptions.
 */
static void kmscon_glyph_reset(struct kmscon_glyph *glyph)
{
	if (!glyph)
		return;

	switch (glyph->type) {
	case GLYPH_LAYOUT:
		g_object_unref(glyph->src.layout.layout);
		break;
	case GLYPH_STR:
		g_object_unref(glyph->src.str.font);
		pango_glyph_string_free(glyph->src.str.str);
		break;
	}

	glyph->type = GLYPH_NONE;
	glyph->width = 0;
}

static void kmscon_glyph_ref(struct kmscon_glyph *glyph)
{
	if (!glyph)
		return;

	++glyph->ref;
}

static void kmscon_glyph_unref(struct kmscon_glyph *glyph)
{
	if (!glyph || !glyph->ref)
		return;

	if (--glyph->ref)
		return;

	kmscon_glyph_reset(glyph);
	free(glyph);
}

/*
 * Generate glyph description.
 * This connects the glyph with the given font an generates the fastest glyph
 * description.
 * Returns 0 on success.
 */
static int kmscon_glyph_set(struct kmscon_glyph *glyph,
						struct kmscon_font *font)
{
	PangoLayout *layout;
	PangoLayoutLine *line;
	PangoGlyphItem *tmp;
	PangoGlyphString *str;
	PangoRectangle rec;
	size_t len;
	const char *val;

	if (!glyph || !font)
		return -EINVAL;

	layout = pango_layout_new(font->ctx);
	if (!layout)
		return -EINVAL;

	val = kmscon_symbol_get_u8(font->st, glyph->ch, &len);
	pango_layout_set_text(layout, val, len);
	kmscon_symbol_free_u8(val);

	pango_layout_get_extents(layout, NULL, &rec);
	line = pango_layout_get_line_readonly(layout, 0);

	if (!line || !line->runs || line->runs->next) {
		kmscon_glyph_reset(glyph);
		glyph->type = GLYPH_LAYOUT;
		glyph->src.layout.layout = layout;
	} else {
		tmp = line->runs->data;
		str = pango_glyph_string_copy(tmp->glyphs);
		if (!str) {
			g_object_unref(layout);
			return -ENOMEM;
		}

		kmscon_glyph_reset(glyph);
		glyph->type = GLYPH_STR;

		glyph->src.str.str = str;
		glyph->src.str.font =
			g_object_ref(tmp->item->analysis.font);
		glyph->src.str.ascent =
			PANGO_PIXELS_CEIL(pango_layout_get_baseline(layout));

		g_object_unref(layout);
	}

	glyph->width = PANGO_PIXELS(rec.width);
	return 0;
}

int kmscon_font_factory_new(struct kmscon_font_factory **out,
					struct kmscon_symbol_table *st)
{
	struct kmscon_font_factory *ff;

	if (!out)
		return -EINVAL;

	ff = malloc(sizeof(*ff));
	if (!ff)
		return -ENOMEM;

	memset(ff, 0, sizeof(*ff));
	ff->ref = 1;
	ff->st = st;

	kmscon_symbol_table_ref(ff->st);
	*out = ff;

	return 0;
}

void kmscon_font_factory_ref(struct kmscon_font_factory *ff)
{
	if (!ff)
		return;

	++ff->ref;
}

void kmscon_font_factory_unref(struct kmscon_font_factory *ff)
{
	if (!ff || !ff->ref)
		return;

	if (--ff->ref)
		return;

	kmscon_symbol_table_unref(ff->st);
	free(ff);
}

/*
 * Measure font width
 * We simply draw all ASCII characters and use the average width as default
 * character width.
 * This has the side effect that all ASCII characters are already cached and the
 * console will speed up.
 */
static int measure_width(struct kmscon_font *font)
{
	unsigned int i, num, width;
	int ret;
	kmscon_symbol_t ch;
	struct kmscon_glyph *glyph;

	if (!font)
		return -EINVAL;

	num = 0;
	for (i = 0; i < 127; ++i) {
		ch = kmscon_symbol_make(i);

		ret = kmscon_font_lookup(font, ch, &glyph);
		if (ret)
			continue;

		if (glyph->width > 0) {
			width += glyph->width;
			num++;
		}

		kmscon_glyph_unref(glyph);
	}

	if (!num)
		return -EFAULT;

	font->width = width / num;
	log_debug("font: width is %u\n", font->width);

	return 0;
}

/*
 * Creates a new font
 * \height is the height in pixel that we have for each character.
 * Returns 0 on success and stores the new font in \out.
 */

int kmscon_font_factory_load(struct kmscon_font_factory *ff,
	struct kmscon_font **out, unsigned int width, unsigned int height)
{
	struct kmscon_font *font;
	int ret;
	PangoFontDescription *desc;
	PangoFontMap *map;
	PangoLanguage *lang;
	cairo_font_options_t *opt;

	if (!ff || !out || !height)
		return -EINVAL;

	log_debug("font: new font (height %u)\n", height);

	font = malloc(sizeof(*font));
	if (!font)
		return -ENOMEM;
	font->ref = 1;
	font->height = height;
	font->st = ff->st;

	map = pango_cairo_font_map_get_default();
	if (!map) {
		ret = -EFAULT;
		goto err_free;
	}

	font->ctx = pango_font_map_create_context(map);
	if (!font->ctx) {
		ret = -EFAULT;
		goto err_free;
	}

	pango_context_set_base_dir(font->ctx, PANGO_DIRECTION_LTR);

	desc = pango_font_description_from_string("monospace");
	if (!desc) {
		ret = -EFAULT;
		goto err_ctx;
	}

	pango_font_description_set_absolute_size(desc, PANGO_SCALE * height);
	pango_context_set_font_description(font->ctx, desc);
	pango_font_description_free(desc);

	lang = pango_language_get_default();
	if (!lang) {
		ret = -EFAULT;
		goto err_ctx;
	}

	pango_context_set_language(font->ctx, lang);

	if (!pango_cairo_context_get_font_options(font->ctx)) {
		opt = cairo_font_options_create();
		if (!opt) {
			ret = -EFAULT;
			goto err_ctx;
		}

		pango_cairo_context_set_font_options(font->ctx, opt);
		cairo_font_options_destroy(opt);
	}

	font->glyphs = g_hash_table_new_full(g_direct_hash, g_direct_equal,
				NULL, (GDestroyNotify) kmscon_glyph_unref);
	if (!font->glyphs) {
		ret = -ENOMEM;
		goto err_ctx;
	}

	ret = measure_width(font);
	if (ret)
		goto err_hash;

	kmscon_symbol_table_ref(font->st);
	*out = font;

	return 0;

err_hash:
	g_hash_table_unref(font->glyphs);
err_ctx:
	g_object_unref(font->ctx);
err_free:
	free(font);
	return ret;
}

void kmscon_font_ref(struct kmscon_font *font)
{
	if (!font)
		return;

	++font->ref;
}

void kmscon_font_unref(struct kmscon_font *font)
{
	if (!font || !font->ref)
		return;

	if (--font->ref)
		return;

	g_hash_table_unref(font->glyphs);
	g_object_unref(font->ctx);
	kmscon_symbol_table_unref(font->st);
	free(font);
	log_debug("font: destroying font\n");
}

unsigned int kmscon_font_get_width(struct kmscon_font *font)
{
	if (!font)
		return 0;

	return font->width;
}

unsigned int kmscon_font_get_height(struct kmscon_font *font)
{
	if (!font)
		return 0;

	return font->height;
}

/*
 * Get glyph for given key. If no glyph can be found in the hash-table, then a
 * new glyph is created and added to the hash-table.
 * Returns 0 on success and stores the glyph with a new reference in \out.
 */
static int kmscon_font_lookup(struct kmscon_font *font,
			kmscon_symbol_t key, struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;
	int ret;

	if (!font || !out)
		return -EINVAL;

	glyph = g_hash_table_lookup(font->glyphs, GUINT_TO_POINTER(key));
	if (!glyph) {
		ret = kmscon_glyph_new(&glyph, key);
		if (ret)
			return ret;

		ret = kmscon_glyph_set(glyph, font);
		if (ret)
			goto err_glyph;

		g_hash_table_insert(font->glyphs, GUINT_TO_POINTER(key), glyph);
	}

	kmscon_glyph_ref(glyph);
	*out = glyph;
	return 0;

err_glyph:
	kmscon_glyph_unref(glyph);
	return ret;
}

/*
 * This draws a glyph for characters \ch into the given cairo context \cr.
 * The glyph will be drawn with the upper-left corner at x/y.
 * Returns 0 on success.
 */
int kmscon_font_draw(struct kmscon_font *font, kmscon_symbol_t ch, void *dcr,
							uint32_t x, uint32_t y)
{
	struct kmscon_glyph *glyph;
	int ret;
	cairo_t *cr = dcr;

	if (!font || !ch || !cr)
		return -EINVAL;

	ret = kmscon_font_lookup(font, ch, &glyph);
	if (ret)
		return ret;

	switch (glyph->type) {
	case GLYPH_LAYOUT:
		cairo_move_to(cr, x, y);
		pango_cairo_update_layout(cr, glyph->src.layout.layout);
		pango_cairo_show_layout(cr, glyph->src.layout.layout);
		break;
	case GLYPH_STR:
		cairo_move_to(cr, x, y + glyph->src.str.ascent);
		pango_cairo_show_glyph_string(cr, glyph->src.str.font,
							glyph->src.str.str);
		break;
	default:
		ret = -EFAULT;
		break;
	}

	kmscon_glyph_unref(glyph);

	return 0;
}
