/*
 * uterm_video - Linux User-Space Terminal Video Handling
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
 * Linux provides 2 famous ways to access the video hardware: FBDEV and DRM.
 * fbdev is the older one of both and is simply a mmap() of the framebuffer into
 * main memory. It does not allow 3D acceleration and if you need 2D
 * acceleration you should use libraries like cairo to draw into the framebuffer
 * provided by this library.
 * DRM is the new approach which provides 3D acceleration with mesa. It allows
 * much more configuration as fbdev and is the recommended way to access video
 * hardware on modern computers.
 * Modern mesa provides 3D acceleration on fbdev, too. This is used in systems
 * like Android. This will allow us to provide an fbdev backend here.
 *
 * Famous linux graphics systems like X.Org/X11 or Wayland use fbdev or DRM
 * internally to access the video hardware. This API allows low-level access to
 * fbdev and DRM without the need of X.Org/X11 or Wayland. If VT support is
 * enabled in your kernel, each application can run on a different VT. For
 * instance, X.Org may run on VT-7, Wayland on VT-8, your application on VT-9
 * and default consoles on VT-1 to VT-6. You can switch between them with
 * ctrl-alt-F1-F12.
 * If VT support is not available you need other ways to switch between
 * applications. See uterm_vt for more.
 */

#ifndef UTERM_UTERM_VIDEO_H
#define UTERM_UTERM_VIDEO_H

#include <eloop.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

struct uterm_mode;
struct uterm_display;
struct uterm_video;
struct uterm_video_module;

enum uterm_display_state {
	UTERM_DISPLAY_ACTIVE,
	UTERM_DISPLAY_ASLEEP,
	UTERM_DISPLAY_INACTIVE,
	UTERM_DISPLAY_GONE,
};

enum uterm_display_dpms {
	UTERM_DPMS_ON,
	UTERM_DPMS_STANDBY,
	UTERM_DPMS_SUSPEND,
	UTERM_DPMS_OFF,
	UTERM_DPMS_UNKNOWN,
};

enum uterm_video_action {
	UTERM_WAKE_UP,
	UTERM_SLEEP,
	UTERM_NEW,
	UTERM_GONE,
	UTERM_REFRESH,
};

struct uterm_video_hotplug {
	struct uterm_display *display;
	int action;
};

enum uterm_display_action {
	UTERM_PAGE_FLIP,
};

struct uterm_display_event {
	int action;
};

enum uterm_video_format {
	UTERM_FORMAT_GREY	= 0x01,
	UTERM_FORMAT_XRGB32	= 0x02,
	UTERM_FORMAT_RGB16	= 0x04,
};

struct uterm_video_buffer {
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	unsigned int format;
	uint8_t *data;
};

struct uterm_video_blend_req {
	const struct uterm_video_buffer *buf;
	unsigned int x;
	unsigned int y;
	uint8_t fr;
	uint8_t fg;
	uint8_t fb;
	uint8_t br;
	uint8_t bg;
	uint8_t bb;
};

typedef void (*uterm_video_cb) (struct uterm_video *video,
				struct uterm_video_hotplug *arg,
				void *data);
typedef void (*uterm_display_cb) (struct uterm_display *disp,
				  struct uterm_display_event *arg,
				  void *data);

/* misc */

const char *uterm_dpms_to_name(int dpms);
bool uterm_video_available(const struct uterm_video_module *mod);

/* display modes interface */

void uterm_mode_ref(struct uterm_mode *mode);
void uterm_mode_unref(struct uterm_mode *mode);
struct uterm_mode *uterm_mode_next(struct uterm_mode *mode);

const char *uterm_mode_get_name(const struct uterm_mode *mode);
unsigned int uterm_mode_get_width(const struct uterm_mode *mode);
unsigned int uterm_mode_get_height(const struct uterm_mode *mode);

/* display interface */

void uterm_display_ref(struct uterm_display *disp);
void uterm_display_unref(struct uterm_display *disp);
struct uterm_display *uterm_display_next(struct uterm_display *disp);

int uterm_display_register_cb(struct uterm_display *disp, uterm_display_cb cb,
			      void *data);
void uterm_display_unregister_cb(struct uterm_display *disp,
				 uterm_display_cb cb, void *data);

struct uterm_mode *uterm_display_get_modes(struct uterm_display *disp);
struct uterm_mode *uterm_display_get_current(struct uterm_display *disp);
struct uterm_mode *uterm_display_get_default(struct uterm_display *disp);

int uterm_display_get_state(struct uterm_display *disp);
int uterm_display_activate(struct uterm_display *disp, struct uterm_mode *mode);
void uterm_display_deactivate(struct uterm_display *disp);
int uterm_display_set_dpms(struct uterm_display *disp, int state);
int uterm_display_get_dpms(const struct uterm_display *disp);

int uterm_display_use(struct uterm_display *disp, bool *opengl);
int uterm_display_get_buffers(struct uterm_display *disp,
			      struct uterm_video_buffer *buffer,
			      unsigned int formats);
int uterm_display_swap(struct uterm_display *disp, bool immediate);
bool uterm_display_is_swapping(struct uterm_display *disp);

int uterm_display_fill(struct uterm_display *disp,
		       uint8_t r, uint8_t g, uint8_t b,
		       unsigned int x, unsigned int y,
		       unsigned int width, unsigned int height);
int uterm_display_blit(struct uterm_display *disp,
		       const struct uterm_video_buffer *buf,
		       unsigned int x, unsigned int y);
int uterm_display_fake_blend(struct uterm_display *disp,
			     const struct uterm_video_buffer *buf,
			     unsigned int x, unsigned int y,
			     uint8_t fr, uint8_t fg, uint8_t fb,
			     uint8_t br, uint8_t bg, uint8_t bb);
int uterm_display_fake_blendv(struct uterm_display *disp,
			      const struct uterm_video_blend_req *req,
			      size_t num);

/* video interface */

int uterm_video_new(struct uterm_video **out, struct ev_eloop *eloop,
		    const char *node, const struct uterm_video_module *mod);
void uterm_video_ref(struct uterm_video *video);
void uterm_video_unref(struct uterm_video *video);

void uterm_video_segfault(struct uterm_video *video);
struct uterm_display *uterm_video_get_displays(struct uterm_video *video);
int uterm_video_register_cb(struct uterm_video *video, uterm_video_cb cb,
			    void *data);
void uterm_video_unregister_cb(struct uterm_video *video, uterm_video_cb cb,
			       void *data);

void uterm_video_sleep(struct uterm_video *video);
int uterm_video_wake_up(struct uterm_video *video);
bool uterm_video_is_awake(struct uterm_video *video);
void uterm_video_poll(struct uterm_video *video);

/* external modules */

#ifdef BUILD_ENABLE_VIDEO_FBDEV
extern const struct uterm_video_module *UTERM_VIDEO_FBDEV;
#else
#define UTERM_VIDEO_FBDEV NULL
#endif

#ifdef BUILD_ENABLE_VIDEO_DRM2D
extern const struct uterm_video_module *UTERM_VIDEO_DRM2D;
#else
#define UTERM_VIDEO_DRM2D NULL
#endif

#ifdef BUILD_ENABLE_VIDEO_DRM3D
extern const struct uterm_video_module *UTERM_VIDEO_DRM3D;
#else
#define UTERM_VIDEO_DRM3D NULL
#endif

#endif /* UTERM_UTERM_VIDEO_H */
