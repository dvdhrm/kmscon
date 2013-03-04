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
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_fbdev_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video_fbdev"

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

static int display_init(struct uterm_display *disp)
{
	struct fbdev_display *fbdev;

	fbdev = malloc(sizeof(*fbdev));
	if (!fbdev)
		return -ENOMEM;
	memset(fbdev, 0, sizeof(*fbdev));
	disp->data = fbdev;
	disp->dpms = UTERM_DPMS_UNKNOWN;

	return 0;
}

static void display_destroy(struct uterm_display *disp)
{
	free(disp->data);
}

static int refresh_info(struct uterm_display *disp)
{
	int ret;
	struct fbdev_display *dfb = disp->data;

	ret = ioctl(dfb->fd, FBIOGET_FSCREENINFO, &dfb->finfo);
	if (ret) {
		log_err("cannot get finfo (%d): %m", errno);
		return -EFAULT;
	}

	ret = ioctl(dfb->fd, FBIOGET_VSCREENINFO, &dfb->vinfo);
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
	struct fbdev_display *dfb = disp->data;
	struct uterm_mode *m;
	struct fbdev_mode *mfb;
	struct fb_var_screeninfo *vinfo;
	struct fb_fix_screeninfo *finfo;
	int ret, i;
	uint64_t quot;
	size_t len;
	unsigned int val;

	if (!force && (disp->flags & DISPLAY_ONLINE))
		return 0;

	/* TODO: We do not support explicit modesetting in fbdev, so we require
	 * @mode to be NULL. You can still switch modes via "fbset" on the
	 * console and then restart the app. It will automatically adapt to the
	 * new mode. The only values changed here are bpp and color mode. */
	if (mode)
		return -EINVAL;

	dfb->fd = open(dfb->node, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (dfb->fd < 0) {
		log_err("cannot open %s (%d): %m", dfb->node, errno);
		return -EFAULT;
	}

	ret = refresh_info(disp);
	if (ret)
		goto err_close;

	finfo = &dfb->finfo;
	vinfo = &dfb->vinfo;

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

	ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
		ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO, vinfo);
		if (ret) {
			log_debug("cannot reset fb offsets (%d): %m", errno);
			goto err_close;
		}
	}

	if (disp->flags & DISPLAY_DBUF)
		log_debug("enable double buffering");
	else
		log_debug("disable double buffering");

	ret = refresh_info(disp);
	if (ret)
		goto err_close;

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

			ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO,
				    vinfo);
			if (ret < 0)
				continue;

			ret = refresh_info(disp);
			if (ret)
				goto err_close;

			if (finfo->visual == FB_VISUAL_TRUECOLOR)
				break;
		}
	}

	if (vinfo->bits_per_pixel != 32 &&
	    vinfo->bits_per_pixel != 16) {
		log_error("device %s does not support 16/32 bpp but: %u",
			  dfb->node, vinfo->bits_per_pixel);
		ret = -EFAULT;
		goto err_close;
	}

	if (vinfo->xres_virtual < vinfo->xres ||
	    (disp->flags & DISPLAY_DBUF &&
	     vinfo->yres_virtual < vinfo->yres * 2) ||
	    vinfo->yres_virtual < vinfo->yres) {
		log_warning("device %s has weird virtual buffer sizes (%d %d %d %d)",
			    dfb->node, vinfo->xres, vinfo->xres_virtual,
			    vinfo->yres, vinfo->yres_virtual);
	}

	if (finfo->visual != FB_VISUAL_TRUECOLOR) {
		log_error("device %s does not support true-color",
			  dfb->node);
		ret = -EFAULT;
		goto err_close;
	}

	if (vinfo->red.length > 8 ||
	    vinfo->green.length > 8 ||
	    vinfo->blue.length > 8) {
		log_error("device %s uses unusual color-ranges",
			  dfb->node);
		ret = -EFAULT;
		goto err_close;
	}

	log_info("activating display %s to %ux%u %u bpp", dfb->node,
		 vinfo->xres, vinfo->yres, vinfo->bits_per_pixel);

	/* calculate monitor rate, default is 60 Hz */
	quot = (vinfo->upper_margin + vinfo->lower_margin + vinfo->yres);
	quot *= (vinfo->left_margin + vinfo->right_margin + vinfo->xres);
	quot *= vinfo->pixclock;
	if (quot) {
		dfb->rate = 1000000000000000LLU / quot;
	} else {
		dfb->rate = 60 * 1000;
		log_warning("cannot read monitor refresh rate, forcing 60 Hz");
	}

	if (dfb->rate == 0) {
		log_warning("monitor refresh rate is 0 Hz, forcing it to 1 Hz");
		dfb->rate = 1;
	} else if (dfb->rate > 200000) {
		log_warning("monitor refresh rate is >200 Hz (%u Hz), forcing it to 200 Hz",
			    dfb->rate / 1000);
		dfb->rate = 200000;
	}

	val = 1000000 / dfb->rate;
	display_set_vblank_timer(disp, val);
	log_debug("vblank timer: %u ms, monitor refresh rate: %u Hz", val,
		  dfb->rate / 1000);

	len = finfo->line_length * vinfo->yres;
	if (disp->flags & DISPLAY_DBUF)
		len *= 2;

	dfb->map = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, dfb->fd, 0);
	if (dfb->map == MAP_FAILED) {
		log_error("cannot mmap device %s (%d): %m", dfb->node,
			  errno);
		ret = -EFAULT;
		goto err_close;
	}

	memset(dfb->map, 0, len);
	dfb->xres = vinfo->xres;
	dfb->yres = vinfo->yres;
	dfb->len = len;
	dfb->stride = finfo->line_length;
	dfb->bufid = 0;
	dfb->Bpp = vinfo->bits_per_pixel / 8;
	dfb->off_r = vinfo->red.offset;
	dfb->len_r = vinfo->red.length;
	dfb->off_g = vinfo->green.offset;
	dfb->len_g = vinfo->green.length;
	dfb->off_b = vinfo->blue.offset;
	dfb->len_b = vinfo->blue.length;
	dfb->dither_r = 0;
	dfb->dither_g = 0;
	dfb->dither_b = 0;
	dfb->xrgb32 = false;
	dfb->rgb16 = false;
	if (dfb->len_r == 8 && dfb->len_g == 8 && dfb->len_b == 8 &&
	    dfb->off_r == 16 && dfb->off_g ==  8 && dfb->off_b ==  0 &&
	    dfb->Bpp == 4)
		dfb->xrgb32 = true;
	else if (dfb->len_r == 5 && dfb->len_g == 6 && dfb->len_b == 5 &&
		 dfb->off_r == 11 && dfb->off_g == 5 && dfb->off_b == 0 &&
		 dfb->Bpp == 2)
		dfb->rgb16 = true;

	/* TODO: make dithering configurable */
	disp->flags |= DISPLAY_DITHERING;

	if (disp->current_mode) {
		m = disp->current_mode;
	} else {
		ret = mode_new(&m, &fbdev_mode_ops);
		if (ret)
			goto err_map;
		ret = uterm_mode_bind(m, disp);
		if (ret) {
			uterm_mode_unref(m);
			goto err_map;
		}
		disp->current_mode = m;
		uterm_mode_unref(m);
	}

	mfb = m->data;
	mfb->width = dfb->xres;
	mfb->height = dfb->yres;

	disp->flags |= DISPLAY_ONLINE;
	return 0;

