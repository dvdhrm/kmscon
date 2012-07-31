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
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include "eloop.h"
#include "static_misc.h"
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
	int (*blit) (struct uterm_display *disp,
		     const struct uterm_video_buffer *buf,
		     unsigned int x, unsigned int y);
};

struct video_ops {
	int (*init) (struct uterm_video *video, const char *node);
	void (*destroy) (struct uterm_video *video);
	void (*segfault) (struct uterm_video *video);
	int (*use) (struct uterm_video *video);
	int (*poll) (struct uterm_video *video);
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

/* dumb drm */

#ifdef UTERM_HAVE_DUMB

#include <xf86drm.h>
#include <xf86drmMode.h>

struct dumb_mode {
	drmModeModeInfo info;
};

struct dumb_rb {
	uint32_t fb;
	uint32_t handle;
	uint32_t stride;
	uint64_t size;

	void *map;
};

struct dumb_display {
	uint32_t conn_id;
	int crtc_id;
	drmModeCrtc *saved_crtc;

	int current_rb;
	struct dumb_rb rb[2];
};

struct dumb_video {
	int fd;
	struct ev_fd *efd;
};

static const bool dumb_available = true;
extern const struct mode_ops dumb_mode_ops;
extern const struct display_ops dumb_display_ops;
extern const struct video_ops dumb_video_ops;

#else /* !UTERM_HAVE_DUMB */

struct dumb_mode {
	int unused;
};

struct dumb_display {
	int unused;
};

struct dumb_video {
	int unused;
};

static const bool dumb_available = false;
static const struct mode_ops dumb_mode_ops;
static const struct display_ops dumb_display_ops;
static const struct video_ops dumb_video_ops;

#endif /* UTERM_HAVE_DUMB */

/* fbdev */

#ifdef UTERM_HAVE_FBDEV

#include <linux/fb.h>

struct fbdev_mode {
	unsigned int width;
	unsigned int height;
};

struct fbdev_display {
	char *node;
	int fd;

	struct fb_fix_screeninfo finfo;
	struct fb_var_screeninfo vinfo;
	unsigned int rate;

	unsigned int bufid;
	size_t xres;
	size_t yres;
	size_t len;
	uint8_t *map;
	unsigned int stride;
	unsigned int bpp;
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
		struct dumb_mode dumb;
		struct fbdev_mode fbdev;
	};
};

int mode_new(struct uterm_mode **out, const struct mode_ops *ops);

/* uterm_display */

