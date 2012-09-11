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
 * Video Control
 * Core Implementation of the uterm_video, uterm_display and uterm_screen
 * objects.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "eloop.h"
#include "log.h"
#include "static_misc.h"
#include "uterm.h"
#include "uterm_video.h"

#define LOG_SUBSYSTEM "video"

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

/* Until we allow multiple displays in one screen, we use this constructor which
 * is basically just a wrapper around "struct uterm_dispaly".
 * The idea behind screens is having one single drawing-target which is spread
 * across several displays which can be placed anywhere in the virtual screen.
 */
int uterm_screen_new_single(struct uterm_screen **out,
				struct uterm_display *disp)
{
	struct uterm_screen *screen;

	if (!out || !disp)
		return -EINVAL;

	screen = malloc(sizeof(*screen));
	if (!screen)
		return -ENOMEM;
	memset(screen, 0, sizeof(*screen));
	screen->ref = 1;
	screen->disp = disp;

	uterm_display_ref(screen->disp);
	*out = screen;
	return 0;
}

void uterm_screen_ref(struct uterm_screen *screen)
{
	if (!screen || !screen->ref)
		return;

	++screen->ref;
}

void uterm_screen_unref(struct uterm_screen *screen)
{
	if (!screen || !screen->ref || --screen->ref)
		return;

	uterm_display_unref(screen->disp);
	free(screen);
}

unsigned int uterm_screen_width(struct uterm_screen *screen)
{
	if (!screen)
		return 0;

	return uterm_mode_get_width(uterm_display_get_current(screen->disp));
}

unsigned int uterm_screen_height(struct uterm_screen *screen)
{
	if (!screen)
		return 0;

	return uterm_mode_get_height(uterm_display_get_current(screen->disp));
}

int uterm_screen_use(struct uterm_screen *screen)
{
	if (!screen || !display_is_online(screen->disp))
		return -EINVAL;

	return VIDEO_CALL(screen->disp->ops->use, -EOPNOTSUPP, screen->disp);
}

int uterm_screen_swap(struct uterm_screen *screen)
{
	if (!screen || !display_is_online(screen->disp))
		return -EINVAL;

	return VIDEO_CALL(screen->disp->ops->swap, 0, screen->disp);
}

int uterm_screen_blit(struct uterm_screen *screen,
		      const struct uterm_video_buffer *buf,
		      unsigned int x, unsigned int y)
{
	if (!screen)
		return -EINVAL;

	return VIDEO_CALL(screen->disp->ops->blit, -EOPNOTSUPP, screen->disp,
			  buf, x, y);
}

int uterm_screen_blend(struct uterm_screen *screen,
		       const struct uterm_video_buffer *buf,
		       unsigned int x, unsigned int y,
		       uint8_t fr, uint8_t fg, uint8_t fb,
		       uint8_t br, uint8_t bg, uint8_t bb)
{
	if (!screen)
		return -EINVAL;

	return VIDEO_CALL(screen->disp->ops->blend, -EOPNOTSUPP, screen->disp,
			  buf, x, y, fr, fg, fb, br, bg, bb);
}

int uterm_screen_blendv(struct uterm_screen *screen,
			const struct uterm_video_blend_req *req, size_t num)
{
	if (!screen)
		return -EINVAL;

	return VIDEO_CALL(screen->disp->ops->blendv, -EOPNOTSUPP,
			  screen->disp, req, num);
}

int uterm_screen_fill(struct uterm_screen *screen,
		      uint8_t r, uint8_t g, uint8_t b,
		      unsigned int x, unsigned int y,
		      unsigned int width, unsigned int height)
{
	if (!screen)
		return -EINVAL;

	return VIDEO_CALL(screen->disp->ops->fill, -EOPNOTSUPP, screen->disp,
			  r, g, b, x, y, width, height);
}

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

void uterm_mode_ref(struct uterm_mode *mode)
{
	if (!mode || !mode->ref)
		return;

	++mode->ref;
}

void uterm_mode_unref(struct uterm_mode *mode)
{
	if (!mode || !mode->ref || --mode->ref)
		return;

	VIDEO_CALL(mode->ops->destroy, 0, mode);
	free(mode);
}

struct uterm_mode *uterm_mode_next(struct uterm_mode *mode)
{
	if (!mode)
		return NULL;

	return mode->next;
}

const char *uterm_mode_get_name(const struct uterm_mode *mode)
{
	if (!mode)
		return NULL;

	return VIDEO_CALL(mode->ops->get_name, NULL, mode);
}

unsigned int uterm_mode_get_width(const struct uterm_mode *mode)
{
	if (!mode)
		return 0;

	return VIDEO_CALL(mode->ops->get_width, 0, mode);
}

unsigned int uterm_mode_get_height(const struct uterm_mode *mode)
{
	if (!mode)
		return 0;

	return VIDEO_CALL(mode->ops->get_height, 0, mode);
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

	ret = VIDEO_CALL(disp->ops->init, 0, disp);
	if (ret)
		goto err_free;

	log_info("new display %p", disp);
	*out = disp;
	return 0;

err_free:
	free(disp);
	return ret;
}

void uterm_display_ref(struct uterm_display *disp)
{
	if (!disp || !disp->ref)
		return;

	++disp->ref;
}

