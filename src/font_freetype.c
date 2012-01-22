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
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "log.h"
#include "unicode.h"

#include <ft2build.h>
#include FT_FREETYPE_H

struct kmscon_font_factory {
	unsigned long ref;
	struct kmscon_symbol_table *st;
	FT_Library lib;
};

struct kmscon_font {
	unsigned long ref;

	struct kmscon_font_factory *ff;
	FT_Face face;
	unsigned int width;
	unsigned int height;
};

struct kmscon_glyph {
	unsigned long ref;
};

int kmscon_font_factory_new(struct kmscon_font_factory **out,
					struct kmscon_symbol_table *st)
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
	ff->st = st;

	err = FT_Init_FreeType(&ff->lib);
	if (err) {
		log_warning("font: cannot initialize FreeType library\n");
		ret = -EFAULT;
		goto err_free;
	}

	kmscon_symbol_table_ref(ff->st);
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

	err = FT_Done_FreeType(ff->lib);
	if (err)
		log_warning("font: cannot deinitialize FreeType library\n");

	kmscon_symbol_table_unref(ff->st);
	free(ff);
}

int kmscon_font_factory_load(struct kmscon_font_factory *ff,
	struct kmscon_font **out, unsigned int width, unsigned int height)
{
	struct kmscon_font *font;
	FT_Error err;
	const char *estr = "unknown error";
	int ret;

	if (!ff || !out)
		return -EINVAL;

	font = malloc(sizeof(*font));
	if (!font)
		return -ENOMEM;

	memset(font, 0, sizeof(*font));
	font->ref = 1;
	font->width = width;
	font->height = height;

	err = FT_New_Face(ff->lib, "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
							0, &font->face);
	if (err) {
		if (err == FT_Err_Unknown_File_Format)
			estr = "unknown file format";

		log_warning("font: cannot load font: %s\n", estr);
		ret = -EFAULT;
		goto err_free;
	}

	if (!font->face->charmap) {
		log_warning("font: cannot load charmap of new font\n");
		ret = -EFAULT;
		goto err_face;
	}

	err = FT_Set_Pixel_Sizes(font->face, width, height);
	if (err) {
		log_warning("font: cannot set pixel size of font\n");
		ret = -EFAULT;
		goto err_face;
	}

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

int kmscon_font_draw(struct kmscon_font *font, kmscon_symbol_t ch,
					void *dcr, uint32_t x, uint32_t y)
{
	if (!font)
		return -EINVAL;

	/* still TODO */

	return 0;
}
