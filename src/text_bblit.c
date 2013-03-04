/*
 * kmscon - Bit-Blitting Text Renderer Backend
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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
#include "shl_log.h"
#include "text.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_bblit"

static int bblit_set(struct kmscon_text *txt)
{
	unsigned int sw, sh, fw, fh;
	struct uterm_mode *mode;

	fw = txt->font->attr.width;
	fh = txt->font->attr.height;
	mode = uterm_display_get_current(txt->disp);
	if (!mode)
		return -EINVAL;
	sw = uterm_mode_get_width(mode);
	sh = uterm_mode_get_height(mode);

	txt->cols = sw / fw;
	txt->rows = sh / fh;

	return 0;
}

static int bblit_draw(struct kmscon_text *txt,
		      uint32_t id, const uint32_t *ch, size_t len,
		      unsigned int width,
		      unsigned int posx, unsigned int posy,
		      const struct tsm_screen_attr *attr)
{
	const struct kmscon_glyph *glyph;
	int ret;
	struct kmscon_font *font;

	if (!width)
		return 0;

	if (attr->bold)
		font = txt->bold_font;
	else
		font = txt->font;

	if (!len) {
		ret = kmscon_font_render_empty(font, &glyph);
	} else {
		ret = kmscon_font_render(font, id, ch, len, &glyph);
	}

	if (ret) {
		ret = kmscon_font_render_inval(font, &glyph);
		if (ret)
			return ret;
	}

	/* draw glyph */
	if (attr->inverse) {
		ret = uterm_display_fake_blend(txt->disp, &glyph->buf,
					       posx * txt->font->attr.width,
					       posy * txt->font->attr.height,
					       attr->br, attr->bg, attr->bb,
					       attr->fr, attr->fg, attr->fb);
	} else {
		ret = uterm_display_fake_blend(txt->disp, &glyph->buf,
					       posx * txt->font->attr.width,
					       posy * txt->font->attr.height,
					       attr->fr, attr->fg, attr->fb,
					       attr->br, attr->bg, attr->bb);
	}

	return ret;
}

struct kmscon_text_ops kmscon_text_bblit_ops = {
	.name = "bblit",
	.owner = NULL,
	.init = NULL,
	.destroy = NULL,
	.set = bblit_set,
	.unset = NULL,
	.prepare = NULL,
	.draw = bblit_draw,
	.render = NULL,
	.abort = NULL,
};
