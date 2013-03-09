/*
 * wlt - Theme Helper
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
 * Wayland Terminal theme/decoration drawing helper
 */

#include <errno.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "shl_log.h"
#include "shl_misc.h"
#include "wlt_main.h"
#include "wlt_theme.h"
#include "wlt_toolkit.h"

#define LOG_SUBSYSTEM "wlt_theme"

enum theme_location {
	LOC_NOWHERE,
	LOC_SOMEWHERE,
	LOC_RESIZE_TOP,
	LOC_RESIZE_BOTTOM,
	LOC_RESIZE_LEFT,
	LOC_RESIZE_RIGHT,
	LOC_RESIZE_TOP_LEFT,
	LOC_RESIZE_TOP_RIGHT,
	LOC_RESIZE_BOTTOM_LEFT,
	LOC_RESIZE_BOTTOM_RIGHT,
	LOC_CONTROL,
	LOC_MINIMIZE,
	LOC_MAXIMIZE,
	LOC_CLOSE,
};

struct wlt_theme {
	struct wlt_window *wnd;
	struct wlt_widget *widget;
	struct wlt_shm_buffer buffer;
	struct wlt_rect alloc;
	unsigned int control_height;
	unsigned int frame_width;
	unsigned int resize_margin;
	unsigned int button_size;
	unsigned int button_padding;
	unsigned int button_margin;

	unsigned int pointer_x;
	unsigned int pointer_y;
	unsigned int pointer_loc;
	bool pointer_pressed;
	unsigned int pointer_grabbed;
};

static void draw_control(struct wlt_theme *theme)
{
	uint8_t *dst;
	uint32_t *line, backcol;
	unsigned int i, j;
	unsigned int b1off, b2off, b3off;

	/* background */
	if (theme->pointer_loc == LOC_NOWHERE)
		backcol = (0x60 << 24) | (0xaa << 16) |
			  (0xaa << 8) | 0xaa;
	else
		backcol = (0x20 << 24) | (0xee << 16) |
			  (0xee << 8) | 0xee;
	dst = theme->buffer.data;
	for (i = 0; i < theme->control_height; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < theme->buffer.width; ++j) {
			if (i == 0 || i == theme->control_height - 1 ||
			    j == 0 || j == theme->buffer.width - 1)
				line[j] = 0xff << 24;
			else
				line[j] = backcol;
		}
		dst += theme->buffer.stride;
	}

	/* buttons */
	b1off = theme->buffer.width - theme->button_margin - theme->button_size;
	b2off = b1off - theme->button_padding - theme->button_size;
	b3off = b2off - theme->button_padding - theme->button_size;
	dst = theme->buffer.data + theme->buffer.stride * theme->button_margin;
	for (i = 0; i < theme->button_size; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < theme->buffer.width; ++j) {
			if (j >= b1off &&
			    j < b1off + theme->button_size) {
				if (i == 0 ||
				    i == theme->button_size - 1 ||
				    j == b1off ||
				    j == b1off + theme->button_size - 1)
					line[j] = 0xff << 24;
				else if (theme->pointer_loc == LOC_CLOSE &&
					 theme->pointer_pressed &&
					 theme->pointer_grabbed == LOC_CLOSE)
					line[j] = (0xff << 24) | 0x1f1f1f1f;
				else if (theme->pointer_loc == LOC_CLOSE &&
					 !theme->pointer_pressed)
					line[j] = 0xffffffff;
				else
					line[j] = (0xff << 24) | 0x33333333;
			} else if (j >= b2off &&
				   j < b2off + theme->button_size) {
				if (i == 0 ||
				    i == theme->button_size - 1 ||
				    j == b2off ||
				    j == b2off + theme->button_size - 1)
					line[j] = 0xff << 24;
				else if (theme->pointer_loc == LOC_MAXIMIZE &&
					 theme->pointer_pressed &&
					 theme->pointer_grabbed == LOC_MAXIMIZE)
					line[j] = (0xff << 24) | 0x1f1f1f1f;
				else if (theme->pointer_loc == LOC_MAXIMIZE &&
					 !theme->pointer_pressed)
					line[j] = 0xffffffff;
				else
					line[j] = (0xff << 24) | 0x66666666;
			} else if (j >= b3off &&
				   j < b3off + theme->button_size) {
				if (i == 0 ||
				    i == theme->button_size - 1 ||
				    j == b3off ||
				    j == b3off + theme->button_size - 1)
					line[j] = 0xff << 24;
				else if (theme->pointer_loc == LOC_MINIMIZE &&
					 theme->pointer_pressed &&
					 theme->pointer_grabbed == LOC_MINIMIZE)
					line[j] = (0xff << 24) | 0x1f1f1f1f;
				else if (theme->pointer_loc == LOC_MINIMIZE &&
					 !theme->pointer_pressed)
					line[j] = 0xffffffff;
				else
					line[j] = (0xff << 24) | 0xaaaaaaaa;
			}
		}
		dst += theme->buffer.stride;
	}
}

