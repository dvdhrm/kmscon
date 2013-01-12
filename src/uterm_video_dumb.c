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
 * DRM Video backend using dumb buffer objects
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
#include "log.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video_dumb"

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

static int display_init(struct uterm_display *disp)
{
	struct uterm_drm2d_display *d2d;
	int ret;

	d2d = malloc(sizeof(*d2d));
	if (!d2d)
		return -ENOMEM;
	memset(d2d, 0, sizeof(*d2d));

	ret = uterm_drm_display_init(disp, d2d);
	if (ret) {
		free(d2d);
		return ret;
	}

	return 0;
}

static void display_destroy(struct uterm_display *disp)
{
	free(uterm_drm_display_get_data(disp));
	uterm_drm_display_destroy(disp);
}

static int init_rb(struct uterm_display *disp, struct uterm_drm2d_rb *rb)
{
	int ret, r;
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = video->data;
	struct drm_mode_create_dumb req;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;

	memset(&req, 0, sizeof(req));
	req.width = uterm_drm_mode_get_width(disp->current_mode);
	req.height = uterm_drm_mode_get_height(disp->current_mode);
	req.bpp = 32;
	req.flags = 0;

	ret = drmIoctl(vdrm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &req);
	if (ret < 0) {
		log_err("cannot create dumb drm buffer");
		return -EFAULT;
	}

	rb->handle = req.handle;
	rb->stride = req.pitch;
	rb->size = req.size;

	ret = drmModeAddFB(vdrm->fd, req.width, req.height,
			   24, 32, rb->stride, rb->handle, &rb->fb);
	if (ret) {
		log_err("cannot add drm-fb");
		ret = -EFAULT;
		goto err_buf;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = rb->handle;

	ret = drmIoctl(vdrm->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		log_err("cannot map dumb buffer");
		ret = -EFAULT;
		goto err_fb;
	}

	rb->map = mmap(0, rb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       vdrm->fd, mreq.offset);
	if (rb->map == MAP_FAILED) {
		log_err("cannot mmap dumb buffer");
		ret = -EFAULT;
		goto err_fb;
	}
	memset(rb->map, 0, rb->size);

	return 0;

err_fb:
	drmModeRmFB(vdrm->fd, rb->fb);
err_buf:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = rb->handle;
	r = drmIoctl(vdrm->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	if (r)
		log_warning("cannot destroy dumb buffer (%d/%d): %m",
			    ret, errno);

	return ret;
}

static void destroy_rb(struct uterm_display *disp, struct uterm_drm2d_rb *rb)
{
	struct uterm_drm_video *vdrm = disp->video->data;
	struct drm_mode_destroy_dumb dreq;
	int ret;

	munmap(rb->map, rb->size);
	drmModeRmFB(vdrm->fd, rb->fb);
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = rb->handle;
	ret = drmIoctl(vdrm->fd, DRM_IOCTL_MODE_DESTROY_DUMB,
		       &dreq);
	if (ret)
		log_warning("cannot destroy dumb buffer (%d/%d): %m",
			    ret, errno);
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);
	int ret;
	drmModeModeInfo *minfo;

	if (!video || !video_is_awake(video) || !mode)
		return -EINVAL;
	if (display_is_online(disp))
		return -EINVAL;

	minfo = uterm_drm_mode_get_info(mode);;
	log_info("activating display %p to %ux%u", disp,
		 minfo->hdisplay, minfo->vdisplay);

	ret = uterm_drm_display_activate(disp, vdrm->fd);
	if (ret)
		return ret;

	d2d->current_rb = 0;
	disp->current_mode = mode;

	ret = init_rb(disp, &d2d->rb[0]);
	if (ret)
		goto err_saved;

	ret = init_rb(disp, &d2d->rb[1]);
	if (ret)
		goto err_rb;

	ret = drmModeSetCrtc(vdrm->fd, ddrm->crtc_id,
			     d2d->rb[0].fb, 0, 0, &ddrm->conn_id, 1,
			     minfo);
	if (ret) {
		log_err("cannot set drm-crtc");
		ret = -EFAULT;
		goto err_fb;
	}

	disp->flags |= DISPLAY_ONLINE;
	return 0;

err_fb:
	destroy_rb(disp, &d2d->rb[1]);
err_rb:
	destroy_rb(disp, &d2d->rb[0]);
err_saved:
	disp->current_mode = NULL;
	uterm_drm_display_deactivate(disp, vdrm->fd);
	return ret;
}

static void display_deactivate(struct uterm_display *disp)
{
	struct uterm_drm_video *vdrm;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	if (!display_is_online(disp))
		return;

	vdrm = disp->video->data;
	log_info("deactivating display %p", disp);

	uterm_drm_display_deactivate(disp, vdrm->fd);

	destroy_rb(disp, &d2d->rb[1]);
	destroy_rb(disp, &d2d->rb[0]);
	disp->current_mode = NULL;
	disp->flags &= ~(DISPLAY_ONLINE | DISPLAY_VSYNC);
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int ret;
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm_video *vdrm;

	if (!display_is_conn(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	vdrm = disp->video->data;
	log_info("setting DPMS of display %p to %s", disp,
		 uterm_dpms_to_name(state));

	ret = uterm_drm_set_dpms(vdrm->fd, ddrm->conn_id, state);
	if (ret < 0)
		return ret;

	disp->dpms = ret;
	return 0;
}

static int display_swap(struct uterm_display *disp)
{
	int ret;
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);
	struct uterm_drm_video *vdrm;

	if (!display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;
	if (disp->dpms != UTERM_DPMS_ON)
		return -EINVAL;

	vdrm = disp->video->data;
	errno = 0;
	d2d->current_rb ^= 1;
	ret = drmModePageFlip(vdrm->fd, ddrm->crtc_id,
			      d2d->rb[d2d->current_rb].fb,
			      DRM_MODE_PAGE_FLIP_EVENT, disp);
	if (ret) {
		log_warn("page-flip failed %d %d", ret, errno);
		return -EFAULT;
	}
	uterm_display_ref(disp);
	disp->flags |= DISPLAY_VSYNC;

	return 0;
}

static int display_blit(struct uterm_display *disp,
			const struct uterm_video_buffer *buf,
			unsigned int x, unsigned int y)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height;
	unsigned int sw, sh;
	struct uterm_drm2d_rb *rb;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	if (!disp->video || !display_is_online(disp))
		return -EINVAL;
	if (!buf || !video_is_awake(disp->video))
		return -EINVAL;
	if (buf->format != UTERM_FORMAT_XRGB32)
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

static int display_fake_blendv(struct uterm_display *disp,
			       const struct uterm_video_blend_req *req,
			       size_t num)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	unsigned int width, height, i, j;
	unsigned int sw, sh;
	unsigned int r, g, b;
	struct uterm_drm2d_rb *rb;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	if (!disp->video || !display_is_online(disp))
		return -EINVAL;
	if (!req || !video_is_awake(disp->video))
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
				/* Division by 256 instead of 255 increases
				 * speed by like 20% on slower machines.
				 * Downside is, full white is 254/254/254
				 * instead of 255/255/255. */
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
				((uint32_t*)dst)[i] = (r << 16) | (g << 8) | b;
			}
			dst += rb->stride;
			src += req->buf->stride;
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
	unsigned int sw, sh;
	struct uterm_drm2d_rb *rb;
	struct uterm_drm2d_display *d2d = uterm_drm_display_get_data(disp);

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!video_is_awake(disp->video))
		return -EINVAL;

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

static const struct display_ops dumb_display_ops = {
	.init = display_init,
	.destroy = display_destroy,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = NULL,
	.swap = display_swap,
	.blit = display_blit,
	.fake_blendv = display_fake_blendv,
	.fill = display_fill,
};

static void show_displays(struct uterm_video *video)
{
	int ret;
	struct uterm_display *iter;
	struct uterm_drm_display *ddrm;
	struct uterm_drm2d_display *d2d;
	struct uterm_drm2d_rb *rb;
	struct uterm_drm_video *vdrm = video->data;

	if (!video_is_awake(video))
		return;

	for (iter = video->displays; iter; iter = iter->next) {
		if (!display_is_online(iter))
			continue;
		if (iter->dpms != UTERM_DPMS_ON)
			continue;

		ddrm = iter->data;
		d2d = uterm_drm_display_get_data(iter);
		rb = &d2d->rb[d2d->current_rb];

		memset(rb->map, 0, rb->size);
		ret = drmModeSetCrtc(vdrm->fd, ddrm->crtc_id,
				     rb->fb, 0, 0, &ddrm->conn_id, 1,
				     uterm_drm_mode_get_info(iter->current_mode));
		if (ret) {
			log_err("cannot set drm-crtc on display %p", iter);
			continue;
		}
	}
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
			      unsigned int usec, void *data)
{
	struct uterm_display *disp = data;

	uterm_display_unref(disp);
	if (disp->flags & DISPLAY_VSYNC) {
		disp->flags &= ~DISPLAY_VSYNC;
		DISPLAY_CB(disp, UTERM_PAGE_FLIP);
	}
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	uint64_t has_dumb;
	struct uterm_drm_video *vdrm;

	ret = uterm_drm_video_init(video, node, page_flip_handler, NULL);
	if (ret)
		return ret;
	vdrm = video->data;

	if (drmGetCap(vdrm->fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 ||
	    !has_dumb) {
		log_err("driver does not support dumb buffers");
		uterm_drm_video_destroy(video);
		return -EOPNOTSUPP;
	}

	return 0;
}

static void video_destroy(struct uterm_video *video)
{
	struct uterm_display *disp;

	log_info("free drm video device %p", video);

	while ((disp = video->displays)) {
		video->displays = disp->next;
		disp->next = NULL;
		uterm_drm_display_unbind(disp);
		uterm_display_unref(disp);
	}

	uterm_drm_video_destroy(video);
}

static int video_poll(struct uterm_video *video)
{
	return uterm_drm_video_poll(video, &dumb_display_ops);
}

static void video_sleep(struct uterm_video *video)
{
	show_displays(video);
	uterm_drm_video_sleep(video);
}

static int video_wake_up(struct uterm_video *video)
{
	int ret;

	ret = uterm_drm_video_wake_up(video, &dumb_display_ops);
	if (ret)
		return ret;

	show_displays(video);
	return 0;
}

static const struct video_ops dumb_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO: reset all saved CRTCs on segfault */
	.use = NULL,
	.poll = video_poll,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};

static const struct uterm_video_module dumb_module = {
	.ops = &dumb_video_ops,
};

const struct uterm_video_module *UTERM_VIDEO_DUMB = &dumb_module;
