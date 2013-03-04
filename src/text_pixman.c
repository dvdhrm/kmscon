/*
 * kmscon - Pixman Text Renderer Backend
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

/*
 * Pixman based text renderer
 */

#include <errno.h>
#include <pixman.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_hashtable.h"
#include "shl_log.h"
#include "text.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_pixman"

struct tp_glyph {
	const struct kmscon_glyph *glyph;
	pixman_image_t *surf;
	uint8_t *data;
};

struct tp_pixman {
	pixman_image_t *white;
	struct shl_hashtable *glyphs;
	struct shl_hashtable *bold_glyphs;

	struct uterm_video_buffer buf[2];
	pixman_image_t *surf[2];
	unsigned int format[2];

	bool new_stride;
	bool use_indirect;
	uint8_t *data[2];
	struct uterm_video_buffer vbuf;

	/* cache */
	unsigned int cur;
	unsigned int c_bpp;
	uint32_t *c_data;
	unsigned int c_stride;
};

static int tp_init(struct kmscon_text *txt)
{
	struct tp_pixman *tp;

	tp = malloc(sizeof(*tp));
	if (!tp)
		return -ENOMEM;

	txt->data = tp;
	return 0;
}

static void tp_destroy(struct kmscon_text *txt)
{
	struct tp_pixman *tp = txt->data;

	free(tp);
}

static void free_glyph(void *data)
{
	struct tp_glyph *glyph = data;

	pixman_image_unref(glyph->surf);
	free(glyph->data);
	free(glyph);
}

static unsigned int format_u2p(unsigned int f)
{
	switch (f) {
	case UTERM_FORMAT_XRGB32:
		return PIXMAN_x8r8g8b8;
	case UTERM_FORMAT_RGB16:
		return PIXMAN_r5g6b5;
	case UTERM_FORMAT_GREY:
		return PIXMAN_a8;
	default:
		return 0;
	}
}

static int alloc_indirect(struct kmscon_text *txt,
			  unsigned int w, unsigned int h)
{
	struct tp_pixman *tp = txt->data;
	unsigned int s, i, format;
	int ret;

	log_info("using blitting engine");

	format = format_u2p(UTERM_FORMAT_XRGB32);
	s = w * 4;

	tp->data[0] = malloc(s * h);
	tp->data[1] = malloc(s * h);
	if (!tp->data[0] || !tp->data[1]) {
		log_error("cannot allocate memory for render-buffer");
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < 2; ++i) {
		tp->format[i] = format;
		tp->surf[i] = pixman_image_create_bits(format, w, h,
						       (void*)tp->data[i], s);
		if (!tp->surf[i]) {
			log_error("cannot create pixman surfaces");
			goto err_pixman;
		}
	}

	tp->vbuf.width = w;
	tp->vbuf.height = h;
	tp->vbuf.stride = s;
	tp->vbuf.format = UTERM_FORMAT_XRGB32;
	tp->use_indirect = true;
	return 0;

err_pixman:
	if (tp->surf[1])
		pixman_image_unref(tp->surf[1]);
	tp->surf[1] = NULL;
	if (tp->surf[0])
		pixman_image_unref(tp->surf[0]);
	tp->surf[0] = NULL;
err_free:
	free(tp->data[1]);
	free(tp->data[0]);
	tp->data[1] = NULL;
	tp->data[0] = NULL;
	return ret;
}

