/*
 * uterm - Linux User-Space Terminal
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
 * FBDEV Video backend
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "log.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video_fbdev"

struct fbdev_mode {
	unsigned int width;
	unsigned int height;
};

struct fbdev_display {
	char *node;
	int fd;
	bool pending_intro;

	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	unsigned int rate;

	unsigned int bufid;
	size_t xres;
	size_t yres;
	size_t len;
	uint8_t *map;
	unsigned int stride;

	bool xrgb32;
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

static int mode_init(struct uterm_mode *mode)
{
	struct fbdev_mode *fbdev;

	fbdev = malloc(sizeof(*fbdev));
	if (!fbdev)
		return -ENOMEM;
	memset(fbdev, 0, sizeof(*fbdev));
	mode->data = fbdev;

	return 0;
}

static void mode_destroy(struct uterm_mode *mode)
{
	free(mode->data);
}

static const char *mode_get_name(const struct uterm_mode *mode)
{
	return "<default>";
}

static unsigned int mode_get_width(const struct uterm_mode *mode)
{
	struct fbdev_mode *fbdev = mode->data;

	return fbdev->width;
}

static unsigned int mode_get_height(const struct uterm_mode *mode)
{
	struct fbdev_mode *fbdev = mode->data;

	return fbdev->height;
}

static const struct mode_ops fbdev_mode_ops = {
	.init = mode_init,
	.destroy = mode_destroy,
	.get_name = mode_get_name,
	.get_width = mode_get_width,
	.get_height = mode_get_height,
};

static int refresh_info(struct uterm_display *disp)
{
	int ret;
	struct fbdev_display *fbdev = disp->data;

	ret = ioctl(fbdev->fd, FBIOGET_FSCREENINFO, &fbdev->finfo);
	if (ret) {
		log_err("cannot get finfo (%d): %m", errno);
		return -EFAULT;
	}

	ret = ioctl(fbdev->fd, FBIOGET_VSCREENINFO, &fbdev->vinfo);
	if (ret) {
		log_err("cannot get vinfo (%d): %m", errno);
		return -EFAULT;
	}

	return 0;
}

static int display_activate_force(struct uterm_display *disp,
				  struct uterm_mode *mode,
				  bool force)
{
	/* TODO: Add support for 24-bpp. However, we need to check how 3-bytes
	 * integers are assembled in big/little/mixed endian systems. */
	static const char depths[] = { 32, 16, 0 };
	struct fb_var_screeninfo *vinfo;
	struct fb_fix_screeninfo *finfo;
	int ret, i;
	uint64_t quot;
	size_t len;
	unsigned int val;
	struct fbdev_display *fbdev = disp->data;
	struct fbdev_mode *fbdev_mode;
	struct uterm_mode *m;

	if (!disp->video || !video_is_awake(disp->video))
		return -EINVAL;
	if (!force && (disp->flags & DISPLAY_ONLINE))
		return 0;

	/* TODO: We do not support explicit modesetting in fbdev, so we require
	 * @mode to be NULL. You can still switch modes via "fbset" on the
	 * console and then restart the app. It will automatically adapt to the
	 * new mode. The only values changed here are bpp and color mode. */
	if (mode)
		return -EINVAL;

	ret = refresh_info(disp);
	if (ret)
		return ret;

	finfo = &fbdev->finfo;
	vinfo = &fbdev->vinfo;

	vinfo->xoffset = 0;
	vinfo->yoffset = 0;
	vinfo->activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	vinfo->xres_virtual = vinfo->xres;
	vinfo->yres_virtual = vinfo->yres * 2;
	disp->flags |= DISPLAY_DBUF;

	/* udlfb is broken as it reports the sizes of the virtual framebuffer
	 * (even mmap() accepts it) but the actual size that we can access
	 * without segfaults is the _real_ framebuffer. Therefore, disable
	 * double-buffering for it.
	 * TODO: fix this kernel-side!
	 * TODO: There are so many broken fbdev drivers that just accept any
	 * virtual FB sizes and then break mmap that we now disable
	 * double-buffering entirely. We might instead add a white-list or
	 * optional command-line argument to re-enable it. */
	if (true || !strcmp(finfo->id, "udlfb")) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
	}

	ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
		ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO, vinfo);
		if (ret) {
			log_debug("cannot reset fb offsets (%d): %m", errno);
			return -EFAULT;
		}
	}

	if (disp->flags & DISPLAY_DBUF)
		log_debug("enabling double buffering");
	else
		log_debug("disabling double buffering");

	ret = refresh_info(disp);
	if (ret)
		return ret;

	/* We require TRUECOLOR mode here. That is, each pixel has a color value
	 * that is split into rgba values that we can set directly. Other visual
	 * modes like pseudocolor or direct-color do not provide this. As I have
	 * never seen a device that does not support TRUECOLOR, I think we can
	 * ignore them here. */
	if (finfo->visual != FB_VISUAL_TRUECOLOR ||
	    vinfo->bits_per_pixel != 32) {
		for (i = 0; depths[i]; ++i) {
			vinfo->bits_per_pixel = depths[i];
			vinfo->activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;

			ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO,
				    vinfo);
			if (ret < 0)
				continue;

			ret = refresh_info(disp);
			if (ret)
				return ret;

			if (finfo->visual == FB_VISUAL_TRUECOLOR)
				break;
		}
	}

	if (vinfo->xres_virtual < vinfo->xres ||
	    (disp->flags & DISPLAY_DBUF &&
	     vinfo->yres_virtual < vinfo->yres * 2) ||
	    vinfo->yres_virtual < vinfo->yres) {
		log_warning("device %s has weird virtual buffer sizes (%d %d %d %d)",
			    fbdev->node, vinfo->xres, vinfo->xres_virtual,
			    vinfo->yres, vinfo->yres_virtual);
	}

	if (vinfo->bits_per_pixel != 32 &&
	    vinfo->bits_per_pixel != 16) {
		log_error("device %s does not support 16/32 bpp but: %u",
			  fbdev->node, vinfo->bits_per_pixel);
		return -EFAULT;
	}

	if (finfo->visual != FB_VISUAL_TRUECOLOR) {
		log_error("device %s does not support true-color",
			  fbdev->node);
		return -EFAULT;
	}

	if (vinfo->red.length > 8 ||
	    vinfo->green.length > 8 ||
	    vinfo->blue.length > 8) {
		log_error("device %s uses unusual color-ranges",
			  fbdev->node);
		return -EFAULT;
	}

	log_info("activating display %s to %ux%u %u bpp", fbdev->node,
		 vinfo->xres, vinfo->yres, vinfo->bits_per_pixel);

	/* calculate monitor rate, default is 60 Hz */
	quot = (vinfo->upper_margin + vinfo->lower_margin + vinfo->yres);
	quot *= (vinfo->left_margin + vinfo->right_margin + vinfo->xres);
	quot *= vinfo->pixclock;
	if (quot) {
		fbdev->rate = 1000000000000000LLU / quot;
	} else {
		fbdev->rate = 60 * 1000;
		log_warning("cannot read monitor refresh rate, forcing 60 Hz");
	}

	if (fbdev->rate == 0) {
		log_warning("monitor refresh rate is 0 Hz, forcing it to 1 Hz");
		fbdev->rate = 1;
	} else if (fbdev->rate > 200000) {
		log_warning("monitor refresh rate is >200 Hz (%u Hz), forcing it to 200 Hz",
			    fbdev->rate / 1000);
		fbdev->rate = 200000;
	}

	val = 1000000 / fbdev->rate;
	display_set_vblank_timer(disp, val);
	log_debug("vblank timer: %u ms, monitor refresh rate: %u Hz", val,
		  fbdev->rate / 1000);

	len = finfo->line_length * vinfo->yres;
	if (disp->flags & DISPLAY_DBUF)
		len *= 2;

	fbdev->map = mmap(0, len, PROT_WRITE, MAP_SHARED,
			       fbdev->fd, 0);
	if (fbdev->map == MAP_FAILED) {
		log_error("cannot mmap device %s (%d): %m", fbdev->node,
			  errno);
		return -EFAULT;
	}

	memset(fbdev->map, 0, len);
	fbdev->xres = vinfo->xres;
	fbdev->yres = vinfo->yres;
	fbdev->len = len;
	fbdev->stride = finfo->line_length;
	fbdev->bufid = 0;
	fbdev->Bpp = vinfo->bits_per_pixel / 8;
	fbdev->off_r = vinfo->red.offset;
	fbdev->len_r = vinfo->red.length;
	fbdev->off_g = vinfo->green.offset;
	fbdev->len_g = vinfo->green.length;
	fbdev->off_b = vinfo->blue.offset;
	fbdev->len_b = vinfo->blue.length;
	fbdev->dither_r = 0;
	fbdev->dither_g = 0;
	fbdev->dither_b = 0;
	fbdev->xrgb32 = false;
	if (fbdev->len_r == 8 &&
	    fbdev->len_g == 8 &&
	    fbdev->len_b == 8 &&
	    fbdev->off_r == 16 &&
	    fbdev->off_g ==  8 &&
	    fbdev->off_b ==  0 &&
	    fbdev->Bpp == 4)
		fbdev->xrgb32 = true;

	/* TODO: make dithering configurable */
	disp->flags |= DISPLAY_DITHERING;

	if (!disp->current_mode) {
		ret = mode_new(&m, &fbdev_mode_ops);
		if (ret) {
			munmap(fbdev->map, fbdev->len);
			return ret;
		}
		m->next = disp->modes;
		disp->modes = m;

		fbdev_mode->width = fbdev->xres;
		fbdev_mode->height = fbdev->yres;
		disp->current_mode = disp->modes;
	}

	disp->flags |= DISPLAY_ONLINE;
	return 0;
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	return display_activate_force(disp, mode, false);
}

