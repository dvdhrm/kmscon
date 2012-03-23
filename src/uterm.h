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
 * This API is divided into categories. Each sub-API implements one aspect.
 * Currently, only video output is available. Other sub-APIs will follow.
 */

#ifndef UTERM_UTERM_H
#define UTERM_UTERM_H

#include <stdbool.h>
#include <stdlib.h>
#include "eloop.h"

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
 * like Android. This will allow us to provide an fbdev backend here, however,
 * we haven't implemented this yet, as this requires the Android software stack
 * on your system which is really not what we want.
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
 * The main object by this API is uterm_video. This object scans the system for
 * video devices, either fbdev or drm. You cannot use both simulatneously,
 * though. This wouldn't make sense as often both fbdev and drm devices refer to
 * the same output. Use "UTERM_VIDEO_DRM" to scan for DRM devices. Otherwise,
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
	UTERM_VIDEO_FBDEV,
};

/* misc */

const char *uterm_dpms_to_name(int dpms);

/* screen interface */

int uterm_screen_new_single(struct uterm_screen **out,
				struct uterm_display *disp);
void uterm_screen_ref(struct uterm_screen *screen);
void uterm_screen_unref(struct uterm_screen *screen);

int uterm_screen_use(struct uterm_screen *screen);
int uterm_screen_swap(struct uterm_screen *screen);

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
			int type,
			struct ev_eloop *eloop);
void uterm_video_ref(struct uterm_video *video);
void uterm_video_unref(struct uterm_video *video);

void uterm_video_segfault(struct uterm_video *video);
struct uterm_display *uterm_video_get_displays(struct uterm_video *video);

void uterm_video_sleep(struct uterm_video *video);
int uterm_video_wake_up(struct uterm_video *video);
bool uterm_video_is_awake(struct uterm_video *video);

#endif /* UTERM_UTERM_H */
