/*
 * uterm - Linux User-Space Terminal drm2d module
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
 * DRM2D Video backend rendering functions
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "eloop.h"
#include "shl_log.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_drm2d_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "uterm_drm2d_render"

int uterm_drm2d_display_blit(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height;
	unsigned int sw, sh;
	struct uterm_drm2d_rb *rb;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	if (!buf || buf->format != UTERM_FORMAT_XRGB32)
		return -EINVAL;

	rb = &d2d->rb[d2d->current_rb ^ 1];
	sw = uterm_drm_mode_get_width(disp->current_mode);
	sh = uterm_drm_mode_get_height(disp->current_mode);

	tmp = x + buf->width;
	if (tmp < x || x >= sw)
		return -EINVAL;
	if (tmp > sw)
		width = sw - x;
	else
		width = buf->width;

	tmp = y + buf->height;
	if (tmp < y || y >= sh)
		return -EINVAL;
	if (tmp > sh)
		height = sh - y;
	else
		height = buf->height;

	dst = rb->map;
	dst = &dst[y * rb->stride + x * 4];
	src = buf->data;

	while (height--) {
		memcpy(dst, src, 4 * width);
		dst += rb->stride;
		src += buf->stride;
	}

	return 0;
}

int uterm_drm2d_display_fake_blendv(struct uterm_display *disp,
				    const struct uterm_video_blend_req *req,
				    size_t num)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height, i, j;
	unsigned int sw, sh;
	uint_fast32_t r, g, b, out;
	struct uterm_drm2d_rb *rb;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	if (!req)
		return -EINVAL;

	rb = &d2d->rb[d2d->current_rb ^ 1];
	sw = uterm_drm_mode_get_width(disp->current_mode);
	sh = uterm_drm_mode_get_height(disp->current_mode);

	for (j = 0; j < num; ++j, ++req) {
		if (!req->buf)
			continue;

		if (req->buf->format != UTERM_FORMAT_GREY)
			return -EOPNOTSUPP;

		tmp = req->x + req->buf->width;
		if (tmp < req->x || req->x >= sw)
			return -EINVAL;
		if (tmp > sw)
			width = sw - req->x;
		else
			width = req->buf->width;

		tmp = req->y + req->buf->height;
		if (tmp < req->y || req->y >= sh)
			return -EINVAL;
		if (tmp > sh)
			height = sh - req->y;
		else
			height = req->buf->height;

		dst = rb->map;
		dst = &dst[req->y * rb->stride + req->x * 4];
		src = req->buf->data;

		while (height--) {
			for (i = 0; i < width; ++i) {
				/* Division by 255 (t /= 255) is done with:
				 *   t += 0x80
				 *   t = (t + (t >> 8)) >> 8
				 * This speeds up the computation by ~20% as the
				 * division is not needed. */
				if (src[i] == 0) {
					r = req->br;
					g = req->bg;
					b = req->bb;
					out = (r << 16) | (g << 8) | b;
				} else if (src[i] == 255) {
					r = req->fr;
					g = req->fg;
					b = req->fb;
					out = (r << 16) | (g << 8) | b;
				} else {
					r = req->fr * src[i] +
					    req->br * (255 - src[i]);
					r += 0x80;
					r = (r + (r >> 8)) >> 8;

					g = req->fg * src[i] +
					    req->bg * (255 - src[i]);
					g += 0x80;
					g = (g + (g >> 8)) >> 8;

					b = req->fb * src[i] +
					    req->bb * (255 - src[i]);
					b += 0x80;
					b = (b + (b >> 8)) >> 8;
					out = (r << 16) | (g << 8) | b;
				}

				((uint32_t*)dst)[i] = out;
			}
			dst += rb->stride;
			src += req->buf->stride;
		}
	}

	return 0;
}

int uterm_drm2d_display_fill(struct uterm_display *disp,
			     uint8_t r, uint8_t g, uint8_t b,
			     unsigned int x, unsigned int y,
			     unsigned int width, unsigned int height)
{
	unsigned int tmp, i;
	uint8_t *dst;
	unsigned int sw, sh;
	struct uterm_drm2d_rb *rb;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	rb = &d2d->rb[d2d->current_rb ^ 1];
	sw = uterm_drm_mode_get_width(disp->current_mode);
	sh = uterm_drm_mode_get_height(disp->current_mode);

	tmp = x + width;
	if (tmp < x || x >= sw)
		return -EINVAL;
	if (tmp > sw)
		width = sw - x;
	tmp = y + height;
	if (tmp < y || y >= sh)
		return -EINVAL;
	if (tmp > sh)
		height = sh - y;

	dst = rb->map;
	dst = &dst[y * rb->stride + x * 4];

	while (height--) {
		for (i = 0; i < width; ++i)
			((uint32_t*)dst)[i] = (r << 16) | (g << 8) | b;
		dst += rb->stride;
	}

	return 0;
}