static void display_deactivate_force(struct uterm_display *disp, bool force)
{
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return;

	log_info("deactivating device %s", fbdev->node);

	if (!force) {
		uterm_mode_unref(disp->current_mode);
		disp->modes = NULL;
		disp->current_mode = NULL;
	}
	memset(fbdev->map, 0, fbdev->len);
	munmap(fbdev->map, fbdev->len);

	if (!force)
		disp->flags &= ~DISPLAY_ONLINE;
}

static void display_deactivate(struct uterm_display *disp)
{
	return display_deactivate_force(disp, false);
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int set, ret;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;

	switch (state) {
	case UTERM_DPMS_ON:
		set = FB_BLANK_UNBLANK;
		break;
	case UTERM_DPMS_STANDBY:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_SUSPEND:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_OFF:
		set = FB_BLANK_POWERDOWN;
		break;
	default:
		return -EINVAL;
	}

	log_info("setting DPMS of device %p to %s", fbdev->node,
		 uterm_dpms_to_name(state));

	ret = ioctl(fbdev->fd, FBIOBLANK, set);
	if (ret) {
		log_error("cannot set DPMS on %s (%d): %m", fbdev->node,
			  errno);
		return -EFAULT;
	}

	disp->dpms = state;
	return 0;
}

static int display_swap(struct uterm_display *disp)
{
	struct fb_var_screeninfo *vinfo;
	int ret;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !video_is_awake(disp->video))
		return -EINVAL;
	if (!(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;

	if (!(disp->flags & DISPLAY_DBUF)) {
		return display_schedule_vblank_timer(disp);
	}

	vinfo = &fbdev->vinfo;
	vinfo->activate = FB_ACTIVATE_VBL;

	if (!fbdev->bufid)
		vinfo->yoffset = fbdev->yres;
	else
		vinfo->yoffset = 0;

	ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		log_warning("cannot swap buffers on %s (%d): %m",
			    fbdev->node, errno);
		return -EFAULT;
	}

	fbdev->bufid ^= 1;
	return display_schedule_vblank_timer(disp);
}

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

