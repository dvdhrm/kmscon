/*
 * kmscon - Font Handling - FreeType2 Backend
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
 * Font Handling - FreeType2
 * This provides a font backend based on FreeType2 library. This is inferior to
 * the pango backend as it does not handle combined characters. However, it
 * pulls in a lot less dependencies so may be prefered on some systems.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "gl.h"
#include "log.h"
#include "misc.h"
#include "unicode.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#define LOG_SUBSYSTEM "font_freetype2"

struct kmscon_font_factory {
	unsigned long ref;
	FT_Library lib;
};

struct kmscon_font {
	unsigned long ref;

	struct kmscon_font_factory *ff;
	FT_Face face;
	unsigned int width;
	unsigned int height;
	struct kmscon_hashtable *glyphs;
};

struct kmscon_glyph {
	bool valid;
	unsigned int tex;
	unsigned int width;
	unsigned int height;
	int left;
	int top;
	unsigned int advance;
};

static int kmscon_glyph_new(struct kmscon_glyph **out, kmscon_symbol_t key,
						struct kmscon_font *font)
{
	struct kmscon_glyph *glyph;
	FT_Error err;
	FT_UInt idx;
	FT_Bitmap *bmap;
	int ret;
	const uint32_t *val;
	size_t len;
	unsigned char *data, d;
	int i, j;

	if (!out)
		return -EINVAL;

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;

	memset(glyph, 0, sizeof(*glyph));

	val = kmscon_symbol_get(&key, &len);

	if (!val[0])
		goto ready;

	/* TODO: Add support for combining characters */
	idx = FT_Get_Char_Index(font->face, val[0]);
	err = FT_Load_Glyph(font->face, idx, FT_LOAD_DEFAULT);
	if (err) {
		ret = -EFAULT;
		goto err_free;
	}

	err = FT_Render_Glyph(font->face->glyph, FT_RENDER_MODE_NORMAL);
	if (err) {
		ret = -EFAULT;
		goto err_free;
	}

	bmap = &font->face->glyph->bitmap;
	if (!bmap->width || !bmap->rows)
		goto ready;

	glyph->tex = gl_tex_new();
	data = malloc(sizeof(unsigned char) * bmap->width * bmap->rows * 4);
	if (!data) {
		ret = -ENOMEM;
		goto err_tex;
	}

	for (j = 0; j < bmap->rows; ++j) {
		for (i = 0; i < bmap->width; ++i) {
			d = bmap->buffer[i + bmap->width * j];
			data[4 * (i + j * bmap->width)] = d;
			data[4 * (i + j * bmap->width) + 1] = d;
			data[4 * (i + j * bmap->width) + 2] = d;
			data[4 * (i + j * bmap->width) + 3] = d;
		}
	}

	gl_tex_load(glyph->tex, bmap->width, 0, bmap->rows, data);
	free(data);

	glyph->width = bmap->width;
	glyph->height = bmap->rows;
	glyph->left = font->face->glyph->bitmap_left;
	glyph->top = font->face->glyph->bitmap_top;
	glyph->advance = font->face->glyph->advance.x >> 6;
	glyph->valid = true;

ready:
	*out = glyph;
	return 0;

err_tex:
	gl_tex_free(glyph->tex);
err_free:
	free(glyph);
	return ret;
}

static void kmscon_glyph_destroy(struct kmscon_glyph *glyph)
{
	if (!glyph)
		return;

	if (glyph->valid)
		gl_tex_free(glyph->tex);
	free(glyph);
}

int kmscon_font_factory_new(struct kmscon_font_factory **out)
{
	struct kmscon_font_factory *ff;
	FT_Error err;
	int ret;

	if (!out)
		return -EINVAL;

	ff = malloc(sizeof(*ff));
	if (!ff)
		return -ENOMEM;

	memset(ff, 0, sizeof(*ff));
	ff->ref = 1;

	err = FT_Init_FreeType(&ff->lib);
	if (err) {
		log_warn("cannot initialize FreeType library");
		ret = -EFAULT;
		goto err_free;
	}

	log_debug("new font factory");
	*out = ff;

	return 0;

err_free:
	free(ff);
	return ret;
}

