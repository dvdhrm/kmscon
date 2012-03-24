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

/* Internal definitions */

#ifndef UTERM_INTERNAL_H
#define UTERM_INTERNAL_H

#include <inttypes.h>
#include <libudev.h>
#include <stdbool.h>
#include <stdlib.h>
#include "eloop.h"
#include "misc.h"
#include "uterm.h"

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
	int (*use) (struct uterm_display *disp);
	int (*swap) (struct uterm_display *disp);
};

struct video_ops {
	int (*init) (struct uterm_video *video);
	void (*destroy) (struct uterm_video *video);
	void (*segfault) (struct uterm_video *video);
	int (*poll) (struct uterm_video *video, int mask);
	void (*sleep) (struct uterm_video *video);
	int (*wake_up) (struct uterm_video *video);
};

#define VIDEO_CALL(func, els, ...) (func ? func(__VA_ARGS__) : els)

/* drm */

#ifdef UTERM_HAVE_DRM

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct drm_mode {
	drmModeModeInfo info;
};

struct drm_rb {
	struct gbm_bo *bo;
	uint32_t fb;
	EGLImageKHR image;
	GLuint rb;
};

struct drm_display {
	uint32_t conn_id;
	int crtc_id;
	drmModeCrtc *saved_crtc;

	int current_rb;
	struct drm_rb rb[2];
	GLuint fb;
};

struct drm_video {
	int id;
	int fd;
	struct ev_fd *efd;
	struct gbm_device *gbm;
	EGLDisplay *disp;
	EGLContext *ctx;
};

static const bool drm_available = true;
extern const struct mode_ops drm_mode_ops;
extern const struct display_ops drm_display_ops;
extern const struct video_ops drm_video_ops;

#else /* !UTERM_HAVE_DRM */

struct drm_mode {
	int unused;
};

struct drm_display {
	int unused;
};

struct drm_video {
	int unused;
};

static const bool drm_available = false;
static const struct mode_ops drm_mode_ops;
static const struct display_ops drm_display_ops;
static const struct video_ops drm_video_ops;

#endif /* UTERM_HAVE_DRM */

/* fbdev */

#ifdef UTERM_HAVE_FBDEV

#include <linux/fb.h>

struct fbdev_mode {
	int unused;
};

struct fbdev_display {
	int id;
	char *node;
	int fd;

	size_t len;
	void *map;
	unsigned int stride;

	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	unsigned int rate;
};

struct fbdev_video {
	int unused;
};

static const bool fbdev_available = true;
extern const struct mode_ops fbdev_mode_ops;
extern const struct display_ops fbdev_display_ops;
extern const struct video_ops fbdev_video_ops;

#else /* !UTERM_HAVE_FBDEV */

struct fbdev_mode {
	int unused;
};

struct fbdev_display {
	int unused;
};

struct fbdev_video {
	int unused;
};

static const bool fbdev_available = false;
static const struct mode_ops fbdev_mode_ops;
static const struct display_ops fbdev_display_ops;
static const struct video_ops fbdev_video_ops;

#endif /* UTERM_HAVE_FBDEV */

/* uterm_screen */

struct uterm_screen {
	unsigned long ref;
	struct uterm_display *disp;
};

/* uterm_mode */

struct uterm_mode {
	unsigned long ref;
	struct uterm_mode *next;

	const struct mode_ops *ops;
	union {
		struct drm_mode drm;
		struct fbdev_mode fbdev;
	};
};

int mode_new(struct uterm_mode **out, const struct mode_ops *ops);

/* uterm_display */

#define DISPLAY_ONLINE		0x01
#define DISPLAY_VSYNC		0x02
#define DISPLAY_AVAILABLE	0x04
#define DISPLAY_OPEN		0x08

struct uterm_display {
	unsigned long ref;
	unsigned int flags;
	struct uterm_display *next;
	struct uterm_video *video;

	struct uterm_mode *modes;
	struct uterm_mode *default_mode;
	struct uterm_mode *current_mode;
	int dpms;

	const struct display_ops *ops;
	union {
		struct drm_display drm;
		struct fbdev_display fbdev;
	};
};

int display_new(struct uterm_display **out, const struct display_ops *ops);

static inline bool display_is_conn(const struct uterm_display *disp)
{
	return disp->video;
}

static inline bool display_is_online(const struct uterm_display *disp)
{
	return display_is_conn(disp) && (disp->flags & DISPLAY_ONLINE);
}

/* uterm_video */

#define VIDEO_AWAKE		0x01
#define VIDEO_HOTPLUG		0x02

struct uterm_video {
	unsigned long ref;
	unsigned int flags;
	struct ev_eloop *eloop;

	struct udev *udev;
	struct udev_monitor *umon;
	struct ev_fd *umon_fd;

	struct uterm_display *displays;
	struct kmscon_hook *hook;

	const struct video_ops *ops;
	union {
		struct drm_video drm;
		struct fbdev_video fbdev;
	};
};

static inline bool video_is_awake(const struct uterm_video *video)
{
	return video->flags & VIDEO_AWAKE;
}

static inline bool video_need_hotplug(const struct uterm_video *video)
{
	return video->flags & VIDEO_HOTPLUG;
}

#define VIDEO_CB(vid, disp, act) kmscon_hook_call((vid)->hook, (vid), \
		&(struct uterm_video_hotplug){ \
			.display = (disp), \
			.action = (act), \
		})

#endif /* UTERM_INTERNAL_H */