void uterm_display_unref(struct uterm_display *disp)
{
	struct uterm_mode *mode;

	if (!disp || !disp->ref || --disp->ref)
		return;

	log_info("free display %p", disp);

	VIDEO_CALL(disp->ops->destroy, 0, disp);

	while ((mode = disp->modes)) {
		disp->modes = mode->next;
		mode->next = NULL;
		uterm_mode_unref(mode);
	}
	free(disp);
}

struct uterm_display *uterm_display_next(struct uterm_display *disp)
{
	if (!disp)
		return NULL;

	return disp->next;
}

struct uterm_mode *uterm_display_get_modes(struct uterm_display *disp)
{
	if (!disp)
		return NULL;

	return disp->modes;
}

struct uterm_mode *uterm_display_get_current(struct uterm_display *disp)
{
	if (!disp)
		return NULL;

	return disp->current_mode;
}

struct uterm_mode *uterm_display_get_default(struct uterm_display *disp)
{
	if (!disp)
		return NULL;

	return disp->default_mode;
}

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

int uterm_display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	if (!disp || !display_is_conn(disp) || display_is_online(disp))
		return -EINVAL;

	if (!mode)
		mode = disp->default_mode;

	return VIDEO_CALL(disp->ops->activate, 0, disp, mode);
}

void uterm_display_deactivate(struct uterm_display *disp)
{
	if (!disp || !display_is_online(disp))
		return;

	VIDEO_CALL(disp->ops->deactivate, 0, disp);
}

int uterm_display_set_dpms(struct uterm_display *disp, int state)
{
	if (!disp || !display_is_conn(disp))
		return -EINVAL;

	return VIDEO_CALL(disp->ops->set_dpms, 0, disp, state);
}

int uterm_display_get_dpms(const struct uterm_display *disp)
{
	if (!disp || !display_is_conn(disp))
		return UTERM_DPMS_OFF;

	return disp->dpms;
}

int uterm_video_new(struct uterm_video **out,
			struct ev_eloop *eloop,
			unsigned int type,
			const char *node)
{
	struct uterm_video *video;
	int ret;
	const struct video_ops *ops;

	if (!out || !eloop)
		return -EINVAL;

	switch (type) {
	case UTERM_VIDEO_DRM:
		if (!drm_available) {
			log_err("DRM backend is not available");
			return -EOPNOTSUPP;
		}
		ops = &drm_video_ops;
		break;
	case UTERM_VIDEO_DUMB:
		if (!dumb_available) {
			log_err("Dumb DRM backend is not available");
			return -EOPNOTSUPP;
		}
		ops = &dumb_video_ops;
		break;
	case UTERM_VIDEO_FBDEV:
		if (!fbdev_available) {
			log_err("FBDEV backend is not available");
			return -EOPNOTSUPP;
		}
		ops = &fbdev_video_ops;
		break;
	default:
		log_err("invalid video backend %d", type);
		return -EINVAL;
	}

	video = malloc(sizeof(*video));
	if (!video)
		return -ENOMEM;
	memset(video, 0, sizeof(*video));
	video->ref = 1;
	video->ops = ops;
	video->eloop = eloop;

	ret = kmscon_hook_new(&video->hook);
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
	kmscon_hook_free(video->hook);
err_free:
	free(video);
	return ret;
}

void uterm_video_ref(struct uterm_video *video)
{
	if (!video || !video->ref)
		return;

	++video->ref;
}

void uterm_video_unref(struct uterm_video *video)
{
	struct uterm_display *disp;

	if (!video || !video->ref || --video->ref)
		return;

	log_info("free device %p", video);

	VIDEO_CALL(video->ops->destroy, 0, video);

	while ((disp = video->displays)) {
		video->displays = disp->next;
		disp->next = NULL;
		uterm_display_unref(disp);
	}

	kmscon_hook_free(video->hook);
	ev_eloop_unref(video->eloop);
	free(video);
}

void uterm_video_segfault(struct uterm_video *video)
{
	if (!video)
		return;

	VIDEO_CALL(video->ops->segfault, 0, video);
}

int uterm_video_use(struct uterm_video *video)
{
	if (!video)
		return -EINVAL;

	return video_do_use(video);
}

struct uterm_display *uterm_video_get_displays(struct uterm_video *video)
{
	if (!video)
		return NULL;

	return video->displays;
}

int uterm_video_register_cb(struct uterm_video *video, uterm_video_cb cb,
				void *data)
{
	if (!video || !cb)
		return -EINVAL;

	return kmscon_hook_add_cast(video->hook, cb, data);
}

void uterm_video_unregister_cb(struct uterm_video *video, uterm_video_cb cb,
				void *data)
{
	if (!video || !cb)
		return;

	kmscon_hook_rm_cast(video->hook, cb, data);
}

void uterm_video_sleep(struct uterm_video *video)
{
	if (!video || !video_is_awake(video))
		return;

	VIDEO_CB(video, NULL, UTERM_SLEEP);
	VIDEO_CALL(video->ops->sleep, 0, video);
}

int uterm_video_wake_up(struct uterm_video *video)
{
	int ret;

	if (!video)
		return -EINVAL;
	if (video_is_awake(video))
		return 0;

	ret = VIDEO_CALL(video->ops->wake_up, 0, video);
	if (ret)
		return ret;

	VIDEO_CB(video, NULL, UTERM_WAKE_UP);
	return 0;
}

bool uterm_video_is_awake(struct uterm_video *video)
{
	return video && video_is_awake(video);
}

void uterm_video_poll(struct uterm_video *video)
{
	if (!video)
		return;

	VIDEO_CALL(video->ops->poll, 0, video);
}
