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
 * Video Control
 * Core Implementation of the uterm_video and uterm_display objects.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video"

SHL_EXPORT
const char *uterm_dpms_to_name(int dpms)
{
	switch (dpms) {
	case UTERM_DPMS_ON:
		return "ON";
	case UTERM_DPMS_STANDBY:
		return "STANDBY";
	case UTERM_DPMS_SUSPEND:
		return "SUSPEND";
	case UTERM_DPMS_OFF:
		return "OFF";
	default:
		return "UNKNOWN";
	}
}

SHL_EXPORT
bool uterm_video_available(const struct uterm_video_module *mod)
{
	if (!mod)
		return false;

	if (mod == UTERM_VIDEO_DRM2D || mod == UTERM_VIDEO_DRM3D)
		return video_drm_available();

	return true;
}

SHL_EXPORT
int mode_new(struct uterm_mode **out, const struct mode_ops *ops)
{
	struct uterm_mode *mode;
	int ret;

	if (!out || !ops)
		return -EINVAL;

	mode = malloc(sizeof(*mode));
	if (!mode)
		return -ENOMEM;
	memset(mode, 0, sizeof(*mode));
	mode->ref = 1;
	mode->ops = ops;

	ret = VIDEO_CALL(mode->ops->init, 0, mode);
	if (ret)
		goto err_free;

	*out = mode;
	return 0;

err_free:
	free(mode);
	return ret;
}

SHL_EXPORT
void uterm_mode_ref(struct uterm_mode *mode)
{
	if (!mode || !mode->ref)
		return;

	++mode->ref;
}

SHL_EXPORT
void uterm_mode_unref(struct uterm_mode *mode)
{
	if (!mode || !mode->ref || --mode->ref)
		return;

	VIDEO_CALL(mode->ops->destroy, 0, mode);
	free(mode);
}

SHL_EXPORT
int uterm_mode_bind(struct uterm_mode *mode, struct uterm_display *disp)
{
	if (!mode || !disp || mode->disp)
		return -EINVAL;

	mode->disp = disp;
	shl_dlist_link_tail(&disp->modes, &mode->list);
	uterm_mode_ref(mode);

	return 0;
}

SHL_EXPORT
void uterm_mode_unbind(struct uterm_mode *mode)
{
	if (!mode)
		return;

	mode->disp = NULL;
	shl_dlist_unlink(&mode->list);
	uterm_mode_unref(mode);
}

SHL_EXPORT
struct uterm_mode *uterm_mode_next(struct uterm_mode *mode)
{
	if (!mode || mode->list.next == &mode->disp->modes)
		return NULL;

	return shl_dlist_entry(mode->list.next, struct uterm_mode, list);
}

SHL_EXPORT
const char *uterm_mode_get_name(const struct uterm_mode *mode)
{
	if (!mode)
		return NULL;

	return VIDEO_CALL(mode->ops->get_name, NULL, mode);
}

SHL_EXPORT
unsigned int uterm_mode_get_width(const struct uterm_mode *mode)
{
	if (!mode)
		return 0;

	return VIDEO_CALL(mode->ops->get_width, 0, mode);
}

SHL_EXPORT
unsigned int uterm_mode_get_height(const struct uterm_mode *mode)
{
	if (!mode)
		return 0;

	return VIDEO_CALL(mode->ops->get_height, 0, mode);
}

int display_schedule_vblank_timer(struct uterm_display *disp)
{
	int ret;

	if (disp->vblank_scheduled)
		return 0;

	ret = ev_timer_update(disp->vblank_timer, &disp->vblank_spec);
	if (ret)
		return ret;

	disp->vblank_scheduled = true;
	return 0;
}

void display_set_vblank_timer(struct uterm_display *disp,
			      unsigned int msecs)
{
	if (msecs >= 1000)
		msecs = 999;
	else if (msecs == 0)
		msecs = 15;

	disp->vblank_spec.it_value.tv_nsec = msecs * 1000 * 1000;
}

