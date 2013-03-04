/*
 * kmscon - Cairo Text Renderer Backend
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
 * Cairo based text renderer
 */

#include <cairo.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_hashtable.h"
#include "shl_log.h"
#include "text.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "text_cairo"

struct tc_glyph {
	const struct kmscon_glyph *glyph;
	cairo_surface_t *surf;
	uint8_t *data;
};

struct tc_cairo {
	struct shl_hashtable *glyphs;
	struct shl_hashtable *bold_glyphs;

	bool new_stride;
	unsigned int cur;
	struct uterm_video_buffer buf[2];
	cairo_surface_t *surf[2];
	cairo_t *ctx[2];

	bool use_indirect;
	uint8_t *data[2];
	struct uterm_video_buffer vbuf;
};

static int tc_init(struct kmscon_text *txt)
{
	struct tc_cairo *tc;

	tc = malloc(sizeof(*tc));
	if (!tc)
		return -ENOMEM;

	txt->data = tc;
	return 0;
}

static void tc_destroy(struct kmscon_text *txt)
{
	struct tc_cairo *tc = txt->data;

	free(tc);
}

static void free_glyph(void *data)
{
	struct tc_glyph *glyph = data;

	cairo_surface_destroy(glyph->surf);
	free(glyph->data);
	free(glyph);
}

static unsigned int format_u2c(unsigned int f)
{
	switch (f) {
	case UTERM_FORMAT_XRGB32:
		return CAIRO_FORMAT_ARGB32;
	case UTERM_FORMAT_RGB16:
		return CAIRO_FORMAT_RGB16_565;
	case UTERM_FORMAT_GREY:
		return CAIRO_FORMAT_A8;
	default:
		return CAIRO_FORMAT_INVALID;
	}
}

static int alloc_indirect(struct kmscon_text *txt,
			  unsigned int w, unsigned int h)
{
	struct tc_cairo *tc = txt->data;
	unsigned int s, i, format;
	int ret;

	log_info("using blitting engine");

	format = format_u2c(UTERM_FORMAT_XRGB32);
	s = cairo_format_stride_for_width(format, w);

	tc->data[0] = malloc(s * h);
	tc->data[1] = malloc(s * h);
	if (!tc->data[0] || !tc->data[1]) {
		log_error("cannot allocate memory for render-buffer");
		ret = -ENOMEM;
		goto err_free;
	}

	for (i = 0; i < 2; ++i) {
		tc->surf[i] = cairo_image_surface_create_for_data(tc->data[i],
								  format,
								  w, h, s);

		ret = cairo_surface_status(tc->surf[i]);
		if (ret != CAIRO_STATUS_SUCCESS) {
			log_error("cannot create cairo surface: %d", ret);
			goto err_cairo;
		}
	}

	tc->vbuf.width = w;
	tc->vbuf.height = h;
	tc->vbuf.stride = s;
	tc->vbuf.format = UTERM_FORMAT_XRGB32;
	tc->use_indirect = true;
	return 0;

err_cairo:
	cairo_surface_destroy(tc->surf[1]);
	cairo_surface_destroy(tc->surf[0]);
err_free:
	free(tc->data[1]);
	free(tc->data[0]);
	tc->data[1] = NULL;
	tc->data[0] = NULL;
	return ret;
}