static void draw_frame(struct wlt_theme *theme)
{
	uint8_t *dst;
	uint32_t *line, col;
	unsigned int i, j, height;

	col = (0x60 << 24) | (0xaa << 16) | (0xaa << 8) | 0xaa;

	/* top frame */
	dst = theme->buffer.data + theme->buffer.stride *
	      theme->control_height;
	for (i = 0; i < theme->frame_width; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < theme->buffer.width; ++j) {
			if (!j || j + 1 == theme->buffer.width)
				line[j] = 0xff << 24;
			else
				line[j] = col;
		}
		dst += theme->buffer.stride;
	}

	/* bottom frame */
	dst = theme->buffer.data + theme->buffer.stride *
	      (theme->buffer.height - theme->frame_width);
	for (i = 0; i < theme->frame_width; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < theme->buffer.width; ++j) {
			if (!j || j + 1 == theme->buffer.width
			       || i + 1 == theme->frame_width)
				line[j] = 0xff << 24;
			else
				line[j] = col;
		}
		dst += theme->buffer.stride;
	}

	/* left frame */
	dst = theme->buffer.data + theme->buffer.stride *
	      (theme->control_height + theme->frame_width);
	height = theme->buffer.height - theme->control_height -
		 theme->frame_width * 2;
	for (i = 0; i < height; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < theme->frame_width; ++j)
			line[j] = j ? col : (0xff << 24);
		dst += theme->buffer.stride;
	}

	/* right frame */
	dst = theme->buffer.data + theme->buffer.stride *
	      (theme->control_height + theme->frame_width);
	height = theme->buffer.height - theme->control_height -
		 theme->frame_width * 2;
	for (i = 0; i < height; ++i) {
		line = (uint32_t*)dst;
		line += theme->buffer.width - theme->frame_width;
		for (j = 0; j < theme->frame_width; ++j) {
			if (j + 1 == theme->frame_width)
				line[j] = 0xff << 24;
			else
				line[j] = col;
		}
		dst += theme->buffer.stride;
	}
}

static void widget_draw_fallback(struct wlt_theme *theme)
{
	uint8_t *dst;
	uint32_t *line;
	unsigned int i, j;

	dst = theme->buffer.data;
	for (i = 0; i < theme->buffer.height; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < theme->buffer.width; ++j) {
			line[j] = 0xff << 24;
		}
		dst += theme->buffer.stride;
	}
}

static void widget_redraw(struct wlt_widget *widget, unsigned int flags,
			  void *data)
{
	struct wlt_theme *theme = data;
	unsigned int width, height;

	if (flags & WLT_WINDOW_FULLSCREEN)
		return;

	width = theme->buffer.width;
	height = theme->buffer.height;
	if (width < 2 ||
	    width < 2 * theme->frame_width ||
	    width < 2 * theme->button_margin + 2 * theme->button_padding +
	            3 * theme->button_size) {
		widget_draw_fallback(theme);
	} else if (height < theme->control_height + 2 * theme->frame_width) {
		widget_draw_fallback(theme);
	} else {
		draw_frame(theme);
		draw_control(theme);
	}
}