static int tp_set(struct kmscon_text *txt)
{
	struct tp_pixman *tp = txt->data;
	int ret;
	unsigned int w, h;
	struct uterm_mode *m;
	pixman_color_t white;

	memset(tp, 0, sizeof(*tp));
	m = uterm_display_get_current(txt->disp);
	w = uterm_mode_get_width(m);
	h = uterm_mode_get_height(m);

	white.red = 0xffff;
	white.green = 0xffff;
	white.blue = 0xffff;
	white.red = 0xffff;

	tp->white = pixman_image_create_solid_fill(&white);
	if (!tp->white) {
		log_error("cannot create pixman solid color buffer");
		return -ENOMEM;
	}

	ret = shl_hashtable_new(&tp->glyphs, shl_direct_hash,
				shl_direct_equal, NULL,
				free_glyph);
	if (ret)
		goto err_white;

	ret = shl_hashtable_new(&tp->bold_glyphs, shl_direct_hash,
				shl_direct_equal, NULL,
				free_glyph);
	if (ret)
		goto err_htable;

	/*
	 * TODO: It is actually faster to use a local shadow buffer and then
	 * blit all data to the framebuffer afterwards. Reads seem to be
	 * horribly slow on some mmap'ed framebuffers. However, that's not true
	 * for all so we actually don't know which to use here.
	 */
	ret = uterm_display_get_buffers(txt->disp, tp->buf,
					UTERM_FORMAT_XRGB32);
	if (ret) {
		log_warning("cannot get buffers for display %p",
			    txt->disp);
		ret = alloc_indirect(txt, w, h);
		if (ret)
			goto err_htable_bold;
	} else {
		tp->format[0] = format_u2p(tp->buf[0].format);
		tp->surf[0] = pixman_image_create_bits_no_clear(tp->format[0],
					tp->buf[0].width, tp->buf[0].height,
					(void*)tp->buf[0].data,
					tp->buf[0].stride);
		tp->format[1] = format_u2p(tp->buf[1].format);
		tp->surf[1] = pixman_image_create_bits_no_clear(tp->format[1],
					tp->buf[1].width, tp->buf[1].height,
					(void*)tp->buf[1].data,
					tp->buf[1].stride);
		if (!tp->surf[0] || !tp->surf[1]) {
			log_error("cannot create pixman surfaces");
			goto err_ctx;
		}
	}

	txt->cols = w / txt->font->attr.width;
	txt->rows = h / txt->font->attr.height;

	return 0;

err_ctx:
	if (tp->surf[1])
		pixman_image_unref(tp->surf[1]);
	if (tp->surf[0])
		pixman_image_unref(tp->surf[0]);
	free(tp->data[1]);
	free(tp->data[0]);
err_htable_bold:
	shl_hashtable_free(tp->bold_glyphs);
err_htable:
	shl_hashtable_free(tp->glyphs);
err_white:
	pixman_image_unref(tp->white);
	return ret;
}

static void tp_unset(struct kmscon_text *txt)
{
	struct tp_pixman *tp = txt->data;

	pixman_image_unref(tp->surf[1]);
	pixman_image_unref(tp->surf[0]);
	free(tp->data[1]);
	free(tp->data[0]);
	shl_hashtable_free(tp->bold_glyphs);
	shl_hashtable_free(tp->glyphs);
	pixman_image_unref(tp->white);
}

static int find_glyph(struct kmscon_text *txt, struct tp_glyph **out,
		      uint32_t id, const uint32_t *ch, size_t len, bool bold)
{
	struct tp_pixman *tp = txt->data;
	struct tp_glyph *glyph;
	struct shl_hashtable *gtable;
	struct kmscon_font *font;
	const struct uterm_video_buffer *buf;
	uint8_t *dst, *src;
	unsigned int format, i;
	int ret, stride;
	bool res;

	if (bold) {
		gtable = tp->bold_glyphs;
		font = txt->bold_font;
	} else {
		gtable = tp->glyphs;
		font = txt->font;
	}

	res = shl_hashtable_find(gtable, (void**)&glyph,
				 (void*)(unsigned long)id);
	if (res) {
		*out = glyph;
		return 0;
	}

	glyph = malloc(sizeof(*glyph));
	if (!glyph)
		return -ENOMEM;
	memset(glyph, 0, sizeof(*glyph));

	if (!len)
		ret = kmscon_font_render_empty(font, &glyph->glyph);
	else
		ret = kmscon_font_render(font, id, ch, len, &glyph->glyph);

	if (ret) {
		ret = kmscon_font_render_inval(font, &glyph->glyph);
		if (ret)
			goto err_free;
	}

	buf = &glyph->glyph->buf;
	stride = buf->stride;
	format = format_u2p(buf->format);
	glyph->surf = pixman_image_create_bits_no_clear(format,
							buf->width,
							buf->height,
							(void*)buf->data,
							buf->stride);
	if (!glyph->surf) {
		stride = (buf->stride + 3) & ~0x3;
		if (!tp->new_stride) {
			tp->new_stride = true;
			log_debug("wrong stride, copy buffer (%d => %d)",
			  buf->stride, stride);
		}

		glyph->data = malloc(stride * buf->height);
		if (!glyph->data) {
			log_error("cannot allocate memory for glyph storage");
			ret = -ENOMEM;
			goto err_free;
		}

		src = buf->data;
		dst = glyph->data;
		for (i = 0; i < buf->height; ++i) {
			memcpy(dst, src, buf->width);
			dst += stride;
			src += buf->stride;
		}

		glyph->surf = pixman_image_create_bits_no_clear(format,
								buf->width,
								buf->height,
								(void*)
								glyph->data,
								stride);
	}
	if (!glyph->surf) {
		log_error("cannot create pixman-glyph: %d %p %d %d %d %d",
			  ret, glyph->data ? glyph->data : buf->data, format,
			  buf->width, buf->height, stride);
		ret = -EFAULT;
		goto err_free;
	}

	ret = shl_hashtable_insert(gtable, (void*)(long)id, glyph);
	if (ret)
		goto err_pixman;

	*out = glyph;
	return 0;

err_pixman:
	pixman_image_unref(glyph->surf);
err_free:
	free(glyph);
	return ret;
}

