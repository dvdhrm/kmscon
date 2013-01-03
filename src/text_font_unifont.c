/*
 * kmscon - Fixed unifont font for font handling of text renderer
 *
 * Copyright (c) 2012 Ted Kotz <ted@kotz.us>
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * SECTION:text_font_unifont.c
 * @short_description: Fixed unifont font for font handling of text renderer
 * @include: text.h
 * 
 * This is a fixed font renderer backend that supports just one font which is
 * statically compiled into the file. This bitmap font has 8x16 and 16x16 
 * glyphs. This can statically compile in any font defined as a unifont style
 * hex format. This font is from the GNU unifont project available at: 
 * http://unifoundry.com/unifont.html
 *
 * This file is heavily based on text_font_8x16.c
 * 
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "text.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "text_font_unifont"

/* array is generated and compiled externally */
extern const struct kmscon_glyph kmscon_text_font_unifont_data_hex_glyphs[];
extern size_t kmscon_text_font_unifont_data_hex_len;

static int kmscon_font_unifont_init(struct kmscon_font *out,
				    const struct kmscon_font_attr *attr)
{
	static const char name[] = "static-unifont";

	log_debug("loading static unifont font");

	memset(&out->attr, 0, sizeof(out->attr));
	memcpy(out->attr.name, name, sizeof(name));
	out->attr.bold = false;
	out->attr.italic = false;
	out->attr.width = 8;
	out->attr.height = 16;
	kmscon_font_attr_normalize(&out->attr);
	out->baseline = 4;

	return 0;
}

static void kmscon_font_unifont_destroy(struct kmscon_font *font)
{
	log_debug("unloading static unifont font");
}

static int kmscon_font_unifont_render(struct kmscon_font *font, uint32_t id,
				      const uint32_t *ch, size_t len,
				      const struct kmscon_glyph **out)
{
	if (len > 1 || *ch >= kmscon_text_font_unifont_data_hex_len)
		return -ERANGE;

	*out = &kmscon_text_font_unifont_data_hex_glyphs[*ch];
	return 0;
}

static int kmscon_font_unifont_render_inval(struct kmscon_font *font,
					    const struct kmscon_glyph **out)
{
	if (0xfffd < kmscon_text_font_unifont_data_hex_len)
		*out = &kmscon_text_font_unifont_data_hex_glyphs[0xfffd];
	else if ('?' < kmscon_text_font_unifont_data_hex_len)
		*out = &kmscon_text_font_unifont_data_hex_glyphs['?'];
	else
		*out = &kmscon_text_font_unifont_data_hex_glyphs[0];

	return 0;
}

static int kmscon_font_unifont_render_empty(struct kmscon_font *font,
					    const struct kmscon_glyph **out)
{
	if (' ' < kmscon_text_font_unifont_data_hex_len) {
		*out = &kmscon_text_font_unifont_data_hex_glyphs[' '];
		return 0;
	} else {
		return kmscon_font_unifont_render_inval(font, out);
	}
}

struct kmscon_font_ops kmscon_font_unifont_ops = {
	.name = "unifont",
	.init = kmscon_font_unifont_init,
	.destroy = kmscon_font_unifont_destroy,
	.render = kmscon_font_unifont_render,
	.render_empty = kmscon_font_unifont_render_empty,
	.render_inval = kmscon_font_unifont_render_inval,
	.finalize = NULL,
};