err_map:
	munmap(dfb->map, dfb->len);
err_close:
	close(dfb->fd);
	return ret;
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	return display_activate_force(disp, mode, false);
}

static void display_deactivate_force(struct uterm_display *disp, bool force)
{
	struct fbdev_display *dfb = disp->data;

	log_info("deactivating device %s", dfb->node);

	if (dfb->map) {
		memset(dfb->map, 0, dfb->len);
		munmap(dfb->map, dfb->len);
		close(dfb->fd);
		dfb->map = NULL;
	}
	if (!force) {
		uterm_mode_unbind(disp->current_mode);
		disp->current_mode = NULL;
		disp->flags &= ~DISPLAY_ONLINE;
	}
}

static void display_deactivate(struct uterm_display *disp)
{
	return display_deactivate_force(disp, false);
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int set, ret;
	struct fbdev_display *dfb = disp->data;

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

	log_info("setting DPMS of device %p to %s", dfb->node,
		 uterm_dpms_to_name(state));

	ret = ioctl(dfb->fd, FBIOBLANK, set);
	if (ret) {
		log_error("cannot set DPMS on %s (%d): %m", dfb->node,
			  errno);
		return -EFAULT;
	}

	disp->dpms = state;
	return 0;
}

static int display_use(struct uterm_display *disp, bool *opengl)
{
	struct fbdev_display *dfb = disp->data;

	if (opengl)
		*opengl = false;

	if (!(disp->flags & DISPLAY_DBUF))
		return 0;

	return dfb->bufid ^ 1;
}

