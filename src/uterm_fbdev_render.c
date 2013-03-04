/*
 * uterm - Linux User-Space Terminal fbdev module
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * FBDEV module rendering functions
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_log.h"
#include "uterm_fbdev_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "fbdev_render"

static int clamp_value(int val, int low, int up)
{
	if (val < low)
		return low;
	else if (val > up)
		return up;
	else
		return val;
}

static uint_fast32_t xrgb32_to_device(struct uterm_display *disp,
				      uint32_t pixel)
{
	uint8_t r, g, b, nr, ng, nb;
	int i;
	uint_fast32_t res;
	struct fbdev_display *fbdev = disp->data;

	r = (pixel >> 16) & 0xff;
	g = (pixel >>  8) & 0xff;
	b = (pixel >>  0) & 0xff;

	if (disp->flags & DISPLAY_DITHERING) {
		/* This is some very basic dithering which simply does small
		 * rotations in the lower pixel bits. TODO: Let's take a look
		 * at Floyd-Steinberg dithering which should give much better
		 * results. It is slightly slower, though.
		 * Or even better would be some Sierra filter like the Sierra
		 * LITE. */
		fbdev->dither_r = r - fbdev->dither_r;
		fbdev->dither_g = g - fbdev->dither_g;
		fbdev->dither_b = b - fbdev->dither_b;
		r = clamp_value(fbdev->dither_r, 0, 255) >> (8 - fbdev->len_r);
		g = clamp_value(fbdev->dither_g, 0, 255) >> (8 - fbdev->len_g);
		b = clamp_value(fbdev->dither_b, 0, 255) >> (8 - fbdev->len_b);
		nr = r << (8 - fbdev->len_r);
		ng = g << (8 - fbdev->len_g);
		nb = b << (8 - fbdev->len_b);

		for (i = fbdev->len_r; i < 8; i <<= 1)
			nr |= nr >> i;
		for (i = fbdev->len_g; i < 8; i <<= 1)
			ng |= ng >> i;
		for (i = fbdev->len_b; i < 8; i <<= 1)
			nb |= nb >> i;

		fbdev->dither_r = nr - fbdev->dither_r;
		fbdev->dither_g = ng - fbdev->dither_g;
		fbdev->dither_b = nb - fbdev->dither_b;

		res  = r << fbdev->off_r;
		res |= g << fbdev->off_g;
		res |= b << fbdev->off_b;
	} else {
		res  = (r >> (8 - fbdev->len_r)) << fbdev->off_r;
		res |= (g >> (8 - fbdev->len_g)) << fbdev->off_g;
		res |= (b >> (8 - fbdev->len_b)) << fbdev->off_b;
	}

	return res;
}

int uterm_fbdev_display_blit(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height, i;
	uint32_t val;
	struct fbdev_display *fbdev = disp->data;

	if (!buf || buf->format != UTERM_FORMAT_XRGB32)
		return -EINVAL;

	tmp = x + buf->width;
	if (tmp < x || x >= fbdev->xres)
		return -EINVAL;
	if (tmp > fbdev->xres)
		width = fbdev->xres - x;
	else
		width = buf->width;

	tmp = y + buf->height;
	if (tmp < y || y >= fbdev->yres)
		return -EINVAL;
	if (tmp > fbdev->yres)
		height = fbdev->yres - y;
	else
		height = buf->height;

	if (!(disp->flags & DISPLAY_DBUF) || fbdev->bufid)
		dst = fbdev->map;
	else
		dst = &fbdev->map[fbdev->yres * fbdev->stride];
	dst = &dst[y * fbdev->stride + x * fbdev->Bpp];
	src = buf->data;

	if (fbdev->xrgb32) {
		while (height--) {
			memcpy(dst, src, 4 * width);
			dst += fbdev->stride;
			src += buf->stride;
		}
	} else if (fbdev->Bpp == 2) {
		while (height--) {
			for (i = 0; i < width; ++i) {
				val = ((uint32_t*)src)[i];
				((uint16_t*)dst)[i] = xrgb32_to_device(disp, val);
			}
			dst += fbdev->stride;
			src += buf->stride;
		}
	} else if (fbdev->Bpp == 4) {
		while (height--) {
			for (i = 0; i < width; ++i) {
				val = ((uint32_t*)src)[i];
				((uint32_t*)dst)[i] = xrgb32_to_device(disp, val);
			}
			dst += fbdev->stride;
			src += buf->stride;
		}
	} else {
		log_debug("invalid Bpp");
	}

	return 0;
}

