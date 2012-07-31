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
 * Linux User-Space Terminal
 * Historically, terminals were implemented in kernel-space on linux. With the
 * development of KMS and the linux input-API it is now possible to implement
 * all we need in user-space. This allows to disable the in-kernel CONFIG_VT and
 * similar options and reduce the kernel-overhead.
 * This library provides an API to implement terminals in user-space. This is
 * not limited to classic text-terminals but rather to all kind of applications
 * that need graphical output (with OpenGL) or direct keyboard/mouse/etc. input
 * from the kernel.
 */

#ifndef UTERM_UTERM_H
#define UTERM_UTERM_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include "eloop.h"

/*
 * Virtual Terminals
 * Virtual terminals allow controlling multiple virtual terminals on one real
 * terminal. It is multi-seat capable and fully asynchronous.
 */

struct uterm_vt;
struct uterm_vt_master;

enum uterm_vt_action {
	UTERM_VT_ACTIVATE,
	UTERM_VT_DEACTIVATE,
};

typedef int (*uterm_vt_cb) (struct uterm_vt *vt, unsigned int action,
			    void *data);

int uterm_vt_master_new(struct uterm_vt_master **out,
			struct ev_eloop *eloop);
void uterm_vt_master_ref(struct uterm_vt_master *vtm);
void uterm_vt_master_unref(struct uterm_vt_master *vtm);

int uterm_vt_allocate(struct uterm_vt_master *vt, struct uterm_vt **out,
		      const char *seat, uterm_vt_cb cb, void *data);
void uterm_vt_deallocate(struct uterm_vt *vt);
void uterm_vt_ref(struct uterm_vt *vt);
void uterm_vt_unref(struct uterm_vt *vt);

int uterm_vt_activate(struct uterm_vt *vt);
int uterm_vt_deactivate(struct uterm_vt *vt);

/*
 * Video Control
 * Linux provides 2 famous ways to access the video hardware: fbdev and drm
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
 * If VT support is not available (very unlikely) you need other ways to switch
 * between applications.
 *
 * The main object by this API is uterm_video. This object attaches to a single
 * graphics card via DRM or on a single frambuffer via fbdev. Many DRM drivers
 * also provide an fbdev driver so you must go sure not to write to both
 * simulatneously. Use "UTERM_VIDEO_DRM" to scan for DRM devices. Otherwise,
 * fbdev is used. DRM is the recommended way. Use fbdev only on embedded devices
 * which do not come with an DRM driver.
 * The uterm_video object scans for graphic-cards and connected displays. Each
 * display is represented as a uterm_display object. The uterm_video object is
 * hotplug-capable so it reports if a display is connected or disconnected.
 * Each uterm_display object can be activated/deactivated independently of the
 * other displays. To draw to a display you need to create a uterm_screen object
 * and add your display to the screen. The screen object allows to spread a
 * single screen onto multiple displays. Currently, the uterm_screen object
 * allows only one display per screen but we may extend this in the future.
 *
 * If you are using fbdev, you *must* correctly destroy your uterm_video object
 * and also call uterm_video_segfault() if you abnormally abort your
 * application. Otherwise your video device remains in undefined state and other
 * applications might not display correctly.
 * If you use DRM, the same operations are recommended but not required as the
 * kernel can correctly reset video devices on its own.
 */

struct uterm_screen;
struct uterm_mode;
struct uterm_display;
struct uterm_video;

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

enum uterm_video_type {
	UTERM_VIDEO_DRM,
	UTERM_VIDEO_DUMB,
	UTERM_VIDEO_FBDEV,
};

enum uterm_video_action {
	UTERM_WAKE_UP,
	UTERM_SLEEP,
	UTERM_NEW,
	UTERM_GONE,
};

struct uterm_video_hotplug {
	struct uterm_display *display;
	int action;
};

enum uterm_video_format {
	UTERM_FORMAT_MONO,
	UTERM_FORMAT_GREY,
	UTERM_FORMAT_XRGB32,
};

struct uterm_video_buffer {
	unsigned int width;
	unsigned int height;
	unsigned int stride;
	unsigned int format;
	uint8_t *data;
};

typedef void (*uterm_video_cb) (struct uterm_video *video,
				struct uterm_video_hotplug *arg,
				void *data);

/* misc */

const char *uterm_dpms_to_name(int dpms);

/* screen interface */

int uterm_screen_new_single(struct uterm_screen **out,
				struct uterm_display *disp);
void uterm_screen_ref(struct uterm_screen *screen);
void uterm_screen_unref(struct uterm_screen *screen);

unsigned int uterm_screen_width(struct uterm_screen *screen);
unsigned int uterm_screen_height(struct uterm_screen *screen);

int uterm_screen_use(struct uterm_screen *screen);
int uterm_screen_swap(struct uterm_screen *screen);
int uterm_screen_blit(struct uterm_screen *screen,
		      const struct uterm_video_buffer *buf,
		      unsigned int x, unsigned int y);
int uterm_screen_fill(struct uterm_screen *screen,
		      uint8_t r, uint8_t g, uint8_t b,
		      unsigned int x, unsigned int y,
		      unsigned int width, unsigned int height);

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