static int display_get_buffers(struct uterm_display *disp,
			       struct uterm_video_buffer *buffer,
			       unsigned int formats)
{
	struct fbdev_display *dfb = disp->data;
	unsigned int f = 0, i;

	if (dfb->xrgb32)
		f = UTERM_FORMAT_XRGB32;
	else if (dfb->rgb16)
		f = UTERM_FORMAT_RGB16;

	if (!(formats & f))
		return -EOPNOTSUPP;

	for (i = 0; i < 2; ++i) {
		buffer[i].width = dfb->xres;
		buffer[i].height = dfb->yres;
		buffer[i].stride = dfb->stride;
		buffer[i].format = f;
		if (!(disp->flags & DISPLAY_DBUF) || !i)
			buffer[i].data = dfb->map;
		else
			buffer[i].data = &dfb->map[dfb->yres * dfb->stride];
	}

	return 0;
}

static int display_swap(struct uterm_display *disp, bool immediate)
{
	struct fbdev_display *dfb = disp->data;
	struct fb_var_screeninfo *vinfo;
	int ret;

	if (!(disp->flags & DISPLAY_DBUF)) {
		if (immediate)
			return 0;
		return display_schedule_vblank_timer(disp);
	}

	vinfo = &dfb->vinfo;
	if (immediate)
		vinfo->activate = FB_ACTIVATE_NOW;
	else
		vinfo->activate = FB_ACTIVATE_VBL;

	if (!dfb->bufid)
		vinfo->yoffset = dfb->yres;
	else
		vinfo->yoffset = 0;

	ret = ioctl(dfb->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		log_warning("cannot swap buffers on %s (%d): %m",
			    dfb->node, errno);
		return -EFAULT;
	}

	dfb->bufid ^= 1;
	return display_schedule_vblank_timer(disp);
}

static const struct display_ops fbdev_display_ops = {
	.init = display_init,
	.destroy = display_destroy,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = display_use,
	.get_buffers = display_get_buffers,
	.swap = display_swap,
	.blit = uterm_fbdev_display_blit,
	.fake_blendv = uterm_fbdev_display_fake_blendv,
	.fill = uterm_fbdev_display_fill,
};

static void intro_idle_event(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_video *video = data;
	struct fbdev_video *vfb = video->data;
	struct uterm_display *disp;
	struct fbdev_display *dfb;
	int ret;

	vfb->pending_intro = false;
	ev_eloop_unregister_idle_cb(eloop, intro_idle_event, data, EV_NORMAL);

	ret = display_new(&disp, &fbdev_display_ops);
	if (ret) {
		log_error("cannot create fbdev display: %d", ret);
		return;
	}

	dfb = disp->data;
	dfb->node = vfb->node;
	ret = uterm_display_bind(disp, video);
	if (ret) {
		log_error("cannot bind fbdev display: %d", ret);
		uterm_display_unref(disp);
		return;
	}

	uterm_display_unref(disp);
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	struct fbdev_video *vfb;

	log_info("new device on %s", node);

	vfb = malloc(sizeof(*vfb));
	if (!vfb)
		return -ENOMEM;
	memset(vfb, 0, sizeof(*vfb));
	video->data = vfb;

	vfb->node = strdup(node);
	if (!vfb->node) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = ev_eloop_register_idle_cb(video->eloop, intro_idle_event, video,
					EV_NORMAL);
	if (ret) {
		log_error("cannot register idle event: %d", ret);
		goto err_node;
	}
	vfb->pending_intro = true;

	return 0;

err_node:
	free(vfb->node);
err_free:
	free(vfb);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct fbdev_video *vfb = video->data;

	log_info("free device on %s", vfb->node);

	if (vfb->pending_intro)
		ev_eloop_unregister_idle_cb(video->eloop, intro_idle_event,
					    video, EV_NORMAL);

	free(vfb->node);
	free(vfb);
}

static void video_sleep(struct uterm_video *video)
{
	struct uterm_display *iter;
	struct shl_dlist *i;

	shl_dlist_for_each(i, &video->displays) {
		iter = shl_dlist_entry(i, struct uterm_display, list);

		if (!display_is_online(iter))
			continue;

		display_deactivate_force(iter, true);
	}
}

static int video_wake_up(struct uterm_video *video)
{
	struct uterm_display *iter;
	struct shl_dlist *i;
	int ret;

	video->flags |= VIDEO_AWAKE;
	shl_dlist_for_each(i, &video->displays) {
		iter = shl_dlist_entry(i, struct uterm_display, list);

		if (!display_is_online(iter))
			continue;

		ret = display_activate_force(iter, NULL, true);
		if (ret)
			return ret;

		if (iter->dpms != UTERM_DPMS_UNKNOWN)
			display_set_dpms(iter, iter->dpms);
	}

	return 0;
}

static const struct video_ops fbdev_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO */
	.poll = NULL,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};

static const struct uterm_video_module fbdev_module = {
	.ops = &fbdev_video_ops,
};

SHL_EXPORT
const struct uterm_video_module *UTERM_VIDEO_FBDEV = &fbdev_module;
