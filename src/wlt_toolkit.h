/*
 * wlt - Toolkit Helper
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Wayland Terminal toolkit helpers
 */

#ifndef WLT_TOOLKIT_H
#define WLT_TOOLKIT_H

#include <stdbool.h>
#include <stdlib.h>
#include "eloop.h"

struct wlt_display;
struct wlt_window;
struct wlt_widget;

enum wlt_display_event {
	WLT_DISPLAY_HUP,
	WLT_DISPLAY_READY,
};

struct wlt_rect {
	unsigned int x;
	unsigned int y;
	unsigned int width;
	unsigned int height;
};

struct wlt_shm_buffer {
	uint8_t *data;
	unsigned int width;
	unsigned int height;
	unsigned int stride;
};

enum cursor_type {
	WLT_CURSOR_NONE,
	WLT_CURSOR_TOP,
	WLT_CURSOR_BOTTOM,
	WLT_CURSOR_LEFT,
	WLT_CURSOR_RIGHT,
	WLT_CURSOR_TOP_LEFT,
	WLT_CURSOR_TOP_RIGHT,
	WLT_CURSOR_BOTTOM_LEFT,
	WLT_CURSOR_BOTTOM_RIGHT,
	WLT_CURSOR_DRAGGING,
	WLT_CURSOR_LEFT_PTR,
	WLT_CURSOR_IBEAM,
	WLT_CURSOR_NUM,
};

#define WLT_WINDOW_MAXIMIZED		0x01
#define WLT_WINDOW_FULLSCREEN		0x02

typedef void (*wlt_display_cb) (struct wlt_display *disp,
				unsigned int event,
				void *data);
typedef void (*wlt_window_close_cb) (struct wlt_window *wnd, void *data);
typedef void (*wlt_widget_redraw_cb) (struct wlt_widget *widget,
				      unsigned int flags,
				      void *data);
typedef void (*wlt_widget_destroy_cb) (struct wlt_widget *widget,
				       void *data);
typedef void (*wlt_widget_prepare_resize_cb) (struct wlt_widget *widget,
					      unsigned int flags,
					      unsigned int width,
					      unsigned int height,
					      unsigned int *min_width,
					      unsigned int *min_height,
					      unsigned int *new_width,
					      unsigned int *new_height,
					      void *data);
typedef void (*wlt_widget_resize_cb) (struct wlt_widget *widget,
				      unsigned int flags,
				      struct wlt_rect *allocation,
				      void *data);
typedef void (*wlt_widget_pointer_enter_cb) (struct wlt_widget *widget,
					     unsigned int x,
					     unsigned int y,
					     void *data);
typedef void (*wlt_widget_pointer_leave_cb) (struct wlt_widget *widget,
					     void *data);
typedef void (*wlt_widget_pointer_motion_cb) (struct wlt_widget *widget,
					      unsigned int x,
					      unsigned int y,
					      void *data);
typedef void (*wlt_widget_pointer_button_cb) (struct wlt_widget *widget,
					      uint32_t button,
					      uint32_t state,
					      void *data);
typedef bool (*wlt_widget_keyboard_cb) (struct wlt_widget *widget,
					unsigned int mods,
					uint32_t key,
					uint32_t ascii,
					uint32_t state,
					bool handled,
					void *data);

int wlt_display_new(struct wlt_display **out,
		    struct ev_eloop *eloop);
void wlt_display_ref(struct wlt_display *disp);
void wlt_display_unref(struct wlt_display *disp);

int wlt_display_create_window(struct wlt_display *disp,
			      struct wlt_window **out,
			      unsigned int width,
			      unsigned int height,
			      void *data);
int wlt_display_register_cb(struct wlt_display *disp,
			    wlt_display_cb cb, void *data);
void wlt_display_unregister_cb(struct wlt_display *disp,
			       wlt_display_cb cb, void *data);

int wlt_display_get_selection_fd(struct wlt_display *disp, const char *mime);
int wlt_display_get_selection_to_fd(struct wlt_display *disp, const char *mime,
				    int output_fd);
int wlt_display_new_data_source(struct wlt_display *disp,
				struct wl_data_source **out);
void wlt_display_set_selection(struct wlt_display *disp,
			       struct wl_data_source *selection);

void wlt_window_ref(struct wlt_window *wnd);
void wlt_window_unref(struct wlt_window *wnd);

int wlt_window_create_widget(struct wlt_window *wnd,
			     struct wlt_widget **out,
			     void *data);
void wlt_window_schedule_redraw(struct wlt_window *wnd);
void wlt_window_damage(struct wlt_window *wnd,
		       struct wlt_rect *damage);
void wlt_window_get_buffer(struct wlt_window *wnd,
			   const struct wlt_rect *alloc,
			   struct wlt_shm_buffer *buf);
void wlt_window_move(struct wlt_window *wnd);
void wlt_window_resize(struct wlt_window *wnd, uint32_t edges);
int wlt_window_set_size(struct wlt_window *wnd,
			unsigned int width, unsigned int height);
void wlt_window_set_cursor(struct wlt_window *wnd, unsigned int cursor);
void wlt_window_set_close_cb(struct wlt_window *wnd,
			     wlt_window_close_cb cb);
void wlt_window_close(struct wlt_window *wnd);
void wlt_window_toggle_maximize(struct wlt_window *wnd);
bool wlt_window_is_maximized(struct wlt_window *wnd);
void wlt_window_toggle_fullscreen(struct wlt_window *wnd);
bool wlt_window_is_fullscreen(struct wlt_window *wnd);
struct ev_eloop *wlt_window_get_eloop(struct wlt_window *wnd);
struct wlt_display *wlt_window_get_display(struct wlt_window *wnd);

void wlt_widget_destroy(struct wlt_widget *widget);
struct wlt_window *wlt_widget_get_window(struct wlt_widget *widget);
void wlt_widget_set_redraw_cb(struct wlt_widget *widget,
			      wlt_widget_redraw_cb cb);
void wlt_widget_set_destroy_cb(struct wlt_widget *widget,
			       wlt_widget_destroy_cb cb);
void wlt_widget_set_resize_cb(struct wlt_widget *widget,
			      wlt_widget_prepare_resize_cb prepare_cb,
			      wlt_widget_resize_cb cb);
void wlt_widget_set_pointer_cb(struct wlt_widget *widget,
			       wlt_widget_pointer_enter_cb enter_cb,
			       wlt_widget_pointer_leave_cb leave_cb,
			       wlt_widget_pointer_motion_cb motion_cb,
			       wlt_widget_pointer_button_cb button_cb);
void wlt_widget_set_keyboard_cb(struct wlt_widget *widget,
				wlt_widget_keyboard_cb cb);

static inline bool wlt_rect_contains(struct wlt_rect *rect,
				     unsigned int x,
				     unsigned int y)
{
	if (x < rect->x || y < rect->y)
		return false;
	if (x >= rect->x + rect->width)
		return false;
	if (y >= rect->y + rect->height)
		return false;
	return true;
}

#endif /* WLT_TOOLKIT_H */
