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
#include "static_misc.h"
#include "uterm.h"
#include "uterm_internal.h"

#define LOG_SUBSYSTEM "video_dumb"

static const char *mode_get_name(const struct uterm_mode *mode)
{
	return mode->dumb.info.name;
}

static unsigned int mode_get_width(const struct uterm_mode *mode)
{
	return mode->dumb.info.hdisplay;
}

static unsigned int mode_get_height(const struct uterm_mode *mode)
{
	return mode->dumb.info.vdisplay;
}

static int init_rb(struct uterm_display *disp, struct dumb_rb *rb)
{
	int ret;
	struct uterm_video *video = disp->video;
	struct drm_mode_create_dumb req;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;

	memset(&req, 0, sizeof(req));
	req.width = disp->current_mode->dumb.info.hdisplay;
	req.height = disp->current_mode->dumb.info.vdisplay;
	req.bpp = 32;
	req.flags = 0;

	ret = drmIoctl(video->dumb.fd, DRM_IOCTL_MODE_CREATE_DUMB, &req);
	if (ret < 0) {
		log_err("cannot create dumb drm buffer");
		return -EFAULT;
	}

	rb->handle = req.handle;
	rb->stride = req.pitch;
	rb->size = req.size;

	ret = drmModeAddFB(video->dumb.fd,
			disp->current_mode->dumb.info.hdisplay,
			disp->current_mode->dumb.info.vdisplay,
			24, 32, rb->stride, rb->handle, &rb->fb);
	if (ret) {
		log_err("cannot add drm-fb");
		ret = -EFAULT;
		goto err_buf;
	}

	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = rb->handle;

	ret = drmIoctl(video->dumb.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		log_err("cannot map dumb buffer");
		ret = -EFAULT;
		goto err_fb;
	}

	rb->map = mmap(0, rb->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       video->dumb.fd, mreq.offset);
	if (rb->map == MAP_FAILED) {
		log_err("cannot mmap dumb buffer");
		ret = -EFAULT;
		goto err_fb;
	}

	return 0;

err_fb:
	drmModeRmFB(video->dumb.fd, rb->fb);
err_buf:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = rb->handle;
	ret = drmIoctl(video->dumb.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	if (ret)
		log_warning("cannot destroy dumb buffer");

	return ret;
}

static void destroy_rb(struct uterm_display *disp, struct dumb_rb *rb)
{
	struct drm_mode_destroy_dumb dreq;
	int ret;

	munmap(rb->map, rb->size);
	drmModeRmFB(disp->video->dumb.fd, rb->fb);
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = rb->handle;
	ret = drmIoctl(disp->video->dumb.fd, DRM_IOCTL_MODE_DESTROY_DUMB,
		       &dreq);
	if (ret)
		log_warning("cannot destroy dumb buffer");
}

static int find_crtc(struct uterm_video *video, drmModeRes *res,
							drmModeEncoder *enc)
{
	int i, crtc;
	struct uterm_display *iter;

	for (i = 0; i < res->count_crtcs; ++i) {
		if (enc->possible_crtcs & (1 << i)) {
			crtc = res->crtcs[i];
			for (iter = video->displays; iter; iter = iter->next) {
				if (iter->dumb.crtc_id == crtc)
					break;
			}
			if (!iter)
				return crtc;
		}
	}

	return -1;
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	struct uterm_video *video = disp->video;
	int ret, crtc, i;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;

	if (!video || !video_is_awake(video) || !mode)
		return -EINVAL;
	if (display_is_online(disp))
		return -EINVAL;

	log_info("activating display %p to %ux%u", disp,
			mode->dumb.info.hdisplay, mode->dumb.info.vdisplay);

	res = drmModeGetResources(video->dumb.fd);
	if (!res) {
		log_err("cannot get resources for display %p", disp);
		return -EFAULT;
	}
	conn = drmModeGetConnector(video->dumb.fd, disp->dumb.conn_id);
	if (!conn) {
		log_err("cannot get connector for display %p", disp);
		drmModeFreeResources(res);
		return -EFAULT;
	}

	crtc = -1;
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(video->dumb.fd, conn->encoders[i]);
		if (!enc)
			continue;
		crtc = find_crtc(video, res, enc);
		drmModeFreeEncoder(enc);
		if (crtc >= 0)
			break;
	}

	drmModeFreeConnector(conn);
	drmModeFreeResources(res);

	if (crtc < 0) {
		log_warn("cannot find crtc for new display");
		return -ENODEV;
	}

	disp->dumb.crtc_id = crtc;
	disp->dumb.current_rb = 0;
	disp->current_mode = mode;
	disp->dumb.saved_crtc = drmModeGetCrtc(video->dumb.fd,
					       disp->dumb.crtc_id);

	ret = init_rb(disp, &disp->dumb.rb[0]);
	if (ret)
		goto err_saved;

	ret = init_rb(disp, &disp->dumb.rb[1]);
	if (ret)
		goto err_rb;

	ret = drmModeSetCrtc(video->dumb.fd, disp->dumb.crtc_id,
			disp->dumb.rb[0].fb, 0, 0, &disp->dumb.conn_id, 1,
			&disp->current_mode->dumb.info);
	if (ret) {
		log_err("cannot set drm-crtc");
		ret = -EFAULT;
		goto err_fb;
	}

	disp->flags |= DISPLAY_ONLINE;
	return 0;

err_fb:
	destroy_rb(disp, &disp->dumb.rb[1]);
err_rb:
	destroy_rb(disp, &disp->dumb.rb[0]);
err_saved:
	disp->current_mode = NULL;
	if (disp->dumb.saved_crtc) {
		drmModeFreeCrtc(disp->dumb.saved_crtc);
		disp->dumb.saved_crtc = NULL;
	}
	return ret;
}

