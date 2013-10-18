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
 * DRM shared functions
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "shl_log.h"
#include "shl_timer.h"
#include "uterm_drm_shared_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "drm_shared"

int uterm_drm_mode_init(struct uterm_mode *mode)
{
	struct uterm_drm_mode *m;

	m = malloc(sizeof(*m));
	if (!m)
		return -ENOMEM;
	memset(m, 0, sizeof(*m));
	mode->data = m;

	return 0;
}

void uterm_drm_mode_destroy(struct uterm_mode *mode)
{
	free(mode->data);
}

const char *uterm_drm_mode_get_name(const struct uterm_mode *mode)
{
	struct uterm_drm_mode *m = mode->data;

	return m->info.name;
}

unsigned int uterm_drm_mode_get_width(const struct uterm_mode *mode)
{
	struct uterm_drm_mode *m = mode->data;

	return m->info.hdisplay;
}

unsigned int uterm_drm_mode_get_height(const struct uterm_mode *mode)
{
	struct uterm_drm_mode *m = mode->data;

	return m->info.vdisplay;
}

void uterm_drm_mode_set(struct uterm_mode *mode, drmModeModeInfo *info)
{
	struct uterm_drm_mode *m = mode->data;

	m->info = *info;
}

const struct mode_ops uterm_drm_mode_ops = {
	.init = uterm_drm_mode_init,
	.destroy = uterm_drm_mode_destroy,
	.get_name = uterm_drm_mode_get_name,
	.get_width = uterm_drm_mode_get_width,
	.get_height = uterm_drm_mode_get_height,
};

int uterm_drm_set_dpms(int fd, uint32_t conn_id, int state)
{
	int i, ret, set;
	drmModeConnector *conn;
	drmModePropertyRes *prop;

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

	conn = drmModeGetConnector(fd, conn_id);
	if (!conn) {
		log_err("cannot get display connector");
		return -EFAULT;
	}

	ret = state;
	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

		if (!strcmp(prop->name, "DPMS")) {
			ret = drmModeConnectorSetProperty(fd, conn_id,
							  prop->prop_id, set);
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
		log_warn("display does not support DPMS");
		ret = UTERM_DPMS_UNKNOWN;
	}

	drmModeFreeConnector(conn);
	return ret;
}

