/*
 * kmscon - Bit-Blitting Text Renderer Backend
 *
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
 * SECTION:text_bblit.c
 * @short_description: Bit-Blitting Text Renderer Backend
 * @include: text.h
 *
 * The bit-blitting renderer requires framebuffer access to the output device
 * and simply blits the glyphs into the buffer.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "text.h"
#include "unicode.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "text_bblit"

static int bblit_set(struct kmscon_text *txt)
{
	unsigned int sw, sh, fw, fh;

	fw = txt->font->attr.width;
	fh = txt->font->attr.height;
	sw = uterm_screen_width(txt->screen);
	sh = uterm_screen_height(txt->screen);

	txt->cols = sw / fw;
	txt->rows = sh / fh;

	return 0;
}

static int bblit_draw(struct kmscon_text *txt, tsm_symbol_t ch,
		      unsigned int posx, unsigned int posy,
		      const struct kmscon_console_attr *attr)
{
	const struct kmscon_glyph *glyph;
	int ret;

	if (ch == 0 || ch == ' ') {
		ret = kmscon_font_render_empty(txt->font, &glyph);
	} else {
		ret = kmscon_font_render(txt->font, ch, &glyph);
	}

	if (ret) {
		ret = kmscon_font_render_inval(txt->font, &glyph);
		if (ret)
			return ret;
	}

	/* draw glyph */
	if (attr->inverse) {
		ret = uterm_screen_blend(txt->screen, &glyph->buf,
					 posx * txt->font->attr.width,
					 posy * txt->font->attr.height,
					 attr->br, attr->bg, attr->bb,
					 attr->fr, attr->fg, attr->fb);
	} else {
		ret = uterm_screen_blend(txt->screen, &glyph->buf,
					 posx * txt->font->attr.width,
					 posy * txt->font->attr.height,
					 attr->fr, attr->fg, attr->fb,
					 attr->br, attr->bg, attr->bb);
	}

	return ret;
}

static const struct kmscon_text_ops kmscon_text_bblit_ops = {
	.name = "bblit",
	.init = NULL,
	.destroy = NULL,
	.set = bblit_set,
	.unset = NULL,
	.prepare = NULL,
	.draw = bblit_draw,
	.render = NULL,
	.abort = NULL,
};

int kmscon_text_bblit_load(void)
{
	int ret;

	ret = kmscon_text_register(&kmscon_text_bblit_ops);
	if (ret) {
		log_error("cannot register bblit renderer");
		return ret;
	}

	return 0;
}

void kmscon_text_bblit_unload(void)
{
	kmscon_text_unregister(kmscon_text_bblit_ops.name);
}