struct uterm_mode *uterm_display_get_modes(struct uterm_display *disp);
struct uterm_mode *uterm_display_get_current(struct uterm_display *disp);
struct uterm_mode *uterm_display_get_default(struct uterm_display *disp);

int uterm_display_get_state(struct uterm_display *disp);
int uterm_display_activate(struct uterm_display *disp, struct uterm_mode *mode);
void uterm_display_deactivate(struct uterm_display *disp);
int uterm_display_set_dpms(struct uterm_display *disp, int state);
int uterm_display_get_dpms(const struct uterm_display *disp);

/* video interface */

int uterm_video_new(struct uterm_video **out,
			struct ev_eloop *eloop,
			unsigned int type,
			const char *node);
void uterm_video_ref(struct uterm_video *video);
void uterm_video_unref(struct uterm_video *video);

void uterm_video_segfault(struct uterm_video *video);
int uterm_video_use(struct uterm_video *video);
struct uterm_display *uterm_video_get_displays(struct uterm_video *video);
int uterm_video_register_cb(struct uterm_video *video, uterm_video_cb cb,
				void *data);
void uterm_video_unregister_cb(struct uterm_video *video, uterm_video_cb cb,
				void *data);

void uterm_video_sleep(struct uterm_video *video);
int uterm_video_wake_up(struct uterm_video *video);
bool uterm_video_is_awake(struct uterm_video *video);
void uterm_video_poll(struct uterm_video *video);

/*
 * Input Devices
 * This input object can combine multiple linux input devices into a single
 * device and notifies the application about events. It has several different
 * keyboard backends so the full XKB feature set is available.
 */

struct uterm_input;

enum uterm_input_modifier {
	UTERM_SHIFT_MASK	= (1 << 0),
	UTERM_LOCK_MASK		= (1 << 1),
	UTERM_CONTROL_MASK	= (1 << 2),
	UTERM_MOD1_MASK		= (1 << 3),
	UTERM_MOD2_MASK		= (1 << 4),
	UTERM_MOD3_MASK		= (1 << 5),
	UTERM_MOD4_MASK		= (1 << 6),
	UTERM_MOD5_MASK		= (1 << 7),
};

#define UTERM_INPUT_INVALID 0xffffffff

struct uterm_input_event {
	uint16_t keycode;	/* linux keycode - KEY_* - linux/input.h */
	uint32_t keysym;	/* X keysym - XK_* - X11/keysym.h */
	unsigned int mods;	/* active modifiers - uterm_modifier mask */
	uint32_t unicode;	/* ucs4 unicode value or UTERM_INPUT_INVALID */
};

#define UTERM_INPUT_HAS_MODS(_ev, _mods) (((_ev)->mods & (_mods)) == (_mods))

typedef void (*uterm_input_cb) (struct uterm_input *input,
				struct uterm_input_event *ev,
				void *data);

int uterm_input_new(struct uterm_input **out, struct ev_eloop *eloop);
void uterm_input_ref(struct uterm_input *input);
void uterm_input_unref(struct uterm_input *input);

void uterm_input_add_dev(struct uterm_input *input, const char *node);
void uterm_input_remove_dev(struct uterm_input *input, const char *node);

int uterm_input_register_cb(struct uterm_input *input,
				uterm_input_cb cb,
				void *data);
void uterm_input_unregister_cb(struct uterm_input *input,
				uterm_input_cb cb,
				void *data);

void uterm_input_sleep(struct uterm_input *input);
void uterm_input_wake_up(struct uterm_input *input);
bool uterm_input_is_awake(struct uterm_input *input);

void uterm_input_keysym_to_string(struct uterm_input *input,
				  uint32_t keysym, char *str, size_t size);

/*
 * System Monitor
 * This watches the system for new seats, graphics devices or other devices that
 * are used by terminals.
 */

struct uterm_monitor;
struct uterm_monitor_seat;
struct uterm_monitor_dev;

enum uterm_monitor_event_type {
	UTERM_MONITOR_NEW_SEAT,
	UTERM_MONITOR_FREE_SEAT,
	UTERM_MONITOR_NEW_DEV,
	UTERM_MONITOR_FREE_DEV,
	UTERM_MONITOR_HOTPLUG_DEV,
};

enum uterm_monitor_dev_type {
	UTERM_MONITOR_DRM,
	UTERM_MONITOR_FBDEV,
	UTERM_MONITOR_INPUT,
};

struct uterm_monitor_event {
	unsigned int type;

	struct uterm_monitor_seat *seat;
	const char *seat_name;
	void *seat_data;

	struct uterm_monitor_dev *dev;
	unsigned int dev_type;
	const char *dev_node;
	void *dev_data;
};

typedef void (*uterm_monitor_cb) (struct uterm_monitor *mon,
					struct uterm_monitor_event *event,
					void *data);

int uterm_monitor_new(struct uterm_monitor **out,
			struct ev_eloop *eloop,
			uterm_monitor_cb cb,
			void *data);
void uterm_monitor_ref(struct uterm_monitor *mon);
void uterm_monitor_unref(struct uterm_monitor *mon);
void uterm_monitor_scan(struct uterm_monitor *mon);

void uterm_monitor_set_seat_data(struct uterm_monitor_seat *seat, void *data);
void uterm_monitor_set_dev_data(struct uterm_monitor_dev *dev, void *data);

#endif /* UTERM_UTERM_H */