static int tp_prepare(struct kmscon_text *txt)
{
	struct tp_pixman *tp = txt->data;
	int ret;
	pixman_image_t *img;

	ret = uterm_display_use(txt->disp, NULL);
	if (ret < 0) {
		log_error("cannot use display %p", txt->disp);
		return ret;
	}

	tp->cur = ret;
	img = tp->surf[tp->cur];
	tp->c_bpp = PIXMAN_FORMAT_BPP(tp->format[tp->cur]);
	tp->c_data = pixman_image_get_data(img);
	tp->c_stride = pixman_image_get_stride(img);

	return 0;
}

static int tp_draw(struct kmscon_text *txt,
		   uint32_t id, const uint32_t *ch, size_t len,
		   unsigned int width,
		   unsigned int posx, unsigned int posy,
		   const struct tsm_screen_attr *attr)
{
	struct tp_pixman *tp = txt->data;
	struct tp_glyph *glyph;
	int ret;
	uint32_t bc;
	pixman_color_t fc;
	pixman_image_t *col;

	if (!width)
		return 0;

	ret = find_glyph(txt, &glyph, id, ch, len, attr->bold);
	if (ret)
		return ret;

	if (attr->inverse) {
		bc = (attr->fr << 16) | (attr->fg << 8) | (attr->fb);
		fc.red = attr->br << 8;
		fc.green = attr->bg << 8;
		fc.blue = attr->bb << 8;
		fc.alpha = 0xffff;
	} else {
		bc = (attr->br << 16) | (attr->bg << 8) | (attr->bb);
		fc.red = attr->fr << 8;
		fc.green = attr->fg << 8;
		fc.blue = attr->fb << 8;
		fc.alpha = 0xffff;
	}

	/* TODO: We _really_ should fix pixman to allow something like
	 * pixman_image_set_solid_fill(img, &fc) to avoid allocating a pixman
	 * image for each glyph here.
	 * libc malloc() is pretty fast, but this still costs us a lot of
	 * rendering performance. */
	if (!fc.red && !fc.green && !fc.blue) {
		col = tp->white;
		pixman_image_ref(col);
	} else {
		col = pixman_image_create_solid_fill(&fc);
		if (!col) {
			log_error("cannot create pixman color image");
			return -ENOMEM;
		}
	}

	if (!bc) {
		pixman_image_composite(PIXMAN_OP_SRC,
				       col,
				       glyph->surf,
				       tp->surf[tp->cur],
				       0, 0, 0, 0,
				       posx * txt->font->attr.width,
				       posy * txt->font->attr.height,
				       txt->font->attr.width,
				       txt->font->attr.height);
	} else {
		pixman_fill(tp->c_data, tp->c_stride / 4, tp->c_bpp,
			    posx * txt->font->attr.width,
			    posy * txt->font->attr.height,
			    txt->font->attr.width,
			    txt->font->attr.height,
			    bc);

		pixman_image_composite(PIXMAN_OP_OVER,
				       col,
				       glyph->surf,
				       tp->surf[tp->cur],
				       0, 0, 0, 0,
				       posx * txt->font->attr.width,
				       posy * txt->font->attr.height,
				       txt->font->attr.width,
				       txt->font->attr.height);
	}

	pixman_image_unref(col);

	return 0;
}

static int tp_render(struct kmscon_text *txt)
{
	struct tp_pixman *tp = txt->data;
	int ret;

	if (!tp->use_indirect)
		return 0;

	tp->vbuf.data = tp->data[tp->cur];
	ret = uterm_display_blit(txt->disp, &tp->vbuf, 0, 0);
	if (ret) {
		log_error("cannot blit back-buffer to display: %d", ret);
		return ret;
	}

	return 0;
}

struct kmscon_text_ops kmscon_text_pixman_ops = {
	.name = "pixman",
	.owner = NULL,
	.init = tp_init,
	.destroy = tp_destroy,
	.set = tp_set,
	.unset = tp_unset,
	.prepare = tp_prepare,
	.draw = tp_draw,
	.render = tp_render,
	.abort = NULL,
};