static void display_vblank_timer_event(struct ev_timer *timer,
				       uint64_t expirations,
				       void *data)
{
	struct uterm_display *disp = data;

	disp->vblank_scheduled = false;
	DISPLAY_CB(disp, UTERM_PAGE_FLIP);
}

int display_new(struct uterm_display **out, const struct display_ops *ops)
{
	struct uterm_display *disp;
	int ret;

	if (!out || !ops)
		return -EINVAL;

	disp = malloc(sizeof(*disp));
	if (!disp)
		return -ENOMEM;
	memset(disp, 0, sizeof(*disp));
	disp->ref = 1;
	disp->ops = ops;
	shl_dlist_init(&disp->modes);

	log_info("new display %p", disp);

	ret = shl_hook_new(&disp->hook);
	if (ret)
		goto err_free;

	disp->vblank_spec.it_value.tv_nsec = 15 * 1000 * 1000;

	ret = ev_timer_new(&disp->vblank_timer, NULL,
			   display_vblank_timer_event, disp, NULL, NULL);
	if (ret)
		goto err_hook;

	ret = VIDEO_CALL(disp->ops->init, 0, disp);
	if (ret)
		goto err_timer;

	*out = disp;
	return 0;

err_timer:
	ev_timer_unref(disp->vblank_timer);
err_hook:
	shl_hook_free(disp->hook);
err_free:
	free(disp);
	return ret;
}

SHL_EXPORT
void uterm_display_ref(struct uterm_display *disp)
{
	if (!disp || !disp->ref)
		return;

	++disp->ref;
}

SHL_EXPORT
void uterm_display_unref(struct uterm_display *disp)
{
	struct uterm_mode *mode;

	if (!disp || !disp->ref || --disp->ref)
		return;

	log_info("free display %p", disp);

	while (!shl_dlist_empty(&disp->modes)) {
		mode = shl_dlist_entry(disp->modes.prev, struct uterm_mode,
				       list);
		uterm_mode_unbind(mode);
	}

	VIDEO_CALL(disp->ops->destroy, 0, disp);
	ev_timer_unref(disp->vblank_timer);
	shl_hook_free(disp->hook);
	free(disp);
}

SHL_EXPORT
int uterm_display_bind(struct uterm_display *disp, struct uterm_video *video)
{
	int ret;

	if (!disp || !video || disp->video)
		return -EINVAL;

	ret = ev_eloop_add_timer(video->eloop, disp->vblank_timer);
	if (ret)
		return ret;

	shl_dlist_link_tail(&video->displays, &disp->list);
	disp->video = video;
	uterm_display_ref(disp);
	VIDEO_CB(disp->video, disp, UTERM_NEW);

	return 0;
}

SHL_EXPORT
void uterm_display_unbind(struct uterm_display *disp)
{
	if (!disp || !disp->video)
		return;

	VIDEO_CB(disp->video, disp, UTERM_GONE);
	uterm_display_deactivate(disp);
	disp->video = NULL;
	shl_dlist_unlink(&disp->list);
	ev_eloop_rm_timer(disp->vblank_timer);
	uterm_display_unref(disp);
}

SHL_EXPORT
struct uterm_display *uterm_display_next(struct uterm_display *disp)
{
	if (!disp || !disp->video || disp->list.next == &disp->video->displays)
		return NULL;

	return shl_dlist_entry(disp->list.next, struct uterm_display, list);
}

SHL_EXPORT
int uterm_display_register_cb(struct uterm_display *disp, uterm_display_cb cb,
			      void *data)
{
	if (!disp)
		return -EINVAL;

	return shl_hook_add_cast(disp->hook, cb, data, false);
}

SHL_EXPORT
void uterm_display_unregister_cb(struct uterm_display *disp,
				 uterm_display_cb cb, void *data)
{
	if (!disp)
		return;

	shl_hook_rm_cast(disp->hook, cb, data);
}

SHL_EXPORT
struct uterm_mode *uterm_display_get_modes(struct uterm_display *disp)
{
	if (!disp || shl_dlist_empty(&disp->modes))
		return NULL;

	return shl_dlist_entry(disp->modes.next, struct uterm_mode, list);
}

