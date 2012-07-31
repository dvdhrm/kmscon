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
 * TODO
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "log.h"
#include "static_misc.h"
#include "text.h"
#include "unicode.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "text_bblit"

struct bblit {
	unsigned int advance_x;
	unsigned int advance_y;
	unsigned int base;
};

static int bblit_init(struct kmscon_text *txt)
{
	struct bblit *bblit;

	bblit = malloc(sizeof(*bblit));
	if (!bblit)
		return -ENOMEM;
	memset(bblit, 0, sizeof(*bblit));

	txt->data = bblit;
	return 0;
}

static void bblit_destroy(struct kmscon_text *txt)
{
	struct bblit *bblit = txt->data;

	free(bblit);
}

static void bblit_recalculate_size(struct kmscon_text *txt)
{
	unsigned int sw, sh, fw, fh;

	if (!txt->font || !txt->screen)
		return;

	fw = txt->font->attr.width;
	fh = txt->font->attr.height;
	sw = uterm_screen_width(txt->screen);
	sh = uterm_screen_height(txt->screen);

	txt->cols = sw / fw;
	txt->rows = sh / fh;
}

static void bblit_new_font(struct kmscon_text *txt)
{
	bblit_recalculate_size(txt);
}

static void bblit_new_bgcolor(struct kmscon_text *txt)
{
	/* nothing to do here */
}

static void bblit_new_screen(struct kmscon_text *txt)
{
	bblit_recalculate_size(txt);
}

static void bblit_prepare(struct kmscon_text *txt)
{
	int ret;
	unsigned int w, h;
	struct bblit *bblit = txt->data;

	/* clear screen by drawing background */
	w = uterm_screen_width(txt->screen);
	h = uterm_screen_height(txt->screen);
	ret = uterm_screen_fill(txt->screen, txt->bg_r, txt->bg_g, txt->bg_b,
				0, 0, w, h);
	if (ret)
		log_warning("cannot draw screen background");

	/* precalculate values for glyph positioning */
	bblit->advance_x = txt->font->attr.width;
	bblit->advance_y = txt->font->attr.height;
	bblit->base = bblit->advance_y - txt->font->baseline;
}

static void bblit_draw(struct kmscon_text *txt, kmscon_symbol_t ch,
		       unsigned int posx, unsigned int posy,
		       const struct font_char_attr *attr)
{
	const struct kmscon_glyph *glyph;
	int ret;
	struct bblit *bblit = txt->data;
	int off, pos;

	ret = kmscon_font_render(txt->font, ch, &glyph);
	if (ret) {
		/* TODO: document 0 as fallback glyph */
		ret = kmscon_font_render(txt->font, 0, &glyph);
		if (ret)
			return;
	}

	/* position glyph correctly */
	off = bblit->base - glyph->ascent;
	pos = posy * bblit->advance_y;
	if (pos + off < 0)
		pos = 0;
	else
		pos += off;

	/* draw glyph */
	if (attr->inverse) {
		uterm_screen_blend(txt->screen, &glyph->buf,
				   posx * bblit->advance_x,
				   pos,
				   attr->br, attr->bg, attr->bb,
				   attr->fr, attr->fg, attr->fb);
	} else {
		uterm_screen_blend(txt->screen, &glyph->buf,
				   posx * bblit->advance_x,
				   pos,
				   attr->fr, attr->fg, attr->fb,
				   attr->br, attr->bg, attr->bb);
	}
}

static void bblit_render(struct kmscon_text *txt)
{
	/* nothing to do */
}

static const struct kmscon_text_ops kmscon_text_bblit_ops = {
	.name = "bblit",
	.init = bblit_init,
	.destroy = bblit_destroy,
	.new_font = bblit_new_font,
	.new_bgcolor = bblit_new_bgcolor,
	.new_screen = bblit_new_screen,
	.prepare = bblit_prepare,
	.draw = bblit_draw,
	.render = bblit_render,
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