#define DISPLAY_ONLINE		0x01
#define DISPLAY_VSYNC		0x02
#define DISPLAY_AVAILABLE	0x04
#define DISPLAY_OPEN		0x08
#define DISPLAY_DBUF		0x10

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
		struct dumb_display dumb;
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

	struct uterm_display *displays;
	struct kmscon_hook *hook;

	const struct video_ops *ops;
	union {
		struct drm_video drm;
		struct dumb_video dumb;
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

static inline int video_do_use(struct uterm_video *video)
{
	return VIDEO_CALL(video->ops->use, -EOPNOTSUPP, video);
}

static inline bool input_bit_is_set(const unsigned long *array, int bit)
{
	return !!(array[bit / LONG_BIT] & (1LL << (bit % LONG_BIT)));
}

/* kbd API */

struct kbd_desc;
struct kbd_dev;

struct kbd_desc_ops {
	int (*init) (struct kbd_desc **out, const char *layout,
		     const char *variant, const char *options);
	void (*ref) (struct kbd_desc *desc);
	void (*unref) (struct kbd_desc *desc);
	int (*alloc) (struct kbd_desc *desc, struct kbd_dev **out);
	void (*keysym_to_string) (uint32_t keysym, char *str, size_t size);
};

struct kbd_dev_ops {
	void (*ref) (struct kbd_dev *dev);
	void (*unref) (struct kbd_dev *dev);
	void (*reset) (struct kbd_dev *dev, const unsigned long *ledbits);
	int (*process) (struct kbd_dev *dev, uint16_t state, uint16_t code,
			struct uterm_input_event *out);
};

struct plain_desc {
	int unused;
};

struct plain_dev {
	unsigned int mods;
};

static const bool plain_available = true;
extern const struct kbd_desc_ops plain_desc_ops;
extern const struct kbd_dev_ops plain_dev_ops;

#ifdef UTERM_HAVE_XKBCOMMON

struct uxkb_desc {
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;
};

struct uxkb_dev {
	struct xkb_state *state;
};

static const bool uxkb_available = true;
extern const struct kbd_desc_ops uxkb_desc_ops;
extern const struct kbd_dev_ops uxkb_dev_ops;

#else /* !UTERM_HAVE_XKBCOMMON */

struct uxkb_desc {
	int unused;
};

struct uxkb_dev {
	int unused;
};

static const bool xkb_available = false;
static const struct kbd_desc_ops uxkb_desc_ops;
static const struct kbd_dev_ops uxkb_dev_ops;

#endif /* UTERM_HAVE_XKBCOMMON */

struct kbd_desc {
	unsigned long ref;
	const struct kbd_desc_ops *ops;

	union {
		struct plain_desc plain;
		struct uxkb_desc uxkb;
	};
};

struct kbd_dev {
	unsigned long ref;
	struct kbd_desc *desc;
	const struct kbd_dev_ops *ops;

	union {
		struct plain_dev plain;
		struct uxkb_dev uxkb;
	};
};

enum kbd_mode {
	KBD_PLAIN,
	KBD_UXKB,
};

static inline int kbd_desc_new(struct kbd_desc **out, const char *layout,
			       const char *variant, const char *options,
			       unsigned int mode)
{
	const struct kbd_desc_ops *ops;

	switch (mode) {
	case KBD_UXKB:
		if (!uxkb_available) {
			log_error("XKB KBD backend not available");
			return -EOPNOTSUPP;
		}
		ops = &uxkb_desc_ops;
		break;
	case KBD_PLAIN:
		if (!plain_available) {
			log_error("plain KBD backend not available");
			return -EOPNOTSUPP;
		}
		ops = &plain_desc_ops;
		break;
	default:
		log_error("unknown KBD backend %u", mode);
		return -EINVAL;
	}

	return ops->init(out, layout, variant, options);
}

static inline void kbd_desc_ref(struct kbd_desc *desc)
{
	if (!desc)
		return;

	return desc->ops->ref(desc);
}

static inline void kbd_desc_unref(struct kbd_desc *desc)
{
	if (!desc)
		return;

	return desc->ops->unref(desc);
}

static inline int kbd_desc_alloc(struct kbd_desc *desc, struct kbd_dev **out)
{
	if (!desc)
		return -EINVAL;

	return desc->ops->alloc(desc, out);
}

static inline void kbd_desc_keysym_to_string(struct kbd_desc *desc,
					     uint32_t keysym,
					     char *str, size_t size)
{
	if (!desc)
		return;

	return desc->ops->keysym_to_string(keysym, str, size);
}

static inline void kbd_dev_ref(struct kbd_dev *dev)
{
	if (!dev)
		return;

	return dev->ops->ref(dev);
}

static inline void kbd_dev_unref(struct kbd_dev *dev)
{
	if (!dev)
		return;

	return dev->ops->unref(dev);
}

static inline void kbd_dev_reset(struct kbd_dev *dev,
				 const unsigned long *ledbits)
{
	if (!dev)
		return;

	return dev->ops->reset(dev, ledbits);
}

static inline int kbd_dev_process(struct kbd_dev *dev,
				  uint16_t key_state,
				  uint16_t code,
				  struct uterm_input_event *out)
{
	if (!dev)
		return -EINVAL;

	return dev->ops->process(dev, key_state, code, out);
}

#endif /* UTERM_INTERNAL_H */
