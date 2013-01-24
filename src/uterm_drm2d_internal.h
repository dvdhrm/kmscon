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

/* Internal definitions */

#ifndef UTERM_DRM2D_INTERNAL_H
#define UTERM_DRM2D_INTERNAL_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include "uterm_video.h"

struct uterm_drm2d_rb {
	uint32_t fb;
	uint32_t handle;
	uint32_t stride;
	uint64_t size;
	void *map;
};

struct uterm_drm2d_display {
	int current_rb;
	struct uterm_drm2d_rb rb[2];
};

struct uterm_drm2d_video {
	int fd;
	struct ev_fd *efd;
};

int uterm_drm2d_display_blit(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y);
int uterm_drm2d_display_fake_blendv(struct uterm_display *disp,
				    const struct uterm_video_blend_req *req,
				    size_t num);
int uterm_drm2d_display_fill(struct uterm_display *disp,
			     uint8_t r, uint8_t g, uint8_t b,
			     unsigned int x, unsigned int y,
			     unsigned int width, unsigned int height);

#endif /* UTERM_DRM2D_INTERNAL_H */