static int display_blit(struct uterm_display *disp,
			const struct uterm_video_buffer *buf,
			unsigned int x, unsigned int y)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height, i;
	uint32_t val;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!buf || !video_is_awake(disp->video))
		return -EINVAL;
	if (buf->format != UTERM_FORMAT_XRGB32)
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

static int display_blend(struct uterm_display *disp,
			 const struct uterm_video_buffer *buf,
			 unsigned int x, unsigned int y,
			 uint8_t fr, uint8_t fg, uint8_t fb,
			 uint8_t br, uint8_t bg, uint8_t bb)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height, i;
	unsigned int r, g, b;
	uint32_t val;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!buf || !video_is_awake(disp->video))
		return -EINVAL;
	if (buf->format != UTERM_FORMAT_GREY)
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

	/* Division by 256 instead of 255 increases
	 * speed by like 20% on slower machines.
	 * Downside is, full white is 254/254/254
	 * instead of 255/255/255. */
	if (fbdev->xrgb32) {
		while (height--) {
			for (i = 0; i < width; ++i) {
				if (src[i] == 0) {
					r = br;
					g = bg;
					b = bb;
				} else if (src[i] == 255) {
					r = fr;
					g = fg;
					b = fb;
				} else {
					r = fr * src[i] +
					    br * (255 - src[i]);
					r /= 256;
					g = fg * src[i] +
					    bg * (255 - src[i]);
					g /= 256;
					b = fb * src[i] +
					    bb * (255 - src[i]);
					b /= 256;
				}
				val = (r << 16) | (g << 8) | b;
				((uint32_t*)dst)[i] = val;
			}
			dst += fbdev->stride;
			src += buf->stride;
		}
	} else if (fbdev->Bpp == 2) {
		while (height--) {
			for (i = 0; i < width; ++i) {
				if (src[i] == 0) {
					r = br;
					g = bg;
					b = bb;
				} else if (src[i] == 255) {
					r = fr;
					g = fg;
					b = fb;
				} else {
					r = fr * src[i] +
					    br * (255 - src[i]);
					r /= 256;
					g = fg * src[i] +
					    bg * (255 - src[i]);
					g /= 256;
					b = fb * src[i] +
					    bb * (255 - src[i]);
					b /= 256;
				}
				val = (r << 16) | (g << 8) | b;
				((uint16_t*)dst)[i] = xrgb32_to_device(disp,
								       val);
			}
			dst += fbdev->stride;
			src += buf->stride;
		}
	} else if (fbdev->Bpp == 4) {
		while (height--) {
			for (i = 0; i < width; ++i) {
				if (src[i] == 0) {
					r = br;
					g = bg;
					b = bb;
				} else if (src[i] == 255) {
					r = fr;
					g = fg;
					b = fb;
				} else {
					r = fr * src[i] +
					    br * (255 - src[i]);
					r /= 256;
					g = fg * src[i] +
					    bg * (255 - src[i]);
					g /= 256;
					b = fb * src[i] +
					    bb * (255 - src[i]);
					b /= 256;
				}
				val = (r << 16) | (g << 8) | b;
				((uint32_t*)dst)[i] = xrgb32_to_device(disp,
								       val);
			}
			dst += fbdev->stride;
			src += buf->stride;
		}
	} else {
		log_warning("invalid Bpp");
	}

	return 0;
}