SHL_EXPORT
struct uterm_mode *uterm_display_get_current(struct uterm_display *disp)
{
	if (!disp)
		return NULL;

	return disp->current_mode;
}

SHL_EXPORT
struct uterm_mode *uterm_display_get_default(struct uterm_display *disp)
{
	if (!disp)
		return NULL;

	return disp->default_mode;
}

SHL_EXPORT
int uterm_display_get_state(struct uterm_display *disp)
{
	if (!disp)
		return UTERM_DISPLAY_GONE;

	if (disp->video) {
		if (disp->flags & DISPLAY_ONLINE) {
			if (disp->video->flags & VIDEO_AWAKE)
				return UTERM_DISPLAY_ACTIVE;
			else
				return UTERM_DISPLAY_ASLEEP;
		} else {
			return UTERM_DISPLAY_INACTIVE;
		}
	} else {
		return UTERM_DISPLAY_GONE;
	}
}

SHL_EXPORT
int uterm_display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	if (!disp || !disp->video || display_is_online(disp) ||
	    !video_is_awake(disp->video))
		return -EINVAL;

	if (!mode)
		mode = disp->default_mode;

	return VIDEO_CALL(disp->ops->activate, 0, disp, mode);
}

SHL_EXPORT
void uterm_display_deactivate(struct uterm_display *disp)
{
	if (!disp || !display_is_online(disp))
		return;

	VIDEO_CALL(disp->ops->deactivate, 0, disp);
}

SHL_EXPORT
int uterm_display_set_dpms(struct uterm_display *disp, int state)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->set_dpms, 0, disp, state);
}

SHL_EXPORT
int uterm_display_get_dpms(const struct uterm_display *disp)
{
	if (!disp || !disp->video)
		return UTERM_DPMS_OFF;

	return disp->dpms;
}

SHL_EXPORT
int uterm_display_use(struct uterm_display *disp, bool *opengl)
{
	if (!disp || !display_is_online(disp))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->use, -EOPNOTSUPP, disp, opengl);
}

SHL_EXPORT
int uterm_display_get_buffers(struct uterm_display *disp,
			      struct uterm_video_buffer *buffer,
			      unsigned int formats)
{
	if (!disp || !display_is_online(disp) || !buffer)
		return -EINVAL;

	return VIDEO_CALL(disp->ops->get_buffers, -EOPNOTSUPP, disp, buffer,
			  formats);
}

SHL_EXPORT
int uterm_display_swap(struct uterm_display *disp, bool immediate)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->swap, 0, disp, immediate);
}

SHL_EXPORT
bool uterm_display_is_swapping(struct uterm_display *disp)
{
	if (!disp)
		return false;

	return disp->vblank_scheduled || (disp->flags & DISPLAY_VSYNC);
}

SHL_EXPORT
int uterm_display_fill(struct uterm_display *disp,
		       uint8_t r, uint8_t g, uint8_t b,
		       unsigned int x, unsigned int y,
		       unsigned int width, unsigned int height)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->fill, -EOPNOTSUPP, disp, r, g, b, x, y,
			  width, height);
}

SHL_EXPORT
int uterm_display_blit(struct uterm_display *disp,
		       const struct uterm_video_buffer *buf,
		       unsigned int x, unsigned int y)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->blit, -EOPNOTSUPP, disp, buf, x, y);
}

SHL_EXPORT
int uterm_display_fake_blend(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y,
			     uint8_t fr, uint8_t fg, uint8_t fb,
			     uint8_t br, uint8_t bg, uint8_t bb)
{
	struct uterm_video_blend_req req;

	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.buf = buf;
	req.x = x;
	req.y = y;
	req.fr = fr;
	req.fg = fg;
	req.fb = fb;
	req.br = br;
	req.bg = bg;
	req.bb = bb;

	return VIDEO_CALL(disp->ops->fake_blendv, -EOPNOTSUPP, disp, &req, 1);
}

SHL_EXPORT
int uterm_display_fake_blendv(struct uterm_display *disp,
			      const struct uterm_video_blend_req *req,
			      size_t num)
{
	if (!disp || !display_is_online(disp) || !video_is_awake(disp->video))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->fake_blendv, -EOPNOTSUPP, disp, req, num);
}