static void widget_prepare_resize(struct wlt_widget *widget,
				  unsigned int flags,
				  unsigned int width, unsigned int height,
				  unsigned int *min_width,
				  unsigned int *min_height,
				  unsigned int *new_width,
				  unsigned int *new_height,
				  void *data)
{
	struct wlt_theme *theme = data;
	unsigned int minw, minh;

	if (flags & WLT_WINDOW_FULLSCREEN)
		return;

	/* set minimal size */
	minw = 2 * theme->button_margin + 2 * theme->button_padding +
	       3 * theme->button_size + *new_width;
	minh = theme->button_size + 2 * theme->button_padding +
	       2 * theme->frame_width + *new_height;
	if (*min_width < minw)
		*min_width = minw;
	if (*min_height < minh)
		*min_height = minh;

	/* set margin size */
	minw = 2 * theme->frame_width;
	minh = theme->control_height + 2 * theme->frame_width;

	*new_width += minw;
	*new_height += minh;
}

static void widget_resize(struct wlt_widget *widget, unsigned int flags,
			  struct wlt_rect *alloc, void *data)
{
	struct wlt_theme *theme = data;
	unsigned int nwidth, nheight;

	wlt_window_get_buffer(theme->wnd, alloc, &theme->buffer);
	memcpy(&theme->alloc, alloc, sizeof(*alloc));

	if (flags & WLT_WINDOW_FULLSCREEN)
		return;

	alloc->x = theme->frame_width;
	alloc->y = theme->control_height + theme->frame_width;
	nwidth = alloc->width - 2 * theme->frame_width;
	nheight = alloc->height;
	nheight -= (theme->control_height + 2 * theme->frame_width);

	if (nwidth > alloc->width || nheight > alloc->height) {
		alloc->width = 0;
		alloc->height = 0;
	} else {
		alloc->width = nwidth;
		alloc->height = nheight;
	}
}

static unsigned int get_pointer_location(struct wlt_theme *theme)
{
	unsigned int m = theme->resize_margin;
	unsigned int b1off, b2off, b3off;

	if (theme->pointer_y < m) {
		if (theme->pointer_x < m)
			return LOC_RESIZE_TOP_LEFT;
		else if (theme->pointer_x >= theme->buffer.width - m)
			return LOC_RESIZE_TOP_RIGHT;
		else
			return LOC_RESIZE_TOP;
	}

	if (theme->pointer_y >= theme->buffer.height - m) {
		if (theme->pointer_x < m)
			return LOC_RESIZE_BOTTOM_LEFT;
		else if (theme->pointer_x >= theme->buffer.width - m)
			return LOC_RESIZE_BOTTOM_RIGHT;
		else
			return LOC_RESIZE_BOTTOM;
	}

	if (theme->pointer_x < m)
		return LOC_RESIZE_LEFT;

	if (theme->pointer_x >= theme->buffer.width - m)
		return LOC_RESIZE_RIGHT;

	if (theme->pointer_y < theme->control_height) {
		b1off = theme->buffer.width - theme->button_margin -
			theme->button_size;
		b2off = b1off - theme->button_padding - theme->button_size;
		b3off = b2off - theme->button_padding - theme->button_size;

		if (theme->pointer_y >= theme->button_margin &&
		    theme->pointer_y < theme->control_height -
				       theme->button_margin) {
			if (theme->pointer_x >= b1off &&
			    theme->pointer_x < b1off + theme->button_size)
				return LOC_CLOSE;
			if (theme->pointer_x >= b2off &&
			    theme->pointer_x < b2off + theme->button_size)
				return LOC_MAXIMIZE;
			if (theme->pointer_x >= b3off &&
			    theme->pointer_x < b3off + theme->button_size)
				return LOC_MINIMIZE;
		}

		return LOC_CONTROL;
	}

	return LOC_SOMEWHERE;
}