static int display_fake_blendv(struct uterm_display *disp,
			       const struct uterm_video_blend_req *req,
			       size_t num)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height, i, j;
	unsigned int r, g, b;
	uint32_t val;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!req || !video_is_awake(disp->video))
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

static int display_fill(struct uterm_display *disp,
			uint8_t r, uint8_t g, uint8_t b,
			unsigned int x, unsigned int y,
			unsigned int width, unsigned int height)
{
	unsigned int tmp, i;
	uint8_t *dst;
	uint32_t full_val, rgb32;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!video_is_awake(disp->video))
		return -EINVAL;

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

static const struct display_ops fbdev_display_ops = {
	.init = NULL,
	.destroy = NULL,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = NULL,
	.swap = display_swap,
	.blit = display_blit,
	.blend = display_blend,
	.blendv = display_fake_blendv,
	.fake_blendv = display_fake_blendv,
	.fill = display_fill,
};

static void intro_idle_event(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_display *disp = data;
	struct fbdev_display *fbdev = disp->data;

	if (!fbdev->pending_intro)
		return;

	fbdev->pending_intro = false;
	ev_eloop_unregister_idle_cb(eloop, intro_idle_event, disp);

	if (!disp->video)
		return;

	VIDEO_CB(disp->video, disp, UTERM_NEW);
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	struct uterm_display *disp;
	struct fbdev_display *fbdev;

	fbdev = malloc(sizeof(*fbdev));
	if (!fbdev)
		return -ENOMEM;
	memset(fbdev, 0, sizeof(*fbdev));

	ret = display_new(&disp, &fbdev_display_ops, video);
	if (ret)
		goto err_fbdev;
	disp->data = fbdev;

	ret = ev_eloop_register_idle_cb(video->eloop, intro_idle_event, disp);
	if (ret) {
		log_error("cannot register idle event: %d", ret);
		goto err_free;
	}
	fbdev->pending_intro = true;

	fbdev->node = strdup(node);
	if (!fbdev->node) {
		log_err("cannot dup node name");
		ret = -ENOMEM;
		goto err_idle;
	}

	fbdev->fd = open(node, O_RDWR | O_CLOEXEC);
	if (fbdev->fd < 0) {
		log_err("cannot open %s (%d): %m", node, errno);
		ret = -EFAULT;
		goto err_node;
	}

	disp->dpms = UTERM_DPMS_UNKNOWN;
	video->displays = disp;

	log_info("new device on %s", fbdev->node);
	return 0;

err_node:
	free(fbdev->node);
err_idle:
	ev_eloop_register_idle_cb(video->eloop, intro_idle_event, disp);
err_free:
	uterm_display_unref(disp);
err_fbdev:
	free(fbdev);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct uterm_display *disp;
	struct fbdev_display *fbdev;

	log_info("free device %p", video);
	disp = video->displays;
	video->displays = disp->next;
	fbdev = disp->data;

	if (fbdev->pending_intro)
		ev_eloop_unregister_idle_cb(video->eloop, intro_idle_event,
					    disp);
	else
		VIDEO_CB(video, disp, UTERM_GONE);

	close(fbdev->fd);
	free(fbdev->node);
	free(fbdev);
	uterm_display_unref(disp);
}

static void video_sleep(struct uterm_video *video)
{
	if (!(video->flags & VIDEO_AWAKE))
		return;

	display_deactivate_force(video->displays, true);
	video->flags &= ~VIDEO_AWAKE;
}

static int video_wake_up(struct uterm_video *video)
{
	int ret;

	if (video->flags & VIDEO_AWAKE)
		return 0;

	video->flags |= VIDEO_AWAKE;
	if (video->displays->flags & DISPLAY_ONLINE) {
		ret = display_activate_force(video->displays, NULL, true);
		if (ret) {
			video->flags &= ~VIDEO_AWAKE;
			return ret;
		}
	}

	return 0;
}

static const struct video_ops fbdev_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO */
	.use = NULL,
	.poll = NULL,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};

static const struct uterm_video_module fbdev_module = {
	.ops = &fbdev_video_ops,
};

const struct uterm_video_module *UTERM_VIDEO_FBDEV = &fbdev_module;