SHL_EXPORT
int uterm_video_new(struct uterm_video **out, struct ev_eloop *eloop,
		    const char *node, const struct uterm_video_module *mod)
{
	struct uterm_video *video;
	int ret;

	if (!out || !eloop)
		return -EINVAL;
	if (!mod || !mod->ops)
		return -EOPNOTSUPP;

	video = malloc(sizeof(*video));
	if (!video)
		return -ENOMEM;
	memset(video, 0, sizeof(*video));
	video->ref = 1;
	video->mod = mod;
	video->ops = mod->ops;
	video->eloop = eloop;
	shl_dlist_init(&video->displays);

	ret = shl_hook_new(&video->hook);
	if (ret)
		goto err_free;

	ret = VIDEO_CALL(video->ops->init, 0, video, node);
	if (ret)
		goto err_hook;

	ev_eloop_ref(video->eloop);
	log_info("new device %p", video);
	*out = video;
	return 0;

err_hook:
	shl_hook_free(video->hook);
err_free:
	free(video);
	return ret;
}

SHL_EXPORT
void uterm_video_ref(struct uterm_video *video)
{
	if (!video || !video->ref)
		return;

	++video->ref;
}

SHL_EXPORT
void uterm_video_unref(struct uterm_video *video)
{
	struct uterm_display *disp;

	if (!video || !video->ref || --video->ref)
		return;

	log_info("free device %p", video);

	while (!shl_dlist_empty(&video->displays)) {
		disp = shl_dlist_entry(video->displays.prev,
				       struct uterm_display, list);
		uterm_display_unbind(disp);
	}

	VIDEO_CALL(video->ops->destroy, 0, video);
	shl_hook_free(video->hook);
	ev_eloop_unref(video->eloop);
	free(video);
}

SHL_EXPORT
void uterm_video_segfault(struct uterm_video *video)
{
	if (!video)
		return;

	VIDEO_CALL(video->ops->segfault, 0, video);
}

SHL_EXPORT
struct uterm_display *uterm_video_get_displays(struct uterm_video *video)
{
	if (!video || shl_dlist_empty(&video->displays))
		return NULL;

	return shl_dlist_entry(video->displays.next, struct uterm_display,
			       list);
}

SHL_EXPORT
int uterm_video_register_cb(struct uterm_video *video, uterm_video_cb cb,
				void *data)
{
	if (!video || !cb)
		return -EINVAL;

	return shl_hook_add_cast(video->hook, cb, data, false);
}

SHL_EXPORT
void uterm_video_unregister_cb(struct uterm_video *video, uterm_video_cb cb,
				void *data)
{
	if (!video || !cb)
		return;

	shl_hook_rm_cast(video->hook, cb, data);
}

SHL_EXPORT
void uterm_video_sleep(struct uterm_video *video)
{
	if (!video || !video_is_awake(video))
		return;

	log_debug("go asleep");

	VIDEO_CB(video, NULL, UTERM_SLEEP);
	video->flags &= ~VIDEO_AWAKE;
	VIDEO_CALL(video->ops->sleep, 0, video);
}

SHL_EXPORT
int uterm_video_wake_up(struct uterm_video *video)
{
	int ret;

	if (!video)
		return -EINVAL;
	if (video_is_awake(video))
		return 0;

	log_debug("wake up");

	ret = VIDEO_CALL(video->ops->wake_up, 0, video);
	if (ret) {
		video->flags &= ~VIDEO_AWAKE;
		return ret;
	}

	video->flags |= VIDEO_AWAKE;
	VIDEO_CB(video, NULL, UTERM_WAKE_UP);
	return 0;
}

SHL_EXPORT
bool uterm_video_is_awake(struct uterm_video *video)
{
	return video && video_is_awake(video);
}

SHL_EXPORT
void uterm_video_poll(struct uterm_video *video)
{
	if (!video)
		return;

	VIDEO_CALL(video->ops->poll, 0, video);
}
