/*
 * kmscon - Bit-Blitting Bulk Text Renderer Backend
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
 * SECTION:text_bbulk.c
 * @short_description: Bit-Blitting Bulk Text Renderer Backend
 * @include: text.h
 *
 * Similar to the bblit renderer but assembles an array of blit-requests and
 * pushes all of them at once to the video device.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_log.h"
#include "text.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_bbulk"

struct bbulk {
	struct uterm_video_blend_req *reqs;
};

#define FONT_WIDTH(txt) ((txt)->font->attr.width)
#define FONT_HEIGHT(txt) ((txt)->font->attr.height)

static int bbulk_init(struct kmscon_text *txt)
{
	struct bbulk *bb;

	bb = malloc(sizeof(*bb));
	if (!bb)
		return -ENOMEM;

	txt->data = bb;
	return 0;
}

static void bbulk_destroy(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	free(bb);
}

static int bbulk_set(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;
	unsigned int sw, sh, i, j;
	struct uterm_video_blend_req *req;
	struct uterm_mode *mode;

	memset(bb, 0, sizeof(*bb));

	mode = uterm_display_get_current(txt->disp);
	if (!mode)
		return -EINVAL;
	sw = uterm_mode_get_width(mode);
	sh = uterm_mode_get_height(mode);

	txt->cols = sw / FONT_WIDTH(txt);
	txt->rows = sh / FONT_HEIGHT(txt);

	bb->reqs = malloc(sizeof(*bb->reqs) * txt->cols * txt->rows);
	if (!bb->reqs)
		return -ENOMEM;
	memset(bb->reqs, 0, sizeof(*bb->reqs) * txt->cols * txt->rows);

	for (i = 0; i < txt->rows; ++i) {
		for (j = 0; j < txt->cols; ++j) {
			req = &bb->reqs[i * txt->cols + j];
			req->x = j * FONT_WIDTH(txt);
			req->y = i * FONT_HEIGHT(txt);
		}
	}

	return 0;
}

static void bbulk_unset(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	free(bb->reqs);
	bb->reqs = NULL;
}

static int bbulk_draw(struct kmscon_text *txt,
		      uint32_t id, const uint32_t *ch, size_t len,
		      unsigned int width,
		      unsigned int posx, unsigned int posy,
		      const struct tsm_screen_attr *attr)
{
	struct bbulk *bb = txt->data;
	const struct kmscon_glyph *glyph;
	int ret;
	struct uterm_video_blend_req *req;
	struct kmscon_font *font;

	if (!width) {
		bb->reqs[posy * txt->cols + posx].buf = NULL;
		return 0;
	}

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

	req = &bb->reqs[posy * txt->cols + posx];
	req->buf = &glyph->buf;
	if (attr->inverse) {
		req->fr = attr->br;
		req->fg = attr->bg;
		req->fb = attr->bb;
		req->br = attr->fr;
		req->bg = attr->fg;
		req->bb = attr->fb;
	} else {
		req->fr = attr->fr;
		req->fg = attr->fg;
		req->fb = attr->fb;
		req->br = attr->br;
		req->bg = attr->bg;
		req->bb = attr->bb;
	}

	return 0;
}

static int bbulk_render(struct kmscon_text *txt)
{
	struct bbulk *bb = txt->data;

	return uterm_display_fake_blendv(txt->disp, bb->reqs,
					 txt->cols * txt->rows);
}

struct kmscon_text_ops kmscon_text_bbulk_ops = {
	.name = "bbulk",
	.owner = NULL,
	.init = bbulk_init,
	.destroy = bbulk_destroy,
	.set = bbulk_set,
	.unset = bbulk_unset,
	.prepare = NULL,
	.draw = bbulk_draw,
	.render = bbulk_render,
	.abort = NULL,
};