int uterm_fbdev_display_fake_blendv(struct uterm_display *disp,
				    const struct uterm_video_blend_req *req,
				    size_t num)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height, i, j;
	unsigned int r, g, b;
	uint32_t val;
	struct fbdev_display *fbdev = disp->data;

	if (!req)
		return -EINVAL;

	for (j = 0; j < num; ++j, ++req) {
		if (!req->buf)
			continue;

		if (req->buf->format != UTERM_FORMAT_GREY)
			return -EOPNOTSUPP;

		tmp = req->x + req->buf->width;
		if (tmp < req->x || req->x >= fbdev->xres)
			return -EINVAL;
		if (tmp > fbdev->xres)
			width = fbdev->xres - req->x;
		else
			width = req->buf->width;

		tmp = req->y + req->buf->height;
		if (tmp < req->y || req->y >= fbdev->yres)
			return -EINVAL;
		if (tmp > fbdev->yres)
			height = fbdev->yres - req->y;
		else
			height = req->buf->height;

		if (!(disp->flags & DISPLAY_DBUF) || fbdev->bufid)
			dst = fbdev->map;
		else
			dst = &fbdev->map[fbdev->yres * fbdev->stride];
		dst = &dst[req->y * fbdev->stride + req->x * fbdev->Bpp];
		src = req->buf->data;

		/* Division by 256 instead of 255 increases
		 * speed by like 20% on slower machines.
		 * Downside is, full white is 254/254/254
		 * instead of 255/255/255. */
		if (fbdev->xrgb32) {
			while (height--) {
				for (i = 0; i < width; ++i) {
					if (src[i] == 0) {
						r = req->br;
						g = req->bg;
						b = req->bb;
					} else if (src[i] == 255) {
						r = req->fr;
						g = req->fg;
						b = req->fb;
					} else {
						r = req->fr * src[i] +
						    req->br * (255 - src[i]);
						r /= 256;
						g = req->fg * src[i] +
						    req->bg * (255 - src[i]);
						g /= 256;
						b = req->fb * src[i] +
						    req->bb * (255 - src[i]);
						b /= 256;
					}
					val = (r << 16) | (g << 8) | b;
					((uint32_t*)dst)[i] = val;
				}
				dst += fbdev->stride;
				src += req->buf->stride;
			}
		} else if (fbdev->Bpp == 2) {
			while (height--) {
				for (i = 0; i < width; ++i) {
					if (src[i] == 0) {
						r = req->br;
						g = req->bg;
						b = req->bb;
					} else if (src[i] == 255) {
						r = req->fr;
						g = req->fg;
						b = req->fb;
					} else {
						r = req->fr * src[i] +
						    req->br * (255 - src[i]);
						r /= 256;
						g = req->fg * src[i] +
						    req->bg * (255 - src[i]);
						g /= 256;
						b = req->fb * src[i] +
						    req->bb * (255 - src[i]);
						b /= 256;
					}
					val = (r << 16) | (g << 8) | b;
					((uint16_t*)dst)[i] =
						xrgb32_to_device(disp, val);
				}
				dst += fbdev->stride;
				src += req->buf->stride;
			}
		} else if (fbdev->Bpp == 4) {
			while (height--) {
				for (i = 0; i < width; ++i) {
					if (src[i] == 0) {
						r = req->br;
						g = req->bg;
						b = req->bb;
					} else if (src[i] == 255) {
						r = req->fr;
						g = req->fg;
						b = req->fb;
					} else {
						r = req->fr * src[i] +
						    req->br * (255 - src[i]);
						r /= 256;
						g = req->fg * src[i] +
						    req->bg * (255 - src[i]);
						g /= 256;
						b = req->fb * src[i] +
						    req->bb * (255 - src[i]);
						b /= 256;
					}
					val = (r << 16) | (g << 8) | b;
					((uint32_t*)dst)[i] =
						xrgb32_to_device(disp, val);
				}
				dst += fbdev->stride;
				src += req->buf->stride;
			}
		} else {
			log_warning("invalid Bpp");
		}
	}

	return 0;
}

int uterm_fbdev_display_fill(struct uterm_display *disp,
			     uint8_t r, uint8_t g, uint8_t b,
			     unsigned int x, unsigned int y,
			     unsigned int width, unsigned int height)
{
	unsigned int tmp, i;
	uint8_t *dst;
	uint32_t full_val, rgb32;
	struct fbdev_display *fbdev = disp->data;

	tmp = x + width;
	if (tmp < x || x >= fbdev->xres)
		return -EINVAL;
	if (tmp > fbdev->xres)
		width = fbdev->xres - x;
	tmp = y + height;
	if (tmp < y || y >= fbdev->yres)
		return -EINVAL;
	if (tmp > fbdev->yres)
		height = fbdev->yres - y;

	if (!(disp->flags & DISPLAY_DBUF) || fbdev->bufid)
		dst = fbdev->map;
	else
		dst = &fbdev->map[fbdev->yres * fbdev->stride];
	dst = &dst[y * fbdev->stride + x * fbdev->Bpp];

	full_val  = ((r & 0xff) >> (8 - fbdev->len_r)) << fbdev->off_r;
	full_val |= ((g & 0xff) >> (8 - fbdev->len_g)) << fbdev->off_g;
	full_val |= ((b & 0xff) >> (8 - fbdev->len_b)) << fbdev->off_b;

	if (fbdev->Bpp == 2) {
		if (disp->flags & DISPLAY_DITHERING) {
			rgb32  = (r & 0xff) << 16;
			rgb32 |= (g & 0xff) <<  8;
			rgb32 |= (b & 0xff) <<  0;
			while (height--) {
				for (i = 0; i < width; ++i)
					((uint16_t*)dst)[i] = xrgb32_to_device(disp, rgb32);
				dst += fbdev->stride;
			}
		} else {
			full_val &= 0xffff;
			while (height--) {
				for (i = 0; i < width; ++i)
					((uint16_t*)dst)[i] = full_val;
				dst += fbdev->stride;
			}
		}
	} else if (fbdev->Bpp == 4) {
		while (height--) {
			for (i = 0; i < width; ++i)
				((uint32_t*)dst)[i] = full_val;
			dst += fbdev->stride;
		}
	} else {
		log_error("invalid Bpp");
		return -EFAULT;
	}

	return 0;
}
