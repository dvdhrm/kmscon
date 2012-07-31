/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
#include "static_misc.h"
#include "uterm.h"
#include "uterm_internal.h"

#define LOG_SUBSYSTEM "video_fbdev"

static const char *mode_get_name(const struct uterm_mode *mode)
{
	return "<default>";
}

static unsigned int mode_get_width(const struct uterm_mode *mode)
{
	return mode->fbdev.width;
}

static unsigned int mode_get_height(const struct uterm_mode *mode)
{
	return mode->fbdev.height;
}

static int refresh_info(struct uterm_display *disp)
{
	int ret;

	ret = ioctl(disp->fbdev.fd, FBIOGET_FSCREENINFO, &disp->fbdev.finfo);
	if (ret) {
		log_err("cannot get finfo (%d): %m", errno);
		return -EFAULT;
	}

	ret = ioctl(disp->fbdev.fd, FBIOGET_VSCREENINFO, &disp->fbdev.vinfo);
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
	static const char depths[] = { 32, 24, 16, 0 };
	struct fb_var_screeninfo *vinfo;
	struct fb_fix_screeninfo *finfo;
	int ret, i;
	uint64_t quot;
	size_t len;

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

	finfo = &disp->fbdev.finfo;
	vinfo = &disp->fbdev.vinfo;

	vinfo->xoffset = 0;
	vinfo->yoffset = 0;
	vinfo->activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	vinfo->xres_virtual = vinfo->xres;
	vinfo->yres_virtual = vinfo->yres * 2;
	vinfo->bits_per_pixel = 32;

	log_info("activating display %s to %ux%u %u bpp", disp->fbdev.node,
		 vinfo->xres, vinfo->yres, vinfo->bits_per_pixel);

	ret = ioctl(disp->fbdev.fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
		ret = ioctl(disp->fbdev.fd, FBIOPUT_VSCREENINFO, vinfo);
		if (ret) {
			log_err("cannot set vinfo (%d): %m",
				errno);
			return -EFAULT;
		}
		log_debug("disabling double buffering");
	} else {
		disp->flags |= DISPLAY_DBUF;
		log_debug("enabling double buffering");
	}

	ret = refresh_info(disp);
	if (ret)
		return ret;

	/* We require TRUECOLOR mode here. That is, each pixel has a color value
	 * that is split into rgba values that we can set directly. Other visual
	 * modes like pseudocolor or direct-color do not provide this. As I have
	 * never seen a device that does not support TRUECOLOR, I think we can
	 * ignore them here. */
	if (finfo->visual != FB_VISUAL_TRUECOLOR) {
		for (i = 0; depths[i]; ++i) {
			vinfo->bits_per_pixel = depths[i];
			vinfo->activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;

			ret = ioctl(disp->fbdev.fd, FBIOPUT_VSCREENINFO,
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
		log_error("device %s does not support out buffer sizes",
			  disp->fbdev.node);
		return -EFAULT;
	}

	if (vinfo->bits_per_pixel % 8) {
		log_error("device %s uses no power of 8 bpp: %u",
			  disp->fbdev.node, vinfo->bits_per_pixel);
		return -EFAULT;
	}

	if (finfo->visual != FB_VISUAL_TRUECOLOR ||
	    vinfo->bits_per_pixel < 16) {
		log_error("device %s does not support true-color bpp >= 16",
			  disp->fbdev.node);
		return -EFAULT;
	}

	/* TODO: remove this check and correctly provide conversions for the
	 * blitting functions. In fact, the for-loop above is totally useless
	 * while using this restriction here but lets be optimistic and say that
	 * this will be replaced soon. */
	if (vinfo->red.offset != 16 || vinfo->red.length != 8 ||
	    vinfo->green.offset != 8 || vinfo->green.length != 8 ||
	    vinfo->blue.offset != 0 || vinfo->green.length != 8 ||
	    vinfo->bits_per_pixel != 32) {
		log_error("device %s does not support xrgb32",
			  disp->fbdev.node);
		return -EFAULT;
	}

	/* calculate monitor rate, default is 60 Hz */
	quot = (vinfo->upper_margin + vinfo->lower_margin + vinfo->yres);
	quot *= (vinfo->left_margin + vinfo->right_margin + vinfo->xres);
	quot *= vinfo->pixclock;
	if (quot)
		disp->fbdev.rate = 1000000000000000LLU / quot;
	else
		disp->fbdev.rate = 60 * 1000;

	len = finfo->line_length * vinfo->yres;
	if (disp->flags & DISPLAY_DBUF)
		len *= 2;

	disp->fbdev.map = mmap(0, len, PROT_WRITE, MAP_SHARED,
			       disp->fbdev.fd, 0);
	if (disp->fbdev.map == MAP_FAILED) {
		log_error("cannot mmap device %s (%d): %m", disp->fbdev.node,
			  errno);
		return -EFAULT;
	}

	memset(disp->fbdev.map, 0, len);
	disp->fbdev.xres = vinfo->xres;
	disp->fbdev.yres = vinfo->yres;
	disp->fbdev.len = len;
	disp->fbdev.bpp = vinfo->bits_per_pixel / 8;
	disp->fbdev.stride = finfo->line_length;
	disp->fbdev.bufid = 0;

	ret = mode_new(&disp->modes, &fbdev_mode_ops);
	if (ret) {
		munmap(disp->fbdev.map, disp->fbdev.len);
		return ret;
	}
	disp->modes->fbdev.width = disp->fbdev.xres;
	disp->modes->fbdev.height = disp->fbdev.yres;
	disp->current_mode = disp->modes;

	disp->flags |= DISPLAY_ONLINE;
	return 0;
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	return display_activate_force(disp, mode, false);
}

static void display_deactivate_force(struct uterm_display *disp, bool force)
{
	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return;

	log_info("deactivating device %s", disp->fbdev.node);
	uterm_mode_unref(disp->modes);
	disp->modes = NULL;
	disp->current_mode = NULL;
	munmap(disp->fbdev.map, disp->fbdev.len);

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

	log_info("setting DPMS of device %p to %s", disp->fbdev.node,
		 uterm_dpms_to_name(state));

	ret = ioctl(disp->fbdev.fd, FBIOBLANK, set);
	if (ret) {
		log_error("cannot set DPMS on %s (%d): %m", disp->fbdev.node,
			  errno);
		return -EFAULT;
	}

	disp->dpms = state;
	return 0;
}

void *fbdev_display_map(struct uterm_display *disp)
{
	if (!disp->video || !video_is_awake(disp->video))
		return NULL;
	if (!(disp->flags & DISPLAY_ONLINE))
		return NULL;

	/* TODO: temporary function to obtain a pointer to the frambuffer from
	 * the calling application. Stuff like bpp, size, etc. must be
	 * published, too, otherwise, no-one will be able to use it. */

	return disp->fbdev.map;
}

static int display_swap(struct uterm_display *disp)
{
	struct fb_var_screeninfo *vinfo;
	int ret;

	if (!disp->video || !video_is_awake(disp->video))
		return -EINVAL;
	if (!(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;

	if (!(disp->flags & DISPLAY_DBUF))
		return 0;

	vinfo = &disp->fbdev.vinfo;
	vinfo->activate = FB_ACTIVATE_VBL;

	if (!disp->fbdev.bufid)
		vinfo->yoffset = disp->fbdev.yres;
	else
		vinfo->yoffset = 0;

	ret = ioctl(disp->fbdev.fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		log_warning("cannot swap buffers on %s (%d): %m",
			    disp->fbdev.node, errno);
		return -EFAULT;
	}

	disp->fbdev.bufid ^= 1;
	return 0;
}

static int display_blit(struct uterm_display *disp,
			const struct uterm_video_buffer *buf,
			unsigned int x, unsigned int y)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!buf || !video_is_awake(disp->video))
		return -EINVAL;
	if (buf->format != UTERM_FORMAT_XRGB32)
		return -EINVAL;

	tmp = x + buf->width;
	if (tmp < x || x >= disp->fbdev.xres)
		return -EINVAL;
	if (tmp > disp->fbdev.xres)
		width = disp->fbdev.xres - x;
	else
		width = buf->width;

	tmp = y + buf->height;
	if (tmp < y || y >= disp->fbdev.yres)
		return -EINVAL;
	if (tmp > disp->fbdev.yres)
		height = disp->fbdev.yres - y;
	else
		height = buf->height;

	if (!(disp->flags & DISPLAY_DBUF) || disp->fbdev.bufid)
		dst = disp->fbdev.map;
	else
		dst = &disp->fbdev.map[disp->fbdev.yres * disp->fbdev.stride];
	dst = &dst[y * disp->fbdev.stride + x * disp->fbdev.bpp];
	src = buf->data;

	while (--height) {
		memcpy(dst, src, 4 * width);
		dst += disp->fbdev.stride;
		src += buf->stride;
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

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!video_is_awake(disp->video))
		return -EINVAL;

	tmp = x + width;
	if (tmp < x || x >= disp->fbdev.xres)
		return -EINVAL;
	if (tmp > disp->fbdev.xres)
		width = disp->fbdev.xres - x;
	tmp = y + height;
	if (tmp < y || y >= disp->fbdev.yres)
		return -EINVAL;
	if (tmp > disp->fbdev.yres)
		height = disp->fbdev.yres - y;

	if (!(disp->flags & DISPLAY_DBUF) || disp->fbdev.bufid)
		dst = disp->fbdev.map;
	else
		dst = &disp->fbdev.map[disp->fbdev.yres * disp->fbdev.stride];
	dst = &dst[y * disp->fbdev.stride + x * disp->fbdev.bpp];

	while (--height) {
		for (i = 0; i < width; ++i) {
			((uint32_t*)dst)[i] = ((r & 0xff) << 16) |
					      ((g & 0xff) << 8) |
					       (b & 0xff);
		}
		dst += disp->fbdev.stride;
	}

	return 0;
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	struct uterm_display *disp;

	ret = display_new(&disp, &fbdev_display_ops);
	if (ret)
		return ret;

	disp->fbdev.node = strdup(node);
	if (!disp->fbdev.node) {
		log_err("cannot dup node name");
		ret = -ENOMEM;
		goto err_free;
	}

	disp->fbdev.fd = open(node, O_RDWR | O_CLOEXEC);
	if (disp->fbdev.fd < 0) {
		log_err("cannot open %s (%d): %m", node, errno);
		ret = -EFAULT;
		goto err_node;
	}

	disp->video = video;
	disp->dpms = UTERM_DPMS_UNKNOWN;
	video->displays = disp;

	log_info("new device on %s", disp->fbdev.node);
	return 0;

err_node:
	free(disp->fbdev.node);
err_free:
	uterm_display_unref(disp);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct uterm_display *disp;

	log_info("free device %p", video);
	disp = video->displays;
	video->displays = disp->next;
	close(disp->fbdev.fd);
	free(disp->fbdev.node);
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

const struct mode_ops fbdev_mode_ops = {
	.init = NULL,
	.destroy = NULL,
	.get_name = mode_get_name,
	.get_width = mode_get_width,
	.get_height = mode_get_height,
};

const struct display_ops fbdev_display_ops = {
	.init = NULL,
	.destroy = NULL,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = NULL,
	.swap = display_swap,
	.blit = display_blit,
	.fill = display_fill,
};

const struct video_ops fbdev_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO */
	.use = NULL,
	.poll = NULL,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};
