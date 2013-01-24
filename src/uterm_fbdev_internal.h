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

/* Internal definitions */

#ifndef UTERM_FBDEV_INTERNAL_H
#define UTERM_FBDEV_INTERNAL_H

#include <inttypes.h>
#include <limits.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdlib.h>
#include "uterm_video.h"

struct fbdev_mode {
	unsigned int width;
	unsigned int height;
};

struct fbdev_display {
	int fd;
	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	unsigned int rate;
	const char *node;

	unsigned int bufid;
	size_t xres;
	size_t yres;
	size_t len;
	uint8_t *map;
	unsigned int stride;

	bool xrgb32;
	bool rgb16;
	unsigned int Bpp;
	unsigned int off_r;
	unsigned int off_g;
	unsigned int off_b;
	unsigned int len_r;
	unsigned int len_g;
	unsigned int len_b;
	int_fast32_t dither_r;
	int_fast32_t dither_g;
	int_fast32_t dither_b;
};

struct fbdev_video {
	char *node;
	bool pending_intro;
};

int uterm_fbdev_display_blit(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y);
int uterm_fbdev_display_fake_blendv(struct uterm_display *disp,
				    const struct uterm_video_blend_req *req,
				    size_t num);
int uterm_fbdev_display_fill(struct uterm_display *disp,
			     uint8_t r, uint8_t g, uint8_t b,
			     unsigned int x, unsigned int y,
			     unsigned int width, unsigned int height);

#endif /* UTERM_FBDEV_INTERNAL_H */