static void set_pointer_location(struct wlt_theme *theme, unsigned int loc)
{
	if (theme->pointer_loc == loc)
		return;

	theme->pointer_loc = loc;
	if (loc == LOC_NOWHERE) {
		theme->pointer_x = -1;
		theme->pointer_y = -1;
	}

	wlt_window_schedule_redraw(theme->wnd);
}

static void widget_pointer_motion(struct wlt_widget *widget,
				  unsigned int x, unsigned int y, void *data)
{
	struct wlt_theme *theme = data;

	if (!wlt_rect_contains(&theme->alloc, x, y)) {
		set_pointer_location(theme, LOC_NOWHERE);
		return;
	} else {
		theme->pointer_x = x - theme->alloc.x;
		theme->pointer_y = y - theme->alloc.y;
		set_pointer_location(theme, get_pointer_location(theme));
	}

	switch (theme->pointer_loc) {
	case LOC_RESIZE_LEFT:
		wlt_window_set_cursor(theme->wnd, WLT_CURSOR_LEFT);
		break;
	case LOC_RESIZE_RIGHT:
		wlt_window_set_cursor(theme->wnd, WLT_CURSOR_RIGHT);
		break;
	case LOC_RESIZE_TOP:
		wlt_window_set_cursor(theme->wnd, WLT_CURSOR_TOP);
		break;
	case LOC_RESIZE_BOTTOM:
		wlt_window_set_cursor(theme->wnd, WLT_CURSOR_BOTTOM);
		break;
	case LOC_RESIZE_TOP_LEFT:
		wlt_window_set_cursor(theme->wnd, WLT_CURSOR_TOP_LEFT);
		break;
	case LOC_RESIZE_TOP_RIGHT:
		wlt_window_set_cursor(theme->wnd, WLT_CURSOR_TOP_RIGHT);
		break;
	case LOC_RESIZE_BOTTOM_LEFT:
		wlt_window_set_cursor(theme->wnd,
				      WLT_CURSOR_BOTTOM_LEFT);
		break;
	case LOC_RESIZE_BOTTOM_RIGHT:
		wlt_window_set_cursor(theme->wnd,
				      WLT_CURSOR_BOTTOM_RIGHT);
		break;
	default:
		wlt_window_set_cursor(theme->wnd, WLT_CURSOR_LEFT_PTR);
	}
}

static void widget_pointer_enter(struct wlt_widget *widget,
				 unsigned int x, unsigned int y, void *data)
{
	struct wlt_theme *theme = data;

	widget_pointer_motion(widget, x, y, theme);
}

static void widget_pointer_leave(struct wlt_widget *widget, void *data)
{
	struct wlt_theme *theme = data;

	if (theme->pointer_pressed) {
		theme->pointer_pressed = false;
		wlt_window_schedule_redraw(theme->wnd);
	}

	set_pointer_location(theme, LOC_NOWHERE);
}

static void button_action(struct wlt_theme *theme)
{
	if (theme->pointer_grabbed != theme->pointer_loc)
		return;

	switch (theme->pointer_loc) {
	case LOC_CLOSE:
		wlt_window_close(theme->wnd);
		break;
	case LOC_MAXIMIZE:
		wlt_window_toggle_maximize(theme->wnd);
		break;
	case LOC_MINIMIZE:
		break;
	}
}