int uterm_drm_get_dpms(int fd, drmModeConnector *conn)
{
	int i, ret;
	drmModePropertyRes *prop;

	for (i = 0; i < conn->count_props; ++i) {
		prop = drmModeGetProperty(fd, conn->props[i]);
		if (!prop) {
			log_error("cannot get DRM property (%d): %m", errno);
			continue;
		}

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

int uterm_drm_display_init(struct uterm_display *disp, void *data)
{
	struct uterm_drm_display *d;

	d = malloc(sizeof(*d));
	if (!d)
		return -ENOMEM;
	memset(d, 0, sizeof(*d));
	disp->data = d;
	d->data = data;

	return 0;
}

void uterm_drm_display_destroy(struct uterm_display *disp)
{
	free(disp->data);
}

int uterm_drm_display_activate(struct uterm_display *disp, int fd)
{
	struct uterm_video *video = disp->video;
	struct uterm_drm_display *ddrm = disp->data;
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	int crtc, i;

	res = drmModeGetResources(fd);
	if (!res) {
		log_err("cannot get resources for display %p", disp);
		return -EFAULT;
	}
	conn = drmModeGetConnector(fd, ddrm->conn_id);
	if (!conn) {
		log_err("cannot get connector for display %p", disp);
		drmModeFreeResources(res);
		return -EFAULT;
	}

	crtc = -1;
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc)
			continue;
		crtc = uterm_drm_video_find_crtc(video, res, enc);
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

	ddrm->crtc_id = crtc;
	if (ddrm->saved_crtc)
		drmModeFreeCrtc(ddrm->saved_crtc);
	ddrm->saved_crtc = drmModeGetCrtc(fd, ddrm->crtc_id);

	return 0;
}

void uterm_drm_display_deactivate(struct uterm_display *disp, int fd)
{
	struct uterm_drm_display *ddrm = disp->data;

	uterm_drm_display_wait_pflip(disp);

	if (ddrm->saved_crtc) {
		if (disp->video->flags & VIDEO_AWAKE) {
			drmModeSetCrtc(fd, ddrm->saved_crtc->crtc_id,
				       ddrm->saved_crtc->buffer_id,
				       ddrm->saved_crtc->x,
				       ddrm->saved_crtc->y,
				       &ddrm->conn_id, 1,
				       &ddrm->saved_crtc->mode);
		}
		drmModeFreeCrtc(ddrm->saved_crtc);
		ddrm->saved_crtc = NULL;
	}

	ddrm->crtc_id = 0;
	disp->flags &= ~(DISPLAY_VSYNC | DISPLAY_ONLINE | DISPLAY_PFLIP);
}

int uterm_drm_display_set_dpms(struct uterm_display *disp, int state)
{
	int ret;
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_drm_video *vdrm = disp->video->data;

	log_info("setting DPMS of display %p to %s", disp,
		 uterm_dpms_to_name(state));

	ret = uterm_drm_set_dpms(vdrm->fd, ddrm->conn_id, state);
	if (ret < 0)
		return ret;

	disp->dpms = ret;
	return 0;
}

int uterm_drm_display_wait_pflip(struct uterm_display *disp)
{
	struct uterm_video *video = disp->video;
	int ret;
	unsigned int timeout = 1000; /* 1s */

	if ((disp->flags & DISPLAY_PFLIP) || !(disp->flags & DISPLAY_VSYNC))
		return 0;

	do {
		ret = uterm_drm_video_wait_pflip(video, &timeout);
		if (ret < 1)
			break;
		else if ((disp->flags & DISPLAY_PFLIP))
			break;
	} while (timeout > 0);

	if (ret < 0)
		return ret;
	if (ret == 0 || !timeout) {
		log_warning("timeout waiting for page-flip on display %p",
			    disp);
		return -ETIMEDOUT;
	}

	return 0;
}

int uterm_drm_display_swap(struct uterm_display *disp, uint32_t fb,
			   bool immediate)
{
	struct uterm_drm_display *ddrm = disp->data;
	struct uterm_video *video = disp->video;
	struct uterm_drm_video *vdrm = video->data;
	int ret;
	drmModeModeInfo *mode;

	if (disp->dpms != UTERM_DPMS_ON)
		return -EINVAL;

	if (immediate) {
		ret = uterm_drm_display_wait_pflip(disp);
		if (ret)
			return ret;

		mode = uterm_drm_mode_get_info(disp->current_mode);
		ret = drmModeSetCrtc(vdrm->fd, ddrm->crtc_id, fb, 0, 0,
				     &ddrm->conn_id, 1, mode);
		if (ret) {
			log_error("cannot set DRM-CRTC (%d): %m", errno);
			return -EFAULT;
		}
	} else {
		if ((disp->flags & DISPLAY_VSYNC))
			return -EBUSY;

		ret = drmModePageFlip(vdrm->fd, ddrm->crtc_id, fb,
				      DRM_MODE_PAGE_FLIP_EVENT, disp);
		if (ret) {
			log_error("cannot page-flip on DRM-CRTC (%d): %m",
				  errno);
			return -EFAULT;
		}

		uterm_display_ref(disp);
		disp->flags |= DISPLAY_VSYNC;
	}

	return 0;
}

static void uterm_drm_display_pflip(struct uterm_display *disp)
{
	struct uterm_drm_video *vdrm = disp->video->data;

	disp->flags &= ~(DISPLAY_PFLIP | DISPLAY_VSYNC);
	if (vdrm->page_flip)
		vdrm->page_flip(disp);

	DISPLAY_CB(disp, UTERM_PAGE_FLIP);
}

static void display_event(int fd, unsigned int frame, unsigned int sec,
			  unsigned int usec, void *data)
{
	struct uterm_display *disp = data;

	if (disp->video && (disp->flags & DISPLAY_VSYNC))
		disp->flags |= DISPLAY_PFLIP;

	uterm_display_unref(disp);
}

static int uterm_drm_video_read_events(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;
	drmEventContext ev;
	int ret;

	/* TODO: DRM subsystem does not support non-blocking reads and it also
	 * doesn't return 0/-1 if the device is dead. This can lead to serious
	 * deadlocks in userspace if we read() after a device was unplugged. Fix
	 * this upstream and then make this code actually loop. */
	memset(&ev, 0, sizeof(ev));
	ev.version = DRM_EVENT_CONTEXT_VERSION;
	ev.page_flip_handler = display_event;
	errno = 0;
	ret = drmHandleEvent(vdrm->fd, &ev);

	if (ret < 0 && errno != EAGAIN)
		return -EFAULT;

	return 0;
}

static void do_pflips(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_video *video = data;
	struct uterm_display *disp;
	struct shl_dlist *iter;

	shl_dlist_for_each(iter, &video->displays) {
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		if ((disp->flags & DISPLAY_PFLIP))
			uterm_drm_display_pflip(disp);
	}
}

static void io_event(struct ev_fd *fd, int mask, void *data)
{
	struct uterm_video *video = data;
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_display *disp;
	struct shl_dlist *iter;
	int ret;

	/* TODO: forward HUP to caller */
	if (mask & (EV_HUP | EV_ERR)) {
		log_err("error or hangup on DRM fd");
		ev_eloop_rm_fd(vdrm->efd);
		vdrm->efd = NULL;
		return;
	}

	if (!(mask & EV_READABLE))
		return;

	ret = uterm_drm_video_read_events(video);
	if (ret)
		return;

	shl_dlist_for_each(iter, &video->displays) {
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		if ((disp->flags & DISPLAY_PFLIP))
			uterm_drm_display_pflip(disp);
	}
}

static void vt_timeout(struct ev_timer *timer, uint64_t exp, void *data)
{
	struct uterm_video *video = data;
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_display *disp;
	struct shl_dlist *iter;
	int r;

	r = uterm_drm_video_wake_up(video);
	if (!r) {
		ev_timer_update(vdrm->vt_timer, NULL);
		shl_dlist_for_each(iter, &video->displays) {
			disp = shl_dlist_entry(iter, struct uterm_display, list);
			VIDEO_CB(video, disp, UTERM_REFRESH);
		}
	}
}

void uterm_drm_video_arm_vt_timer(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;
	struct itimerspec spec;

	spec.it_value.tv_sec = 0;
	spec.it_value.tv_nsec = 20L * 1000L * 1000L; /* 20ms */
	spec.it_interval = spec.it_value;

	ev_timer_update(vdrm->vt_timer, &spec);
}

int uterm_drm_video_init(struct uterm_video *video, const char *node,
			 const struct display_ops *display_ops,
			 uterm_drm_page_flip_t pflip, void *data)
{
	struct uterm_drm_video *vdrm;
	int ret;

	log_info("new drm device via %s", node);

	vdrm = malloc(sizeof(*vdrm));
	if (!vdrm)
		return -ENOMEM;
	memset(vdrm, 0, sizeof(*vdrm));
	video->data = vdrm;
	vdrm->data = data;
	vdrm->page_flip = pflip;
	vdrm->display_ops = display_ops;

	vdrm->fd = open(node, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (vdrm->fd < 0) {
		log_err("cannot open drm device %s (%d): %m", node, errno);
		ret = -EFAULT;
		goto err_free;
	}
	/* TODO: fix the race-condition with DRM-Master-on-open */
	drmDropMaster(vdrm->fd);

	ret = ev_eloop_new_fd(video->eloop, &vdrm->efd, vdrm->fd, EV_READABLE,
			      io_event, video);
	if (ret)
		goto err_close;

	ret = shl_timer_new(&vdrm->timer);
	if (ret)
		goto err_fd;

	ret = ev_eloop_new_timer(video->eloop, &vdrm->vt_timer, NULL,
				 vt_timeout, video);
	if (ret)
		goto err_timer;

	video->flags |= VIDEO_HOTPLUG;
	return 0;

err_timer:
	shl_timer_free(vdrm->timer);
err_fd:
	ev_eloop_rm_fd(vdrm->efd);
err_close:
	close(vdrm->fd);
err_free:
	free(vdrm);
	return ret;
}

void uterm_drm_video_destroy(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;

	ev_eloop_rm_timer(vdrm->vt_timer);
	ev_eloop_unregister_idle_cb(video->eloop, do_pflips, video, EV_SINGLE);
	shl_timer_free(vdrm->timer);
	ev_eloop_rm_fd(vdrm->efd);
	close(vdrm->fd);
	free(video->data);
}

int uterm_drm_video_find_crtc(struct uterm_video *video, drmModeRes *res,
			      drmModeEncoder *enc)
{
	int i, crtc;
	struct uterm_display *iter;
	struct uterm_drm_display *ddrm;
	struct shl_dlist *it;

	for (i = 0; i < res->count_crtcs; ++i) {
		if (enc->possible_crtcs & (1 << i)) {
			crtc = res->crtcs[i];
			shl_dlist_for_each(it, &video->displays) {
				iter = shl_dlist_entry(it,
						       struct uterm_display,
						       list);
				ddrm = iter->data;
				if (ddrm->crtc_id == crtc)
					break;
			}
			if (it == &video->displays)
				return crtc;
		}
	}

	return -1;
}

static void bind_display(struct uterm_video *video, drmModeRes *res,
			 drmModeConnector *conn)
{
	struct uterm_drm_video *vdrm = video->data;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;
	struct uterm_mode *mode;
	int ret, i;

	ret = display_new(&disp, vdrm->display_ops);
	if (ret)
		return;
	ddrm = disp->data;

	for (i = 0; i < conn->count_modes; ++i) {
		ret = mode_new(&mode, &uterm_drm_mode_ops);
		if (ret)
			continue;

		uterm_drm_mode_set(mode, &conn->modes[i]);

		ret = uterm_mode_bind(mode, disp);
		if (ret) {
			uterm_mode_unref(mode);
			continue;
		}

		/* TODO: more sophisticated default-mode selection */
		if (!disp->default_mode)
			disp->default_mode = mode;

		uterm_mode_unref(mode);
	}

	if (shl_dlist_empty(&disp->modes)) {
		log_warn("no valid mode for display found");
		ret = -EFAULT;
		goto err_unref;
	}

	ddrm->conn_id = conn->connector_id;
	disp->flags |= DISPLAY_AVAILABLE;
	disp->dpms = uterm_drm_get_dpms(vdrm->fd, conn);

	log_info("display %p DPMS is %s", disp,
		 uterm_dpms_to_name(disp->dpms));

	ret = uterm_display_bind(disp, video);
	if (ret)
		goto err_unref;

	uterm_display_unref(disp);
	return;

err_unref:
	uterm_display_unref(disp);
	return;
}

int uterm_drm_video_hotplug(struct uterm_video *video,
			    bool read_dpms)
{
	struct uterm_drm_video *vdrm = video->data;
	drmModeRes *res;
	drmModeConnector *conn;
	struct uterm_display *disp;
	struct uterm_drm_display *ddrm;
	int i, dpms;
	struct shl_dlist *iter, *tmp;

	if (!video_is_awake(video) || !video_need_hotplug(video))
		return 0;

	res = drmModeGetResources(vdrm->fd);
	if (!res) {
		log_err("cannot retrieve drm resources");
		return -EACCES;
	}

	shl_dlist_for_each(iter, &video->displays) {
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		disp->flags &= ~DISPLAY_AVAILABLE;
	}

	for (i = 0; i < res->count_connectors; ++i) {
		conn = drmModeGetConnector(vdrm->fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection != DRM_MODE_CONNECTED) {
			drmModeFreeConnector(conn);
			continue;
		}

		shl_dlist_for_each(iter, &video->displays) {
			disp = shl_dlist_entry(iter, struct uterm_display,
					       list);
			ddrm = disp->data;

			if (ddrm->conn_id != res->connectors[i])
				continue;

			disp->flags |= DISPLAY_AVAILABLE;
			if (!read_dpms || !display_is_online(disp))
				break;

			dpms = uterm_drm_get_dpms(vdrm->fd, conn);
			if (dpms != disp->dpms) {
				log_debug("DPMS state for display %p changed",
					  disp);
				uterm_drm_display_set_dpms(disp, disp->dpms);
			}
			break;
		}

		if (iter == &video->displays)
			bind_display(video, res, conn);

		drmModeFreeConnector(conn);
	}

	drmModeFreeResources(res);

	shl_dlist_for_each_safe(iter, tmp, &video->displays) {
		disp = shl_dlist_entry(iter, struct uterm_display, list);
		if (!(disp->flags & DISPLAY_AVAILABLE))
			uterm_display_unbind(disp);
	}

	video->flags &= ~VIDEO_HOTPLUG;
	return 0;
}

int uterm_drm_video_wake_up(struct uterm_video *video)
{
	int ret;
	struct uterm_drm_video *vdrm = video->data;

	ret = drmSetMaster(vdrm->fd);
	if (ret) {
		log_err("cannot set DRM-master");
		return -EACCES;
	}

	video->flags |= VIDEO_AWAKE;
	ret = uterm_drm_video_hotplug(video, true);
	if (ret) {
		drmDropMaster(vdrm->fd);
		return ret;
	}

	return 0;
}

void uterm_drm_video_sleep(struct uterm_video *video)
{
	struct uterm_drm_video *vdrm = video->data;

	drmDropMaster(vdrm->fd);
	ev_timer_drain(vdrm->vt_timer, NULL);
	ev_timer_update(vdrm->vt_timer, NULL);
}

int uterm_drm_video_poll(struct uterm_video *video)
{
	video->flags |= VIDEO_HOTPLUG;
	return uterm_drm_video_hotplug(video, false);
}

/* Waits for events on DRM fd for \mtimeout milliseconds and returns 0 if the
 * timeout expired, -ERR on errors and 1 if a page-flip event has been read.
 * \mtimeout is adjusted to the remaining time. */
int uterm_drm_video_wait_pflip(struct uterm_video *video,
			       unsigned int *mtimeout)
{
	struct uterm_drm_video *vdrm = video->data;
	struct pollfd pfd;
	int ret;
	uint64_t elapsed;

	shl_timer_start(vdrm->timer);

	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = vdrm->fd;
	pfd.events = POLLIN;

	log_debug("waiting for pageflip on %p", video);
	ret = poll(&pfd, 1, *mtimeout);

	elapsed = shl_timer_stop(vdrm->timer);
	*mtimeout = *mtimeout - (elapsed / 1000 + 1);

	if (ret < 0) {
		log_error("poll() failed on DRM fd (%d): %m", errno);
		return -EFAULT;
	} else if (!ret) {
		log_warning("timeout waiting for page-flip on %p", video);
		return 0;
	} else if ((pfd.revents & POLLIN)) {
		ret = uterm_drm_video_read_events(video);
		if (ret)
			return ret;

		ret = ev_eloop_register_idle_cb(video->eloop, do_pflips,
						video, EV_ONESHOT | EV_SINGLE);
		if (ret)
			return ret;

		return 1;
	} else {
		log_debug("poll() HUP/ERR on DRM fd (%d)", pfd.revents);
		return -EFAULT;
	}
}