static int tc_set(struct kmscon_text *txt)
{
	struct tc_cairo *tc = txt->data;
	int ret, ret2;
	unsigned int format, w, h;
	struct uterm_mode *m;

	memset(tc, 0, sizeof(*tc));
	m = uterm_display_get_current(txt->disp);
	w = uterm_mode_get_width(m);
	h = uterm_mode_get_height(m);

	ret = shl_hashtable_new(&tc->glyphs, shl_direct_hash,
				shl_direct_equal, NULL,
				free_glyph);
	if (ret)
		return ret;

	ret = shl_hashtable_new(&tc->bold_glyphs, shl_direct_hash,
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
	ret = uterm_display_get_buffers(txt->disp, tc->buf,
					UTERM_FORMAT_XRGB32 |
					UTERM_FORMAT_RGB16);
	if (ret) {
		log_warning("cannot get buffers for display %p",
			    txt->disp);
		ret = alloc_indirect(txt, w, h);
		if (ret)
			goto err_htable_bold;
	} else {
		format = format_u2c(tc->buf[0].format);
		tc->surf[0] = cairo_image_surface_create_for_data(
							tc->buf[0].data,
							format,
							tc->buf[0].width,
							tc->buf[0].height,
							tc->buf[0].stride);
		format = format_u2c(tc->buf[1].format);
		tc->surf[1] = cairo_image_surface_create_for_data(
							tc->buf[1].data,
							format,
							tc->buf[1].width,
							tc->buf[1].height,
							tc->buf[1].stride);

		ret = cairo_surface_status(tc->surf[0]);
		ret2 = cairo_surface_status(tc->surf[1]);
		if (ret != CAIRO_STATUS_SUCCESS ||
		    ret2 != CAIRO_STATUS_SUCCESS) {
			log_error("cannot create cairo surface: %d %d",
				  ret, ret2);
			cairo_surface_destroy(tc->surf[1]);
			cairo_surface_destroy(tc->surf[0]);
			ret = alloc_indirect(txt, w, h);
			if (ret)
				goto err_htable_bold;
		}
	}

	tc->ctx[0] = cairo_create(tc->surf[0]);
	tc->ctx[1] = cairo_create(tc->surf[1]);
	ret = cairo_status(tc->ctx[0]);
	ret2 = cairo_status(tc->ctx[1]);
	if (ret != CAIRO_STATUS_SUCCESS || ret2 != CAIRO_STATUS_SUCCESS) {
		log_error("cannot create cairo contexts: %d %d", ret, ret2);
		goto err_ctx;
	}

	txt->cols = w / txt->font->attr.width;
	txt->rows = h / txt->font->attr.height;

	return 0;

err_ctx:
	cairo_destroy(tc->ctx[1]);
	cairo_destroy(tc->ctx[0]);
	cairo_surface_destroy(tc->surf[1]);
	cairo_surface_destroy(tc->surf[0]);
	free(tc->data[1]);
	free(tc->data[0]);
err_htable_bold:
	shl_hashtable_free(tc->bold_glyphs);
err_htable:
	shl_hashtable_free(tc->glyphs);
	return ret;
}

static void tc_unset(struct kmscon_text *txt)
{
	struct tc_cairo *tc = txt->data;

	cairo_destroy(tc->ctx[1]);
	cairo_destroy(tc->ctx[0]);
	cairo_surface_destroy(tc->surf[1]);
	cairo_surface_destroy(tc->surf[0]);
	free(tc->data[1]);
	free(tc->data[0]);
	shl_hashtable_free(tc->bold_glyphs);
	shl_hashtable_free(tc->glyphs);
}

static int find_glyph(struct kmscon_text *txt, struct tc_glyph **out,
		      uint32_t id, const uint32_t *ch, size_t len, bool bold)
{
	struct tc_cairo *tc = txt->data;
	struct tc_glyph *glyph;
	struct shl_hashtable *gtable;
	struct kmscon_font *font;
	const struct uterm_video_buffer *buf;
	uint8_t *dst, *src;
	unsigned int format, i;
	int ret, stride;
	bool res;

	if (bold) {
		gtable = tc->bold_glyphs;
		font = txt->bold_font;
	} else {
		gtable = tc->glyphs;
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
	format = format_u2c(buf->format);
	glyph->surf = cairo_image_surface_create_for_data(buf->data,
							  format,
							  buf->width,
							  buf->height,
							  buf->stride);
	ret = cairo_surface_status(glyph->surf);
	if (ret == CAIRO_STATUS_INVALID_STRIDE) {
		stride = cairo_format_stride_for_width(format, buf->width);
		if (!tc->new_stride) {
			tc->new_stride = true;
			log_debug("wrong stride, copy buffer (%d => %d)",
			  buf->stride, stride);
		}

		glyph->data = malloc(stride * buf->height);
		if (!glyph->data) {
			log_error("cannot allocate memory for glyph storage");
			ret = -ENOMEM;
			goto err_cairo;
		}

		src = buf->data;
		dst = glyph->data;
		for (i = 0; i < buf->height; ++i) {
			memcpy(dst, src, buf->width);
			dst += stride;
			src += buf->stride;
		}

		cairo_surface_destroy(glyph->surf);
		glyph->surf = cairo_image_surface_create_for_data(glyph->data,
								  format,
								  buf->width,
								  buf->height,
								  stride);
		ret = cairo_surface_status(glyph->surf);
	}
	if (ret != CAIRO_STATUS_SUCCESS) {
		log_error("cannot create cairo-glyph: %d %p %d %d %d %d",
			  ret, glyph->data ? glyph->data : buf->data, format,
			  buf->width, buf->height, stride);
		ret = -EFAULT;
		goto err_cairo;
	}

	ret = shl_hashtable_insert(gtable, (void*)(long)id, glyph);
	if (ret)
		goto err_cairo;

	*out = glyph;
	return 0;

err_cairo:
	cairo_surface_destroy(glyph->surf);
	free(glyph->data);
err_free:
	free(glyph);
	return ret;
}

static int tc_prepare(struct kmscon_text *txt)
{
	struct tc_cairo *tc = txt->data;
	int ret;

	ret = uterm_display_use(txt->disp, NULL);
	if (ret < 0) {
		log_error("cannot use display %p", txt->disp);
		return ret;
	}

	tc->cur = ret;

	return 0;
}

static int tc_draw(struct kmscon_text *txt,
		   uint32_t id, const uint32_t *ch, size_t len,
		   unsigned int width,
		   unsigned int posx, unsigned int posy,
		   const struct tsm_screen_attr *attr)
{
	struct tc_cairo *tc = txt->data;
	cairo_t *cr = tc->ctx[tc->cur];
	struct tc_glyph *glyph;
	int ret;

	if (!width)
		return 0;

	ret = find_glyph(txt, &glyph, id, ch, len, attr->bold);
	if (ret)
		return ret;

	cairo_rectangle(cr,
			posx * txt->font->attr.width,
			posy * txt->font->attr.height,
			txt->font->attr.width,
			txt->font->attr.height);

	if (attr->inverse)
		cairo_set_source_rgb(cr, attr->fr / 255.0, attr->fg / 255.0,
				     attr->fb / 255.0);
	else
		cairo_set_source_rgb(cr, attr->br / 255.0, attr->bg / 255.0,
				     attr->bb / 255.0);

	cairo_fill(cr);

	if (attr->inverse)
		cairo_set_source_rgb(cr, attr->br / 255.0, attr->bg / 255.0,
				     attr->bb / 255.0);
	else
		cairo_set_source_rgb(cr, attr->fr / 255.0, attr->fg / 255.0,
				     attr->fb / 255.0);

	cairo_mask_surface(cr, glyph->surf,
			   posx * txt->font->attr.width,
			   posy * txt->font->attr.height);

	return 0;
}

static int tc_render(struct kmscon_text *txt)
{
	struct tc_cairo *tc = txt->data;
	int ret;

	cairo_surface_flush(tc->surf[tc->cur]);

	if (!tc->use_indirect)
		return 0;

	tc->vbuf.data = tc->data[tc->cur];
	ret = uterm_display_blit(txt->disp, &tc->vbuf, 0, 0);
	if (ret) {
		log_error("cannot blit back-buffer to display: %d", ret);
		return ret;
	}

	return 0;
}

struct kmscon_text_ops kmscon_text_cairo_ops = {
	.name = "cairo",
	.owner = NULL,
	.init = tc_init,
	.destroy = tc_destroy,
	.set = tc_set,
	.unset = tc_unset,
	.prepare = tc_prepare,
	.draw = tc_draw,
	.render = tc_render,
	.abort = NULL,
};