static void widget_pointer_button(struct wlt_widget *widget,
				  uint32_t button, uint32_t state, void *data)
{
	struct wlt_theme *theme = data;

	if (button != BTN_LEFT)
		return;

	if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		if (theme->pointer_pressed) {
			button_action(theme);
			theme->pointer_pressed = false;
			theme->pointer_grabbed = LOC_NOWHERE;
			wlt_window_schedule_redraw(theme->wnd);
		}
		return;
	}

	if (!theme->pointer_pressed) {
		theme->pointer_pressed = true;
		theme->pointer_grabbed = theme->pointer_loc;
		wlt_window_schedule_redraw(theme->wnd);
	}

	/* prevent resize/move during fullscreen/maximized */
	if (wlt_window_is_maximized(theme->wnd) ||
	    wlt_window_is_fullscreen(theme->wnd))
		return;

	switch (theme->pointer_loc) {
	case LOC_RESIZE_LEFT:
		wlt_window_resize(theme->wnd, WL_SHELL_SURFACE_RESIZE_LEFT);
		break;
	case LOC_RESIZE_RIGHT:
		wlt_window_resize(theme->wnd, WL_SHELL_SURFACE_RESIZE_RIGHT);
		break;
	case LOC_RESIZE_TOP:
		wlt_window_resize(theme->wnd,
			WL_SHELL_SURFACE_RESIZE_TOP);
		break;
	case LOC_RESIZE_BOTTOM:
		wlt_window_resize(theme->wnd,
			WL_SHELL_SURFACE_RESIZE_BOTTOM);
		break;
	case LOC_RESIZE_TOP_LEFT:
		wlt_window_resize(theme->wnd,
			WL_SHELL_SURFACE_RESIZE_TOP_LEFT);
		break;
	case LOC_RESIZE_TOP_RIGHT:
		wlt_window_resize(theme->wnd,
			WL_SHELL_SURFACE_RESIZE_TOP_RIGHT);
		break;
	case LOC_RESIZE_BOTTOM_LEFT:
		wlt_window_resize(theme->wnd,
			WL_SHELL_SURFACE_RESIZE_BOTTOM_LEFT);
		break;
	case LOC_RESIZE_BOTTOM_RIGHT:
		wlt_window_resize(theme->wnd,
			WL_SHELL_SURFACE_RESIZE_BOTTOM_RIGHT);
		break;
	case LOC_CONTROL:
		wlt_window_move(theme->wnd);
		break;
	}
}

static bool widget_key(struct wlt_widget *widget, unsigned int mask,
		       uint32_t sym, uint32_t ascii, uint32_t state,
		       bool handled, void *data)
{
	struct wlt_theme *theme = data;

	if (handled || state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return false;

	if (conf_grab_matches(wlt_conf.grab_fullscreen,
			      mask, 1, &sym)) {
		wlt_window_toggle_fullscreen(theme->wnd);
		return true;
	}

	return false;
}


static void widget_destroy(struct wlt_widget *widget, void *data)
{
	struct wlt_theme *theme = data;

	log_debug("destroy theme");

	free(theme);
}

int wlt_theme_new(struct wlt_theme **out, struct wlt_window *wnd)
{
	struct wlt_theme *theme;
	int ret;

	if (!out || !wnd)
		return -EINVAL;

	log_debug("create new theme");

	theme = malloc(sizeof(*theme));
	if (!theme)
		return -ENOMEM;
	memset(theme, 0, sizeof(*theme));
	theme->wnd = wnd;
	theme->control_height = 25;
	theme->frame_width = 5;
	theme->resize_margin = 5;
	theme->button_size = 15;
	theme->button_padding = 3;
	theme->button_margin = 5;
	theme->pointer_grabbed = LOC_NOWHERE;
	set_pointer_location(theme, LOC_NOWHERE);

	ret = wlt_window_create_widget(wnd, &theme->widget, theme);
	if (ret) {
		log_error("cannot create widget");
		goto err_free;
	}

	wlt_widget_set_destroy_cb(theme->widget, widget_destroy);
	wlt_widget_set_redraw_cb(theme->widget, widget_redraw);
	wlt_widget_set_resize_cb(theme->widget, widget_prepare_resize,
				 widget_resize);
	wlt_widget_set_pointer_cb(theme->widget, widget_pointer_enter,
				  widget_pointer_leave, widget_pointer_motion,
				  widget_pointer_button);
	wlt_widget_set_keyboard_cb(theme->widget, widget_key);
	*out = theme;
	return 0;

err_free:
	free(theme);
	return ret;
}

void wlt_theme_destroy(struct wlt_theme *theme)
{
	if (!theme)
		return;

	wlt_widget_destroy(theme->widget);
}
