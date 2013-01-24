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

/* Internal definitions */

#ifndef UTERM_VIDEO_INTERNAL_H
#define UTERM_VIDEO_INTERNAL_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "uterm_video.h"

/* backend-operations */

struct mode_ops {
	int (*init) (struct uterm_mode *mode);
	void (*destroy) (struct uterm_mode *mode);
	const char *(*get_name) (const struct uterm_mode *mode);
	unsigned int (*get_width) (const struct uterm_mode *mode);
	unsigned int (*get_height) (const struct uterm_mode *mode);
};

struct display_ops {
	int (*init) (struct uterm_display *display);
	void (*destroy) (struct uterm_display *display);
	int (*activate) (struct uterm_display *disp, struct uterm_mode *mode);
	void (*deactivate) (struct uterm_display *disp);
	int (*set_dpms) (struct uterm_display *disp, int state);
	int (*use) (struct uterm_display *disp, bool *opengl);
	int (*get_buffers) (struct uterm_display *disp,
			    struct uterm_video_buffer *buffer,
			    unsigned int formats);
	int (*swap) (struct uterm_display *disp, bool immediate);
	int (*blit) (struct uterm_display *disp,
		     const struct uterm_video_buffer *buf,
		     unsigned int x, unsigned int y);
	int (*fake_blendv) (struct uterm_display *disp,
			    const struct uterm_video_blend_req *req,
			    size_t num);
	int (*fill) (struct uterm_display *disp,
		     uint8_t r, uint8_t g, uint8_t b, unsigned int x,
		     unsigned int y, unsigned int width, unsigned int height);
};

struct video_ops {
	int (*init) (struct uterm_video *video, const char *node);
	void (*destroy) (struct uterm_video *video);
	void (*segfault) (struct uterm_video *video);
	int (*poll) (struct uterm_video *video);
	void (*sleep) (struct uterm_video *video);
	int (*wake_up) (struct uterm_video *video);
};

struct uterm_video_module {
	const struct video_ops *ops;
};

#define VIDEO_CALL(func, els, ...) (func ? func(__VA_ARGS__) : els)

/* uterm_mode */

struct uterm_mode {
	struct shl_dlist list;
	unsigned long ref;
	struct uterm_display *disp;

	const struct mode_ops *ops;
	void *data;
};

int mode_new(struct uterm_mode **out, const struct mode_ops *ops);
int uterm_mode_bind(struct uterm_mode *mode, struct uterm_display *disp);
void uterm_mode_unbind(struct uterm_mode *mode);

/* uterm_display */

#define DISPLAY_ONLINE		0x01
#define DISPLAY_VSYNC		0x02
#define DISPLAY_AVAILABLE	0x04
#define DISPLAY_OPEN		0x08
#define DISPLAY_DBUF		0x10
#define DISPLAY_DITHERING	0x20
#define DISPLAY_PFLIP		0x40

struct uterm_display {
	struct shl_dlist list;
	unsigned long ref;
	unsigned int flags;
	struct uterm_video *video;

	struct shl_hook *hook;
	struct shl_dlist modes;
	struct uterm_mode *default_mode;
	struct uterm_mode *current_mode;
	int dpms;

	bool vblank_scheduled;
	struct itimerspec vblank_spec;
	struct ev_timer *vblank_timer;

	const struct display_ops *ops;
	void *data;
};

int display_new(struct uterm_display **out, const struct display_ops *ops);
void display_set_vblank_timer(struct uterm_display *disp,
			      unsigned int msecs);
int display_schedule_vblank_timer(struct uterm_display *disp);
int uterm_display_bind(struct uterm_display *disp, struct uterm_video *video);
void uterm_display_unbind(struct uterm_display *disp);

#define DISPLAY_CB(disp, act) shl_hook_call((disp)->hook, (disp), \
		&(struct uterm_display_event){ \
			.action = (act), \
		})

static inline bool display_is_online(const struct uterm_display *disp)
{
	return disp->video && (disp->flags & DISPLAY_ONLINE);
}

/* uterm_video */

#define VIDEO_AWAKE		0x01
#define VIDEO_HOTPLUG		0x02

struct uterm_video {
	unsigned long ref;
	unsigned int flags;
	struct ev_eloop *eloop;

	struct shl_dlist displays;
	struct shl_hook *hook;

	const struct uterm_video_module *mod;
	const struct video_ops *ops;
	void *data;
};

static inline bool video_is_awake(const struct uterm_video *video)
{
	return video->flags & VIDEO_AWAKE;
}

static inline bool video_need_hotplug(const struct uterm_video *video)
{
	return video->flags & VIDEO_HOTPLUG;
}

#define VIDEO_CB(vid, disp, act) shl_hook_call((vid)->hook, (vid), \
		&(struct uterm_video_hotplug){ \
			.display = (disp), \
			.action = (act), \
		})

#if defined(BUILD_ENABLE_VIDEO_DRM3D) || defined(BUILD_ENABLE_VIDEO_DRM2D)

#include <xf86drm.h>

static inline bool video_drm_available(void)
{
	return drmAvailable();
}

#else

static inline bool video_drm_available(void)
{
	return false;
}

#endif

#endif /* UTERM_VIDEO_INTERNAL_H */