static void display_deactivate(struct uterm_display *disp)
{
	if (!display_is_online(disp))
		return;

	if (disp->dumb.saved_crtc) {
		if (disp->video->flags & VIDEO_AWAKE) {
			drmModeSetCrtc(disp->video->dumb.fd,
					disp->dumb.saved_crtc->crtc_id,
					disp->dumb.saved_crtc->buffer_id,
					disp->dumb.saved_crtc->x,
					disp->dumb.saved_crtc->y,
					&disp->dumb.conn_id,
					1,
					&disp->dumb.saved_crtc->mode);
		}
		drmModeFreeCrtc(disp->dumb.saved_crtc);
		disp->dumb.saved_crtc = NULL;
	}

	destroy_rb(disp, &disp->dumb.rb[1]);
	destroy_rb(disp, &disp->dumb.rb[0]);
	disp->current_mode = NULL;
	disp->flags &= ~(DISPLAY_ONLINE | DISPLAY_VSYNC);
	log_info("deactivating display %p", disp);
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int i, ret, set;
	drmModeConnector *conn;
	drmModePropertyRes *prop;

	if (!display_is_conn(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	switch (state) {
	case UTERM_DPMS_ON:
		set = DRM_MODE_DPMS_ON;
		break;
	case UTERM_DPMS_STANDBY:
		set = DRM_MODE_DPMS_STANDBY;
		break;
	case UTERM_DPMS_SUSPEND:
		set = DRM_MODE_DPMS_SUSPEND;
		break;
	case UTERM_DPMS_OFF:
		set = DRM_MODE_DPMS_OFF;
		break;
	default:
		return -EINVAL;
	}

	log_info("setting DPMS of display %p to %s", disp,
			uterm_dpms_to_name(state));

	conn = drmModeGetConnector(disp->video->dumb.fd, disp->dumb.conn_id);
	if (!conn) {
		log_err("cannot get display connector");
		return -EFAULT;
	}

	ret = 0;
	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(disp->video->dumb.fd, conn->props[i]);
		if (!prop)
			continue;

		if (!strcmp(prop->name, "DPMS")) {
			ret = drmModeConnectorSetProperty(disp->video->dumb.fd,
				disp->dumb.conn_id, prop->prop_id, set);
			if (ret) {
				log_info("cannot set DPMS");
				ret = -EFAULT;
			}
			drmModeFreeProperty(prop);
			break;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props) {
		ret = 0;
		log_warn("display does not support DPMS");
		state = UTERM_DPMS_UNKNOWN;
	}

	drmModeFreeConnector(conn);
	disp->dpms = state;
	return ret;
}

static int display_swap(struct uterm_display *disp)
{
	int ret;

	if (!display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;
	if (disp->dpms != UTERM_DPMS_ON)
		return -EINVAL;

	errno = 0;
	disp->dumb.current_rb ^= 1;
	ret = drmModePageFlip(disp->video->dumb.fd, disp->dumb.crtc_id,
				disp->dumb.rb[disp->dumb.current_rb].fb,
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
	struct dumb_rb *rb;
	unsigned int width, height;
	unsigned int sw, sh;

	if (!disp->video || !display_is_online(disp))
		return -EINVAL;
	if (!buf || !video_is_awake(disp->video))
		return -EINVAL;
	if (buf->format != UTERM_FORMAT_XRGB32)
		return -EINVAL;

	rb = &disp->dumb.rb[disp->dumb.current_rb ^ 1];
	sw = disp->current_mode->dumb.info.hdisplay;
	sh = disp->current_mode->dumb.info.vdisplay;

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

static int display_blend(struct uterm_display *disp,
			 const struct uterm_video_buffer *buf,
			 unsigned int x, unsigned int y,
			 uint8_t fr, uint8_t fg, uint8_t fb,
			 uint8_t br, uint8_t bg, uint8_t bb)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	struct dumb_rb *rb;
	unsigned int width, height, i;
	unsigned int sw, sh;
	unsigned int r, g, b;

	if (!disp->video || !display_is_online(disp))
		return -EINVAL;
	if (!buf || !video_is_awake(disp->video))
		return -EINVAL;

	rb = &disp->dumb.rb[disp->dumb.current_rb ^ 1];
	sw = disp->current_mode->dumb.info.hdisplay;
	sh = disp->current_mode->dumb.info.vdisplay;

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

	if (buf->format == UTERM_FORMAT_GREY) {
		while (height--) {
			for (i = 0; i < width; ++i) {
				/* Division by 256 instead of 255 increases
				 * speed by like 20% on slower machines.
				 * Downside is, full white is 254/254/254
				 * instead of 255/255/255. */
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
				((uint32_t*)dst)[i] = (r << 16) | (g << 8) | b;
			}
			dst += rb->stride;
			src += buf->stride;
		}
	} else {
		log_warning("using unsupported buffer format for blending");
	}

	return 0;
}

static int display_blendv(struct uterm_display *disp,
			  const struct uterm_video_blend_req *req, size_t num)
{
	unsigned int tmp;
	uint8_t *dst, *src;
	struct dumb_rb *rb;
	unsigned int width, height, i, j;
	unsigned int sw, sh;
	unsigned int r, g, b;

	if (!disp->video || !display_is_online(disp))
		return -EINVAL;
	if (!req || !video_is_awake(disp->video))
		return -EINVAL;

	rb = &disp->dumb.rb[disp->dumb.current_rb ^ 1];
	sw = disp->current_mode->dumb.info.hdisplay;
	sh = disp->current_mode->dumb.info.vdisplay;

	for (j = 0; j < num; ++j, ++req) {
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
	struct dumb_rb *rb;
	unsigned int sw, sh;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;
	if (!video_is_awake(disp->video))
		return -EINVAL;

	rb = &disp->dumb.rb[disp->dumb.current_rb ^ 1];
	sw = disp->current_mode->dumb.info.hdisplay;
	sh = disp->current_mode->dumb.info.vdisplay;

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

static void show_displays(struct uterm_video *video)
{
	int ret;
	struct uterm_display *iter;

	if (!video_is_awake(video))
		return;

	for (iter = video->displays; iter; iter = iter->next) {
		if (!display_is_online(iter))
			continue;
		if (iter->dpms != UTERM_DPMS_ON)
			continue;

		ret = drmModeSetCrtc(video->dumb.fd, iter->dumb.crtc_id,
			iter->dumb.rb[iter->dumb.current_rb].fb, 0, 0,
			&iter->dumb.conn_id, 1, &iter->current_mode->dumb.info);
		if (ret) {
			log_err("cannot set drm-crtc on display %p", iter);
			continue;
		}
	}
}

static int get_dpms(struct uterm_display *disp, drmModeConnector *conn)
{
	int i, ret;
	drmModePropertyRes *prop;

	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(disp->video->dumb.fd, conn->props[i]);
		if (!prop)
			continue;

		if (!strcmp(prop->name, "DPMS")) {
			switch (conn->prop_values[i]) {
			case DRM_MODE_DPMS_ON:
				ret = UTERM_DPMS_ON;
				break;
			case DRM_MODE_DPMS_STANDBY:
				ret = UTERM_DPMS_STANDBY;
				break;
			case DRM_MODE_DPMS_SUSPEND:
				ret = UTERM_DPMS_SUSPEND;
				break;
			case DRM_MODE_DPMS_OFF:
			default:
				ret = UTERM_DPMS_OFF;
			}

			drmModeFreeProperty(prop);
			return ret;
		}
		drmModeFreeProperty(prop);
	}

	if (i == conn->count_props)
		log_warn("display does not support DPMS");
	return UTERM_DPMS_UNKNOWN;
}

static void bind_display(struct uterm_video *video, drmModeRes *res,
							drmModeConnector *conn)
{
	struct uterm_display *disp;
	struct uterm_mode *mode;
	int ret, i;

	ret = display_new(&disp, &dumb_display_ops);
	if (ret)
		return;

	for (i = 0; i < conn->count_modes; ++i) {
		ret = mode_new(&mode, &dumb_mode_ops);
		if (ret)
			continue;
		mode->dumb.info = conn->modes[i];
		mode->next = disp->modes;
		disp->modes = mode;

		/* TODO: more sophisticated default-mode selection */
		if (!disp->default_mode)
			disp->default_mode = mode;
	}

	if (!disp->modes) {
		log_warn("no valid mode for display found");
		uterm_display_unref(disp);
		return;
	}

	disp->video = video;
	disp->dumb.conn_id = conn->connector_id;
	disp->flags |= DISPLAY_AVAILABLE;
	disp->next = video->displays;
	video->displays = disp;
	disp->dpms = get_dpms(disp, conn);
	log_info("display %p DPMS is %s", disp,
			uterm_dpms_to_name(disp->dpms));
	VIDEO_CB(video, disp, UTERM_NEW);
}

static void unbind_display(struct uterm_display *disp)
{
	if (!display_is_conn(disp))
		return;

	VIDEO_CB(disp->video, disp, UTERM_GONE);
	display_deactivate(disp);
	disp->video = NULL;
	disp->flags &= ~DISPLAY_AVAILABLE;
	uterm_display_unref(disp);
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
						unsigned int usec, void *data)
{
	struct uterm_display *disp = data;

	disp->flags &= ~DISPLAY_VSYNC;
	uterm_display_unref(disp);
}

static void event(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_video *video = data;
	drmEventContext ev;

	if (mask & (EV_HUP | EV_ERR)) {
		log_err("error or hangup on DRM fd");
		ev_eloop_rm_fd(video->dumb.efd);
		video->dumb.efd = NULL;
		return;
	}

	if (mask & EV_READABLE) {
		memset(&ev, 0, sizeof(ev));
		ev.version = DRM_EVENT_CONTEXT_VERSION;
		ev.page_flip_handler = page_flip_handler;
		drmHandleEvent(video->dumb.fd, &ev);
	}
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	struct dumb_video *dumb = &video->dumb;
	uint64_t has_dumb;

	log_info("probing %s", node);

	dumb->fd = open(node, O_RDWR | O_CLOEXEC);
	if (dumb->fd < 0) {
		log_err("cannot open drm device %s (%d): %m", node, errno);
		return -EFAULT;
	}
	drmDropMaster(dumb->fd);

	if (drmGetCap(dumb->fd, DRM_CAP_DUMB_BUFFER, &has_dumb) < 0 ||
	    !has_dumb) {
		log_err("driver does not support dumb buffers");
		ret = -EFAULT;
		goto err_close;
	}

	ret = ev_eloop_new_fd(video->eloop, &dumb->efd, dumb->fd,
				EV_READABLE, event, video);
	if (ret)
		goto err_close;

	video->flags |= VIDEO_HOTPLUG;
	log_info("new drm device via %s", node);

	return 0;

err_close:
	close(dumb->fd);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct dumb_video *dumb = &video->dumb;
	struct uterm_display *disp;

	while ((disp = video->displays)) {
		video->displays = disp->next;
		disp->next = NULL;
		unbind_display(disp);
	}

	log_info("free drm device");
	ev_eloop_rm_fd(dumb->efd);
	drmDropMaster(dumb->fd);
	close(dumb->fd);
}

static int hotplug(struct uterm_video *video)
{
	drmModeRes *res;
	drmModeConnector *conn;
	struct uterm_display *disp, *tmp;
	int i;

	if (!video_is_awake(video) || !video_need_hotplug(video))
		return 0;

	res = drmModeGetResources(video->dumb.fd);
	if (!res) {
		log_err("cannot retrieve drm resources");
		return -EACCES;
	}

	for (disp = video->displays; disp; disp = disp->next)
		disp->flags &= ~DISPLAY_AVAILABLE;

	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(video->dumb.fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED) {
			for (disp = video->displays; disp; disp = disp->next) {
				if (disp->dumb.conn_id == res->connectors[i]) {
					disp->flags |= DISPLAY_AVAILABLE;
					break;
				}
			}
			if (!disp)
				bind_display(video, res, conn);
		}
		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);

	while (video->displays) {
		tmp = video->displays;
		if (tmp->flags & DISPLAY_AVAILABLE)
			break;

		video->displays = tmp->next;
		tmp->next = NULL;
		unbind_display(tmp);
	}
	for (disp = video->displays; disp && disp->next; ) {
		tmp = disp->next;
		if (tmp->flags & DISPLAY_AVAILABLE) {
			disp = tmp;
		} else {
			disp->next = tmp->next;
			tmp->next = NULL;
			unbind_display(tmp);
		}
	}

	video->flags &= ~VIDEO_HOTPLUG;
	return 0;
}

static int video_poll(struct uterm_video *video)
{
	video->flags |= VIDEO_HOTPLUG;
	return hotplug(video);
}

static void video_sleep(struct uterm_video *video)
{
	if (!video_is_awake(video))
		return;

	drmDropMaster(video->dumb.fd);
	video->flags &= ~VIDEO_AWAKE;
}

static int video_wake_up(struct uterm_video *video)
{
	int ret;

	if (video_is_awake(video))
		return 0;

	ret = drmSetMaster(video->dumb.fd);
	if (ret) {
		log_err("cannot set DRM-master");
		return -EACCES;
	}

	video->flags |= VIDEO_AWAKE;
	ret = hotplug(video);
	if (ret) {
		video->flags &= ~VIDEO_AWAKE;
		drmDropMaster(video->dumb.fd);
		return ret;
	}

	show_displays(video);
	return 0;
}

const struct mode_ops dumb_mode_ops = {
	.init = NULL,
	.destroy = NULL,
	.get_name = mode_get_name,
	.get_width = mode_get_width,
	.get_height = mode_get_height,
};

const struct display_ops dumb_display_ops = {
	.init = NULL,
	.destroy = NULL,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = NULL,
	.swap = display_swap,
	.blit = display_blit,
	.blend = display_blend,
	.blendv = display_blendv,
	.fill = display_fill,
};

const struct video_ops dumb_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO: reset all saved CRTCs on segfault */
	.use = NULL,
	.poll = video_poll,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};