void kmscon_font_factory_ref(struct kmscon_font_factory *ff)
{
	if (!ff)
		return;

	++ff->ref;
}

void kmscon_font_factory_unref(struct kmscon_font_factory *ff)
{
	FT_Error err;

	if (!ff || !ff->ref)
		return;

	if (--ff->ref)
		return;

	log_debug("destroying font factory");

	err = FT_Done_FreeType(ff->lib);
	if (err)
		log_warn("cannot deinitialize FreeType library");

	free(ff);
}

int kmscon_font_factory_load(struct kmscon_font_factory *ff,
	struct kmscon_font **out, unsigned int width, unsigned int height)
{
	struct kmscon_font *font;
	FT_Error err;
	const char *estr = "unknown error";
	int ret;
	const char *path = "./fonts/DejaVuSansMono.ttf";

	if (!ff || !out || !height)
		return -EINVAL;

	if (!width)
		width = height;

	log_debug("loading new font %s", path);

	font = malloc(sizeof(*font));
	if (!font)
		return -ENOMEM;

	memset(font, 0, sizeof(*font));
	font->ref = 1;
	font->width = width;
	font->height = height;

	/* TODO: Use fontconfig to get font paths */
	err = FT_New_Face(ff->lib, path, 0, &font->face);
	if (err) {
		if (err == FT_Err_Unknown_File_Format)
			estr = "unknown file format";

		log_warn("cannot load font: %s", estr);
		ret = -EFAULT;
		goto err_free;
	}

	if (!font->face->charmap) {
		log_warn("cannot load charmap of new font");
		ret = -EFAULT;
		goto err_face;
	}

	err = FT_Set_Pixel_Sizes(font->face, width, height);
	if (err) {
		log_warn("cannot set pixel size of font");
		ret = -EFAULT;
		goto err_face;
	}

	ret = kmscon_hashtable_new(&font->glyphs, kmscon_direct_hash,
				kmscon_direct_equal, NULL,
				(kmscon_free_cb)kmscon_glyph_destroy);
	if (ret)
		goto err_face;

	kmscon_font_factory_ref(ff);
	font->ff = ff;
	*out = font;

	return 0;

err_face:
	FT_Done_Face(font->face);
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

	log_debug("destroying font");

	kmscon_hashtable_free(font->glyphs);
	FT_Done_Face(font->face);
	kmscon_font_factory_unref(font->ff);
	free(font);
}

unsigned int kmscon_font_get_height(struct kmscon_font *font)
{
	if (!font)
		return 0;

	return font->height;
}

unsigned int kmscon_font_get_width(struct kmscon_font *font)
{
	if (!font)
		return 0;

	return font->width;
}

static int kmscon_font_lookup(struct kmscon_font *font,
			kmscon_symbol_t key, struct kmscon_glyph **out)
{
	struct kmscon_glyph *glyph;
	int ret;
	bool res;

	if (!font || !out)
		return -EINVAL;

	res = kmscon_hashtable_find(font->glyphs, (void**)&glyph,
					(void*)(long)key);
	if (!res) {
		ret = kmscon_glyph_new(&glyph, key, font);
		if (ret)
			return ret;

		kmscon_hashtable_insert(font->glyphs, (void*)(long)key, glyph);
	}

	*out = glyph;
	return 0;
}

int kmscon_font_draw(struct kmscon_font *font, kmscon_symbol_t ch, float *m,
			struct gl_shader *shader)
{
	int ret;
	struct kmscon_glyph *glyph;
	static const float val[] = { 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1 };

	if (!font)
		return -EINVAL;

	ret = kmscon_font_lookup(font, ch, &glyph);
	if (ret)
		return ret;

	if (!glyph->valid)
		return 0;

	gl_m4_scale(m, 1.0 / glyph->advance, 1.0 / font->height, 1);
	gl_m4_translate(m, glyph->left, font->height - glyph->top, 0);
	gl_m4_scale(m, glyph->width, glyph->height, 1);

	gl_shader_draw_tex(shader, val, val, 6, glyph->tex, m);

	return 0;
}
