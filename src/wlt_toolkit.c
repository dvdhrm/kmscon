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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include "eloop.h"
#include "shl_array.h"
#include "shl_dlist.h"
#include "shl_hook.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "tsm_vte.h"
#include "wlt_main.h"
#include "wlt_toolkit.h"

#define LOG_SUBSYSTEM "wlt_toolkit"

struct wlt_pool {
	struct wl_shm_pool *w_pool;
	size_t size;
	uint8_t *data;
};

enum {
	STATE_INIT = 0,
	STATE_RUNNING,
	STATE_HUP,
};

struct data_offer {
	unsigned long ref;
	struct wl_data_offer *w_offer;
	struct wlt_display *disp;
	struct shl_array *types;
};

struct wlt_display {
	unsigned long ref;
	struct ev_eloop *eloop;
	struct wl_display *dp;
	struct ev_fd *dp_fd;
	struct wl_global_listener *dp_listener;
	struct shl_hook *listeners;
	unsigned int state;
	struct wl_registry *w_registry;

	struct shl_dlist window_list;

	struct wl_compositor *w_comp;
	struct wl_seat *w_seat;
	struct wl_shell *w_shell;
	struct wl_shm *w_shm;

	uint32_t last_serial;
	uint32_t pointer_enter_serial;
	struct wl_pointer *w_pointer;
	struct wlt_window *pointer_focus;

	uint32_t cursor_serial;
	unsigned int current_cursor;
	struct wl_surface *w_cursor_surface;
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor *cursors[WLT_CURSOR_NUM];

	struct wl_keyboard *w_keyboard;
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
	struct wlt_window *keyboard_focus;
	struct ev_timer *repeat_timer;
	uint32_t repeat_sym;
	uint32_t repeat_ascii;

	struct wl_data_device_manager *w_manager;
	struct wl_data_device *w_data_dev;
	struct data_offer *drag_offer;
	struct data_offer *selection_offer;
};

struct wlt_window {
	unsigned long ref;
	struct shl_dlist list;
	void *data;
	wlt_window_close_cb close_cb;
	bool close_pending;
	struct wlt_display *disp;
	struct wlt_pool *pool;

	struct wl_surface *w_surface;
	struct wl_shell_surface *w_shell_surface;
	struct wl_buffer *w_buffer;

	bool buffer_attached;
	bool skip_damage;
	bool need_resize;
	bool need_frame;
	bool idle_pending;
	unsigned int new_width;
	unsigned int new_height;
	unsigned int saved_width;
	unsigned int saved_height;
	unsigned int resize_edges;
	bool maximized;
	bool fullscreen;
	struct wlt_shm_buffer buffer;
	struct wl_callback *w_frame;

	struct shl_dlist widget_list;
};

struct wlt_widget {
	struct shl_dlist list;
	struct wlt_window *wnd;
	void *data;
	wlt_widget_redraw_cb redraw_cb;
	wlt_widget_destroy_cb destroy_cb;
	wlt_widget_prepare_resize_cb prepare_resize_cb;
	wlt_widget_resize_cb resize_cb;
	wlt_widget_pointer_enter_cb pointer_enter_cb;
	wlt_widget_pointer_leave_cb pointer_leave_cb;
	wlt_widget_pointer_motion_cb pointer_motion_cb;
	wlt_widget_pointer_button_cb pointer_button_cb;
	wlt_widget_keyboard_cb keyboard_cb;
};

static int wlt_pool_new(struct wlt_pool **out, struct wlt_display *disp,
			size_t size)
{
	static const char template[] = "/wlterm-shared-XXXXXX";
	struct wlt_pool *pool;
	int fd, ret;
	const char *path;
	char *name;

	path = getenv("XDG_RUNTIME_DIR");
	if (!path) {
		log_error("XDG_RUNTIME_DIR not set");
		return -EFAULT;
	}

	pool = malloc(sizeof(*pool));
	if (!pool)
		return -ENOMEM;
	memset(pool, 0, sizeof(*pool));
	pool->size = size;

	name = malloc(strlen(path) + sizeof(template));
	if (!name) {
		ret = -ENOMEM;
		goto err_free;
	}
	strcpy(name, path);
	strcat(name, template);

	fd = mkostemp(name, O_CLOEXEC);
	if (fd < 0) {
		log_error("cannot create temporary file %s via mkostemp() (%d): %m",
			  name, errno);
		ret = -EFAULT;
		free(name);
		goto err_free;
	}

	ret = unlink(name);
	if (ret)
		log_warning("cannot unlink temporary file %s (%d): %m",
			    name, errno);
	free(name);

	ret = ftruncate(fd, size);
	if (ret) {
		log_error("cannot truncate temporary file to length %lu (%d): %m",
			  size, errno);
		ret = -EFAULT;
		goto err_close;
	}

	pool->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
			  fd, 0);
	if (pool->data == MAP_FAILED) {
		log_error("cannot mmap temporary file (%d): %m", errno);
		ret = -EFAULT;
		goto err_close;
	}

	pool->w_pool = wl_shm_create_pool(disp->w_shm, fd, size);
	if (!pool->w_pool) {
		log_error("cannot create wayland shm pool object");
		ret = -EFAULT;
		goto err_map;
	}

	close(fd);
	*out = pool;
	return 0;

err_map:
	munmap(pool->data, size);
err_close:
	close(fd);
err_free:
	free(pool);
	return ret;
}

static void wlt_pool_free(struct wlt_pool *pool)
{
	munmap(pool->data, pool->size);
	wl_shm_pool_destroy(pool->w_pool);
	free(pool);
}

/*
 * These cursor-names are based on:
 *   https://bugs.kde.org/attachment.cgi?id=67313
 */

static char *(*cursors[])[] = {
	[WLT_CURSOR_NONE] = &(char*[]) {
		NULL,
	},
	[WLT_CURSOR_TOP] = &(char*[]) {
		"top_side",
		"n-resize",
		NULL,
	},
	[WLT_CURSOR_BOTTOM] = &(char*[]) {
		"bottom_side",
		"s-resize",
		NULL,
	},
	[WLT_CURSOR_LEFT] = &(char*[]) {
		"left_side",
		"w-resize",
		NULL,
	},
	[WLT_CURSOR_RIGHT] = &(char*[]) {
		"right_side",
		"e-resize",
		NULL,
	},
	[WLT_CURSOR_TOP_LEFT] = &(char*[]) {
		"top_left_corner",
		"nw-resize",
		NULL,
	},
	[WLT_CURSOR_TOP_RIGHT] = &(char*[]) {
		"top_right_corner",
		"ne-resize",
		NULL,
	},
	[WLT_CURSOR_BOTTOM_LEFT] = &(char*[]) {
		"bottom_left_corner",
		"sw-resize",
		NULL,
	},
	[WLT_CURSOR_BOTTOM_RIGHT] = &(char*[]) {
		"bottom_right_corner",
		"se-resize",
		NULL,
	},
	[WLT_CURSOR_DRAGGING] = &(char*[]) {
		"grabbing",
		"closedhand",
		"208530c400c041818281048008011002",
		NULL,
	},
	[WLT_CURSOR_LEFT_PTR] = &(char*[]) {
		"left_ptr",
		"default",
		"top_left_arrow",
		"left-arrow",
		NULL,
	},
	[WLT_CURSOR_IBEAM] = &(char*[]) {
		"xterm",
		"ibeam",
		"text",
		NULL,
	},
};

static int load_cursors(struct wlt_display *disp)
{
	unsigned int i, j;
	size_t size = sizeof(cursors) / sizeof(*cursors);
	struct wl_cursor *cursor;

	disp->w_cursor_surface = wl_compositor_create_surface(disp->w_comp);

	disp->cursor_theme = wl_cursor_theme_load(NULL, 32, disp->w_shm);
	if (!disp->cursor_theme) {
		log_warning("cannot load curdor theme");
		return -EFAULT;
	}

	for (i = 0; i < WLT_CURSOR_NUM; ++i) {
		cursor = NULL;
		if (i < size && cursors[i]) {
			for (j = 0; (*cursors[i])[j]; ++j) {
				cursor = wl_cursor_theme_get_cursor(
							disp->cursor_theme,
							(*cursors[i])[j]);
				if (cursor)
					break;
			}
		}

		if (!cursor && i != WLT_CURSOR_NONE)
			log_warning("cannot load cursor for ID %d", i);

		disp->cursors[i] = cursor;
	}

	return 0;
}

static void unload_cursors(struct wlt_display *disp)
{
	if (disp->cursor_theme)
		wl_cursor_theme_destroy(disp->cursor_theme);
	if (disp->w_cursor_surface)
		wl_surface_destroy(disp->w_cursor_surface);
}

static void set_cursor(struct wlt_display *disp, unsigned int cursor)
{
	bool force = false;
	struct wl_cursor *cur;
	struct wl_cursor_image *image;
	struct wl_buffer *buffer;

	if (cursor >= WLT_CURSOR_NUM)
		cursor = WLT_CURSOR_LEFT_PTR;

	if (disp->pointer_enter_serial > disp->cursor_serial)
		force = true;

	if (!force && cursor == disp->current_cursor)
		return;

	disp->current_cursor = cursor;
	disp->cursor_serial = disp->pointer_enter_serial;

	cur = disp->cursors[cursor];
	if (!cur) {
		wl_pointer_set_cursor(disp->w_pointer,
				      disp->pointer_enter_serial,
				      NULL, 0, 0);
		return;
	}

	image = cur->images[0];
	buffer = wl_cursor_image_get_buffer(image);
	if (!buffer) {
		log_error("cannot load buffer for cursor image");
		return;
	}

	wl_pointer_set_cursor(disp->w_pointer,
			      disp->pointer_enter_serial,
			      disp->w_cursor_surface,
			      image->hotspot_x, image->hotspot_y);
	wl_surface_attach(disp->w_cursor_surface, buffer, 0, 0);
	wl_surface_damage(disp->w_cursor_surface, 0, 0,
			  image->width, image->height);
	wl_surface_commit(disp->w_cursor_surface);
}

static void dp_dispatch(struct wlt_display *disp, bool nonblock)
{
	int ret;

	errno = 0;
	if (nonblock) {
		ret = wl_display_dispatch_pending(disp->dp);
		if (ret != -1)
			ret = wl_display_flush(disp->dp);
	} else {
		ret = wl_display_dispatch(disp->dp);
	}

	if (ret == -1) {
		log_error("error during wayland dispatch (%d): %m", errno);
		return;
	} else if (errno == EAGAIN) {
		ret = ev_fd_update(disp->dp_fd, EV_READABLE | EV_WRITEABLE);
		if (ret)
			log_warning("cannot update wayland-fd event-polling modes (%d)",
				    ret);
	} else {
		ret = ev_fd_update(disp->dp_fd, EV_READABLE);
		if (ret)
			log_warning("cannot update wayland-fd event-polling modes (%d)",
				    ret);
	}
}

static void dp_pre_event(struct ev_eloop *eloop, void *unused, void *data)
{
	struct wlt_display *disp = data;

	dp_dispatch(disp, true);
}

static void dp_event(struct ev_fd *fd, int mask, void *data)
{
	struct wlt_display *disp = data;

	if (mask & (EV_HUP | EV_ERR)) {
		log_warning("HUP/ERR on wayland socket");
		disp->state = STATE_HUP;
		shl_hook_call(disp->listeners, disp, (void*)WLT_DISPLAY_HUP);
		ev_eloop_rm_fd(disp->dp_fd);
		disp->dp_fd = NULL;
		return;
	}

	if (mask & EV_READABLE)
		dp_dispatch(disp, false);
	else
		dp_dispatch(disp, true);
}

static void pointer_enter(void *data, struct wl_pointer *w_pointer,
			  uint32_t serial, struct wl_surface *w_surface,
			  wl_fixed_t x, wl_fixed_t y)
{
	struct wlt_display *disp = data;
	struct shl_dlist *iter;
	struct wlt_window *wnd;
	struct wlt_widget *widget;

	if (!w_surface)
		return;

	shl_dlist_for_each(iter, &disp->window_list) {
		wnd = shl_dlist_entry(iter, struct wlt_window, list);
		if (wnd->w_surface == w_surface)
			break;
	}

	if (iter == &disp->window_list) {
		log_debug("unknown surface");
		return;
	}

	disp->pointer_enter_serial = serial;
	disp->last_serial = serial;
	disp->pointer_focus = wnd;
	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->pointer_enter_cb)
			widget->pointer_enter_cb(widget, x >> 8, y >> 8,
						 widget->data);
	}
}

static void pointer_leave(void *data, struct wl_pointer *w_pointer,
			  uint32_t serial, struct wl_surface *w_surface)
{
	struct wlt_display *disp = data;
	struct shl_dlist *iter;
	struct wlt_window *wnd;
	struct wlt_widget *widget;

	wnd = disp->pointer_focus;
	disp->pointer_focus = NULL;
	disp->last_serial = serial;

	if (!wnd)
		return;

	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->pointer_leave_cb)
			widget->pointer_leave_cb(widget, widget->data);
	}
}

static void pointer_motion(void *data, struct wl_pointer *w_pointer,
			   uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct wlt_display *disp = data;
	struct shl_dlist *iter;
	struct wlt_window *wnd;
	struct wlt_widget *widget;

	wnd = disp->pointer_focus;
	if (!wnd)
		return;

	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->pointer_motion_cb)
			widget->pointer_motion_cb(widget, x >> 8, y >> 8,
						  widget->data);
	}
}

static void pointer_button(void *data, struct wl_pointer *w_pointer,
			   uint32_t serial, uint32_t time, uint32_t button,
			   uint32_t state)
{
	struct wlt_display *disp = data;
	struct shl_dlist *iter;
	struct wlt_window *wnd;
	struct wlt_widget *widget;

	wnd = disp->pointer_focus;
	disp->last_serial = serial;

	if (!wnd)
		return;

	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->pointer_button_cb)
			widget->pointer_button_cb(widget, button, state,
						  widget->data);
	}
}

static void pointer_axis(void *data, struct wl_pointer *w_pointer,
			 uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
			    uint32_t format, int fd, uint32_t size)
{
	struct wlt_display *disp = data;
	char *map;

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		log_error("invalid keyboard format");
		close(fd);
		return;
	}

	map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		log_error("cannot mmap keyboard keymap");
		close(fd);
		return;
	}

	disp->xkb_keymap = xkb_keymap_new_from_string(disp->xkb_ctx, map,
						      XKB_KEYMAP_FORMAT_TEXT_V1,
						      0);
	munmap(map, size);
	close(fd);

	if (!disp->xkb_keymap) {
		log_error("cannot create xkb keymap");
		return;
	}

	disp->xkb_state = xkb_state_new(disp->xkb_keymap);
	if (!disp->xkb_state) {
		log_error("cannot create XKB state object");
		xkb_keymap_unref(disp->xkb_keymap);
		disp->xkb_keymap = NULL;
		return;
	}
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
			   uint32_t serial, struct wl_surface *w_surface,
			   struct wl_array *keys)
{
	struct wlt_display *disp = data;
	struct shl_dlist *iter;
	struct wlt_window *wnd;

	disp->last_serial = serial;
	if (!disp->xkb_state)
		return;

	if (!w_surface)
		return;

	shl_dlist_for_each(iter, &disp->window_list) {
		wnd = shl_dlist_entry(iter, struct wlt_window, list);
		if (wnd->w_surface == w_surface)
			break;
	}

	if (iter == &disp->window_list)
		return;

	disp->keyboard_focus = wnd;
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
			   uint32_t serial, struct wl_surface *surface)
{
	struct wlt_display *disp = data;

	disp->last_serial = serial;
	disp->keyboard_focus = NULL;
	ev_timer_update(disp->repeat_timer, NULL);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
			 uint32_t serial, uint32_t time, uint32_t key,
			 uint32_t state_w)
{
	struct wlt_display *disp = data;
	struct wlt_window *wnd = disp->keyboard_focus;
	uint32_t code, num_syms, ascii;
	unsigned int mask;
	enum wl_keyboard_key_state state = state_w;
	const xkb_keysym_t *syms;
	xkb_keysym_t sym;
	struct shl_dlist *iter;
	struct wlt_widget *widget;
	struct itimerspec spec;
	bool handled;

	disp->last_serial = serial;
	if (!disp->xkb_state)
		return;

	code = key + 8;
	if (!wnd)
		return;

	mask = shl_get_xkb_mods(disp->xkb_state);
	num_syms = xkb_state_key_get_syms(disp->xkb_state, code, &syms);
	ascii = shl_get_ascii(disp->xkb_state, code, syms, num_syms);
	sym = XKB_KEY_NoSymbol;
	if (num_syms == 1)
		sym = syms[0];

	handled = false;
	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->keyboard_cb) {
			if (widget->keyboard_cb(widget, mask, sym, ascii, state,
						handled, widget->data))
				handled = true;
		}
	}

	if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		ev_timer_update(disp->repeat_timer, NULL);
	} else if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		disp->repeat_sym = sym;
		disp->repeat_ascii = ascii;
		spec.it_interval.tv_sec = 0;
		spec.it_interval.tv_nsec = wlt_conf.xkb_repeat_rate * 1000000;
		spec.it_value.tv_sec = 0;
		spec.it_value.tv_nsec = wlt_conf.xkb_repeat_delay * 1000000;
		ev_timer_update(disp->repeat_timer, &spec);
	}
}

static void repeat_event(struct ev_timer *timer, uint64_t num, void *data)
{
	struct wlt_display *disp = data;
	struct wlt_window *wnd = disp->keyboard_focus;
	struct wlt_widget *widget;
	struct shl_dlist *iter;
	unsigned int mask;
	bool handled;

	if (!wnd)
		return;

	mask = shl_get_xkb_mods(disp->xkb_state);
	handled = false;
	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->keyboard_cb) {
			if (widget->keyboard_cb(widget, mask, disp->repeat_sym,
						disp->repeat_ascii,
						WL_KEYBOARD_KEY_STATE_PRESSED,
						handled, widget->data))
				handled = true;
		}
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
			       uint32_t serial, uint32_t mods_depressed,
			       uint32_t mods_latched, uint32_t mods_locked,
			       uint32_t group)
{
	struct wlt_display *disp = data;

	disp->last_serial = serial;
	if (!disp->xkb_state)
		return;

	xkb_state_update_mask(disp->xkb_state, mods_depressed, mods_latched,
			      mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
};

static void data_offer_unref(struct data_offer *offer)
{
	unsigned int i, len;

	if (!offer || !offer->ref || --offer->ref)
		return;

	len = shl_array_get_length(offer->types);
	for (i = 0; i < len; ++i)
		free(*SHL_ARRAY_AT(offer->types, char*, i));
	shl_array_free(offer->types);
	wl_data_offer_destroy(offer->w_offer);
	free(offer);
}

static void data_offer_offer(void *data, struct wl_data_offer *w_offer,
			     const char *type)
{
	char *tmp;
	int ret;
	struct data_offer *offer = wl_data_offer_get_user_data(w_offer);

	tmp = strdup(type);
	if (!tmp)
		return;

	ret = shl_array_push(offer->types, &tmp);
	if (ret) {
		free(tmp);
		return;
	}
}

static const struct wl_data_offer_listener data_offer_listener = {
	.offer = data_offer_offer,
};

static void data_dev_data_offer(void *data, struct wl_data_device *w_dev,
				struct wl_data_offer *w_offer)
{
	struct data_offer *offer;
	struct wlt_display *disp = data;
	int ret;

	offer = malloc(sizeof(*offer));
	if (!offer) {
		wl_data_offer_destroy(w_offer);
		return;
	}
	memset(offer, 0, sizeof(*offer));
	offer->ref = 1;
	offer->w_offer = w_offer;
	offer->disp = disp;

	ret = shl_array_new(&offer->types, sizeof(char*), 4);
	if (ret) {
		wl_data_offer_destroy(w_offer);
		free(offer);
		return;
	}

	wl_data_offer_add_listener(w_offer, &data_offer_listener, offer);
	wl_data_offer_set_user_data(w_offer, offer);
}

static void data_dev_enter(void *data, struct wl_data_device *w_dev,
			   uint32_t serial, struct wl_surface *w_surface,
			   wl_fixed_t x, wl_fixed_t y,
			   struct wl_data_offer *w_offer)
{
	struct wlt_display *disp = data;

	if (disp->drag_offer) {
		data_offer_unref(disp->drag_offer);
		disp->drag_offer = NULL;
	}

	if (!w_offer)
		return;

	disp->drag_offer = wl_data_offer_get_user_data(w_offer);
}

static void data_dev_leave(void *data, struct wl_data_device *w_dev)
{
	struct wlt_display *disp = data;

	if (disp->drag_offer) {
		data_offer_unref(disp->drag_offer);
		disp->drag_offer = NULL;
	}
}

static void data_dev_motion(void *data, struct wl_data_device *w_dev,
			    uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
}

static void data_dev_drop(void *data, struct wl_data_device *w_dev)
{
}

static void data_dev_selection(void *data, struct wl_data_device *w_dev,
			       struct wl_data_offer *w_offer)
{
	struct wlt_display *disp = data;

	if (disp->selection_offer) {
		data_offer_unref(disp->selection_offer);
		disp->selection_offer = NULL;
	}

	if (!w_offer)
		return;

	disp->selection_offer = wl_data_offer_get_user_data(w_offer);
}

int wlt_display_get_selection_to_fd(struct wlt_display *disp, const char *mime,
				    int output_fd)
{
	unsigned int i, num;
	struct data_offer *offer;

	if (!disp || !mime)
		return -EINVAL;
	if (!disp->selection_offer)
		return -ENOENT;

	offer = disp->selection_offer;
	num = shl_array_get_length(offer->types);
	for (i = 0; i < num; ++i) {
		if (!strcmp(mime, *SHL_ARRAY_AT(offer->types, char*, i)))
			break;
	}

	if (i == num)
		return -EAGAIN;

	wl_data_offer_receive(offer->w_offer, mime, output_fd);
	return 0;
}

int wlt_display_get_selection_fd(struct wlt_display *disp, const char *mime)
{
	int p[2], ret;

	if (pipe2(p, O_CLOEXEC | O_NONBLOCK))
		return -EFAULT;

	ret = wlt_display_get_selection_to_fd(disp, mime, p[1]);
	close(p[1]);

	if (ret) {
		close(p[0]);
		return ret;
	}

	return p[0];
}

int wlt_display_new_data_source(struct wlt_display *disp,
				struct wl_data_source **out)
{
	struct wl_data_source *src;

	if (!disp)
		return -EINVAL;

	src = wl_data_device_manager_create_data_source(disp->w_manager);
	if (!src)
		return -EFAULT;

	*out = src;
	return 0;
}

void wlt_display_set_selection(struct wlt_display *disp,
			       struct wl_data_source *selection)
{
	if (!disp)
		return;

	wl_data_device_set_selection(disp->w_data_dev, selection,
				     disp->last_serial);
}

static const struct wl_data_device_listener data_dev_listener = {
	.data_offer = data_dev_data_offer,
	.enter = data_dev_enter,
	.leave = data_dev_leave,
	.motion = data_dev_motion,
	.drop = data_dev_drop,
	.selection = data_dev_selection,
};

static void check_ready(struct wlt_display *disp)
{
	if (disp->state > STATE_INIT)
		return;

	if (disp->w_comp &&
	    disp->w_seat &&
	    disp->w_shell &&
	    disp->w_shm &&
	    disp->w_pointer &&
	    disp->w_keyboard &&
	    disp->w_manager) {
		log_debug("wayland display initialized");
		load_cursors(disp);

		disp->w_data_dev = wl_data_device_manager_get_data_device(
						disp->w_manager, disp->w_seat);
		wl_data_device_add_listener(disp->w_data_dev,
					    &data_dev_listener,
					    disp);

		disp->state = STATE_RUNNING;
		shl_hook_call(disp->listeners, disp, (void*)WLT_DISPLAY_READY);
	}
}

static void seat_capabilities(void *data, struct wl_seat *seat,
			      enum wl_seat_capability caps)
{
	struct wlt_display *disp = data;

	if (caps & WL_SEAT_CAPABILITY_POINTER && !disp->w_pointer) {
		disp->w_pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(disp->w_pointer, &pointer_listener,
					disp);
	}

	if (caps & WL_SEAT_CAPABILITY_KEYBOARD && !disp->w_keyboard) {
		disp->w_keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(disp->w_keyboard, &keyboard_listener,
					 disp);
	}

	check_ready(disp);
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
};

static void dp_global(void *data, struct wl_registry *registry, uint32_t id,
		      const char *iface, uint32_t version)
{
	struct wlt_display *disp = data;

	if (!strcmp(iface, "wl_display")) {
		log_debug("new wl_display global");
		return;
	} else if (!strcmp(iface, "wl_compositor")) {
		if (disp->w_comp) {
			log_error("global wl_compositor advertised twice");
			return;
		}
		disp->w_comp = wl_registry_bind(disp->w_registry,
						id,
						&wl_compositor_interface,
						1);
		if (!disp->w_comp) {
			log_error("cannot bind wl_compositor object");
			return;
		}
	} else if (!strcmp(iface, "wl_seat")) {
		if (disp->w_seat) {
			log_error("global wl_seat advertised twice");
			return;
		}
		disp->w_seat = wl_registry_bind(disp->w_registry,
						id,
						&wl_seat_interface,
						1);
		if (!disp->w_seat) {
			log_error("cannot bind wl_seat object");
			return;
		}

		wl_seat_add_listener(disp->w_seat, &seat_listener, disp);
	} else if (!strcmp(iface, "wl_shell")) {
		if (disp->w_shell) {
			log_error("global wl_shell advertised twice");
			return;
		}
		disp->w_shell = wl_registry_bind(disp->w_registry,
						 id,
						 &wl_shell_interface,
						 1);
		if (!disp->w_shell) {
			log_error("cannot bind wl_shell object");
			return;
		}
	} else if (!strcmp(iface, "wl_shm")) {
		if (disp->w_shm) {
			log_error("global wl_shm advertised twice");
			return;
		}
		disp->w_shm = wl_registry_bind(disp->w_registry,
					       id,
					       &wl_shm_interface,
					       1);
		if (!disp->w_shm) {
			log_error("cannot bind wl_shm object");
			return;
		}
	} else if (!strcmp(iface, "wl_data_device_manager")) {
		if (disp->w_manager) {
			log_error("global wl_data_device_manager advertised twice");
			return;
		}
		disp->w_manager = wl_registry_bind(disp->w_registry,
						   id,
						   &wl_data_device_manager_interface,
						   1);
		if (!disp->w_manager) {
			log_error("cannot bind wl_data_device_manager_object");
			return;
		}
	} else {
		log_debug("ignoring new unknown global %s", iface);
		return;
	}

	log_debug("new global %s", iface);

	check_ready(disp);
}

static const struct wl_registry_listener registry_listener = {
	.global = dp_global,
	/* TODO: handle .global_remove */
};

int wlt_display_new(struct wlt_display **out,
		    struct ev_eloop *eloop)
{
	struct wlt_display *disp;
	int ret, fd;

	if (!out || !eloop)
		return -EINVAL;

	log_debug("creating new wlt-display");

	disp = malloc(sizeof(*disp));
	if (!disp)
		return -ENOMEM;
	memset(disp, 0, sizeof(*disp));
	disp->ref = 1;
	disp->eloop = eloop;
	shl_dlist_init(&disp->window_list);

	ret = shl_hook_new(&disp->listeners);
	if (ret) {
		log_error("cannot create listeners hook");
		goto err_free;
	}

	disp->dp = wl_display_connect(NULL);
	if (!disp->dp) {
		log_error("cannot connect to wayland socket (%d): %m", errno);
		ret = -EFAULT;
		goto err_listener;
	}

	fd = wl_display_get_fd(disp->dp);
	if (fd < 0) {
		log_error("cannot retrieve wayland FD");
		ret = -EFAULT;
		goto err_dp;
	}

	/* TODO: nonblocking wl_display doesn't work, yet. fix that upstream */
	/*
	int set;
	set = fcntl(fd, F_GETFL);
	if (set < 0) {
		log_error("cannot retrieve file flags for wayland FD (%d): %m",
			  errno);
		ret = -EFAULT;
		goto err_dp;
	}

	set |= O_NONBLOCK;
	ret = fcntl(fd, F_SETFL, set);
	if (ret) {
		log_error("cannot set file flags for wayland FD (%d): %m",
			  errno);
		ret = -EFAULT;
		goto err_dp;
	}*/

	ret = ev_eloop_new_fd(eloop,
			      &disp->dp_fd,
			      fd,
			      EV_READABLE,
			      dp_event,
			      disp);
	if (ret) {
		log_error("cannot create event-fd for wayland display (%d)",
			  ret);
		goto err_dp;
	}

	ret = ev_eloop_register_pre_cb(eloop, dp_pre_event, disp);
	if (ret) {
		log_error("cannot register pre-cb (%d)", ret);
		goto err_dp_fd;
	}

	ret = ev_eloop_new_timer(eloop, &disp->repeat_timer, NULL,
				 repeat_event, disp);
	if (ret) {
		log_error("cannot create repeat-timer");
		goto err_pre_cb;
	}

	disp->w_registry = wl_display_get_registry(disp->dp);
	if (!disp->w_registry) {
		log_error("cannot get wayland registry object");
		goto err_timer;
	}

	wl_registry_add_listener(disp->w_registry, &registry_listener, disp);

	disp->xkb_ctx = xkb_context_new(0);
	if (!disp->xkb_ctx) {
		log_error("cannot create XKB context");
		goto err_timer;
	}

	log_debug("wlt-display waiting for globals...");

	ev_eloop_ref(disp->eloop);
	*out = disp;
	return 0;

err_timer:
	ev_eloop_rm_timer(disp->repeat_timer);
err_pre_cb:
	ev_eloop_unregister_pre_cb(disp->eloop, dp_pre_event, disp);
err_dp_fd:
	ev_eloop_rm_fd(disp->dp_fd);
err_dp:
	wl_display_disconnect(disp->dp);
err_listener:
	shl_hook_free(disp->listeners);
err_free:
	free(disp);
	return ret;
}

void wlt_display_ref(struct wlt_display *disp)
{
	if (!disp || !disp->ref)
		return;

	++disp->ref;
}

void wlt_display_unref(struct wlt_display *disp)
{
	if (!disp || !disp->ref || --disp->ref)
		return;

	unload_cursors(disp);
	wl_display_flush(disp->dp);
	wl_display_disconnect(disp->dp);
	ev_eloop_rm_timer(disp->repeat_timer);
	ev_eloop_unregister_pre_cb(disp->eloop, dp_pre_event, disp);
	ev_eloop_rm_fd(disp->dp_fd);
	xkb_context_unref(disp->xkb_ctx);
	shl_hook_free(disp->listeners);
	ev_eloop_unref(disp->eloop);
	free(disp);
}

int wlt_display_register_cb(struct wlt_display *disp,
			    wlt_display_cb cb, void *data)
{
	if (!disp)
		return -EINVAL;

	return shl_hook_add_cast(disp->listeners, cb, data, false);
}

void wlt_display_unregister_cb(struct wlt_display *disp,
			       wlt_display_cb cb, void *data)
{
	if (!disp)
		return;

	shl_hook_rm_cast(disp->listeners, cb, data);
}

static void shell_surface_ping(void *data, struct wl_shell_surface *s,
			       uint32_t serial)
{
	wl_shell_surface_pong(s, serial);
}

static void wlt_window_do_redraw(struct wlt_window *wnd,
				 unsigned int oldw,
				 unsigned int oldh)
{
	struct shl_dlist *iter;
	struct wlt_widget *widget;
	unsigned int x, y, flags;
	struct wlt_rect alloc;

	flags = 0;
	if (wnd->maximized)
		flags |= WLT_WINDOW_MAXIMIZED;
	if (wnd->fullscreen)
		flags |= WLT_WINDOW_FULLSCREEN;

	alloc.x = 0;
	alloc.y = 0;
	alloc.width = wnd->buffer.width;
	alloc.height = wnd->buffer.height;
	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->resize_cb)
			widget->resize_cb(widget, flags, &alloc, widget->data);
	}

	memset(wnd->buffer.data, 0,
	       wnd->buffer.stride * wnd->buffer.height);

	wnd->skip_damage = true;
	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->redraw_cb)
			widget->redraw_cb(widget, flags, widget->data);
	}
	wnd->skip_damage = false;

	x = 0;
	y = 0;
	if (!wnd->buffer_attached) {
		wnd->buffer_attached = true;
		if (wnd->resize_edges & WL_SHELL_SURFACE_RESIZE_LEFT)
			x = (int)oldw - wnd->buffer.width;
		if (wnd->resize_edges & WL_SHELL_SURFACE_RESIZE_TOP)
			y = (int)oldh - wnd->buffer.height;
		wnd->resize_edges = 0;
	}

	wl_surface_attach(wnd->w_surface, wnd->w_buffer, x, y);
	wl_surface_damage(wnd->w_surface, 0, 0, wnd->buffer.width,
			  wnd->buffer.height);
	wl_surface_commit(wnd->w_surface);
}

static int resize_window(struct wlt_window *wnd, unsigned int width,
			 unsigned int height)
{
	struct shl_dlist *iter;
	struct wlt_widget *widget;
	struct wl_buffer *old_buffer = NULL;
	struct wlt_pool *old_pool = NULL;
	size_t nsize;
	int ret;
	unsigned int oldw, oldh, neww, newh, minw, minh, flags;

	if (!width)
		width = wnd->buffer.width;
	if (!height)
		height = wnd->buffer.height;

	if (!width || !height)
		return -EINVAL;

	flags = 0;
	if (wnd->maximized)
		flags |= WLT_WINDOW_MAXIMIZED;
	if (wnd->fullscreen)
		flags |= WLT_WINDOW_FULLSCREEN;

	neww = 0;
	newh = 0;
	minw = 0;
	minh = 0;
	shl_dlist_for_each(iter, &wnd->widget_list) {
		widget = shl_dlist_entry(iter, struct wlt_widget, list);
		if (widget->prepare_resize_cb)
			widget->prepare_resize_cb(widget,
						  flags,
						  width, height,
						  &minw, &minh,
						  &neww, &newh,
						  widget->data);
	}

	if (neww)
		width = neww;
	if (newh)
		height = newh;

	if (width == wnd->buffer.width &&
	    height == wnd->buffer.height) {
		wlt_window_do_redraw(wnd, width, height);
		return 0;
	}

	oldw = wnd->buffer.width;
	oldh = wnd->buffer.height;

	nsize = width * height * 4;
	if (wnd->pool && wnd->pool->size >= nsize) {
		old_buffer = wnd->w_buffer;
		wnd->w_buffer = wl_shm_pool_create_buffer(wnd->pool->w_pool,
						0, width, height,
						width * 4,
						WL_SHM_FORMAT_ARGB8888);
		if (!wnd->w_buffer) {
			log_error("cannot create wayland shm buffer");
			wnd->w_buffer = old_buffer;
			return -ENOMEM;
		}
	} else {
		old_pool = wnd->pool;
		ret = wlt_pool_new(&wnd->pool, wnd->disp, nsize);
		if (ret) {
			log_error("cannot create memory pool");
			wnd->pool = old_pool;
			return ret;
		}

		old_buffer = wnd->w_buffer;
		wnd->w_buffer = wl_shm_pool_create_buffer(wnd->pool->w_pool,
						0, width, height,
						width * 4,
						WL_SHM_FORMAT_ARGB8888);
		if (!wnd->w_buffer) {
			log_error("cannot create wayland shm buffer");
			wnd->w_buffer = old_buffer;
			wlt_pool_free(wnd->pool);
			wnd->pool = old_pool;
			return -ENOMEM;
		}
	}

	wnd->buffer.data = wnd->pool->data;
	wnd->buffer.width = width;
	wnd->buffer.height = height;
	wnd->buffer.stride = width * 4;
	wnd->buffer_attached = false;

	wlt_window_do_redraw(wnd, oldw, oldh);

	if (old_buffer)
		wl_buffer_destroy(old_buffer);
	if (old_pool)
		wlt_pool_free(old_pool);

	return 0;
}

static void frame_callback(void *data, struct wl_callback *w_callback,
			   uint32_t time);
static void idle_frame(struct ev_eloop *eloop, void *unused, void *data);

static const struct wl_callback_listener frame_callback_listener = {
	.done = frame_callback,
};

static void do_frame(struct wlt_window *wnd)
{
	ev_eloop_unregister_idle_cb(wnd->disp->eloop, idle_frame, wnd,
				    EV_NORMAL);

	if (wnd->need_resize) {
		wnd->need_frame = true;
		wnd->need_resize = false;
		wnd->w_frame = wl_surface_frame(wnd->w_surface);
		wl_callback_add_listener(wnd->w_frame,
					 &frame_callback_listener, wnd);
		resize_window(wnd, wnd->new_width, wnd->new_height);
	} else {
		wnd->need_frame = true;
		wnd->w_frame = wl_surface_frame(wnd->w_surface);
		wl_callback_add_listener(wnd->w_frame,
					 &frame_callback_listener, wnd);
		wlt_window_do_redraw(wnd, wnd->buffer.width,
				     wnd->buffer.height);
	}
}

static void schedule_frame(struct wlt_window *wnd);

static void frame_callback(void *data, struct wl_callback *w_callback,
			   uint32_t time)
{
	struct wlt_window *wnd = data;

	wl_callback_destroy(w_callback);
	wnd->w_frame = NULL;

	wnd->idle_pending = false;
	if (wnd->need_frame)
		schedule_frame(wnd);
}

static void idle_frame(struct ev_eloop *eloop, void *unused, void *data)
{
	struct wlt_window *wnd = data;

	do_frame(wnd);
}

/*
 * Buffer Handling and Frame Scheduling
 * We use wl_shm for buffer allocation. This means, we have a single buffer
 * client side and the server loads it into its backbuffer for rendering. If the
 * server does not do this, we are screwed anyway, but that's on behalf of the
 * server, so we don't care.
 *
 * However, this means, when we create a buffer, we need to notify the
 * compositor and then wait until the compositor has created its back-buffer,
 * before we continue using this buffer. We can use the "frame" callback to get
 * notified about this.
 *
 * The logic we have is:
 * You set the boolean flags what action is needed in wlt_window and then call
 * "schedule_frame()". If we didn't already do any buffer operations in this
 * frame, then this function schedules an idle-callback which then performs
 * the requested functions (flags in wlt_window). Afterwards, it sets a marker
 * that this frame was already used and schedules a frame-callback.
 * If during this time another call to schedule_frame() is made, we do nothing
 * but wait for the frame-callback. It will then directly perform all the
 * requested functions and reschedule a frame-callback.
 * If nothing was schedule for one frame, we have no more interest in
 * frame-callbacks and thus we set "need_frame" to false again and don't
 * schedule any more frame-callbacks.
 */
static void schedule_frame(struct wlt_window *wnd)
{
	int ret;

	if (!wnd)
		return;

	wnd->need_frame = true;

	if (wnd->idle_pending)
		return;

	ret = ev_eloop_register_idle_cb(wnd->disp->eloop, idle_frame, wnd,
					EV_NORMAL);
	if (ret)
		log_error("cannot schedule idle callback: %d", ret);
	else
		wnd->idle_pending = true;
}

static void shell_surface_configure(void *data, struct wl_shell_surface *s,
				    uint32_t edges, int32_t width,
				    int32_t height)
{
	struct wlt_window *wnd = data;

	if (width <= 0)
		width = 1;
	if (height <= 0)
		height = 1;

	wnd->resize_edges = edges;
	wlt_window_set_size(wnd, width, height);
}

static void shell_surface_popup_done(void *data, struct wl_shell_surface *s)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	.ping = shell_surface_ping,
	.configure = shell_surface_configure,
	.popup_done = shell_surface_popup_done,
};

static void close_window(struct ev_eloop *eloop, void *unused, void *data)
{
	struct wlt_window *wnd = data;

	ev_eloop_unregister_idle_cb(eloop, close_window, wnd, EV_NORMAL);
	wnd->close_pending = false;

	if (wnd->close_cb)
		wnd->close_cb(wnd, wnd->data);
}

int wlt_display_create_window(struct wlt_display *disp,
			      struct wlt_window **out,
			      unsigned int width,
			      unsigned int height,
			      void *data)
{
	struct wlt_window *wnd;
	int ret;

	if (!disp || !out || !width || !height)
		return -EINVAL;

	if (disp->state != STATE_RUNNING) {
		log_error("cannot create window, display is not running but in state %u",
			  disp->state);
		return -EBUSY;
	}

	wnd = malloc(sizeof(*wnd));
	if (!wnd)
		return -ENOMEM;
	memset(wnd, 0, sizeof(*wnd));
	wnd->ref = 1;
	wnd->disp = disp;
	wnd->data = data;
	shl_dlist_init(&wnd->widget_list);

	wnd->w_surface = wl_compositor_create_surface(disp->w_comp);
	if (!wnd->w_surface) {
		log_error("cannot create wayland surface");
		ret = -EFAULT;
		goto err_free;
	}

	wnd->w_shell_surface = wl_shell_get_shell_surface(disp->w_shell,
							  wnd->w_surface);
	if (!wnd->w_shell_surface) {
		log_error("cannot retrieve shell-surface for wayland surface");
		ret = -EFAULT;
		goto err_surface;
	}

	wl_shell_surface_add_listener(wnd->w_shell_surface,
				      &shell_surface_listener, wnd);
	wl_shell_surface_set_toplevel(wnd->w_shell_surface);

	ret = resize_window(wnd, width, height);
	if (ret)
		goto err_shell_surface;

	wlt_display_ref(disp);
	shl_dlist_link(&disp->window_list, &wnd->list);
	*out = wnd;
	return 0;

err_shell_surface:
	wl_shell_surface_destroy(wnd->w_shell_surface);
err_surface:
	wl_surface_destroy(wnd->w_surface);
err_free:
	free(wnd);
	return ret;
}

void wlt_window_ref(struct wlt_window *wnd)
{
	if (!wnd || !wnd->ref)
		return;

	++wnd->ref;
}

void wlt_window_unref(struct wlt_window *wnd)
{
	struct wlt_widget *widget;

	if (!wnd || !wnd->ref || --wnd->ref)
		return;

	while (!shl_dlist_empty(&wnd->widget_list)) {
		widget = shl_dlist_entry(wnd->widget_list.next,
					 struct wlt_widget, list);
		wlt_widget_destroy(widget);
	}

	if (wnd->close_pending)
		ev_eloop_unregister_idle_cb(wnd->disp->eloop, close_window,
					    wnd, EV_NORMAL);
	if (wnd->idle_pending)
		ev_eloop_unregister_idle_cb(wnd->disp->eloop, idle_frame, wnd,
					    EV_NORMAL);
	shl_dlist_unlink(&wnd->list);
	if (wnd->w_frame)
		wl_callback_destroy(wnd->w_frame);
	if (wnd->w_buffer)
		wl_buffer_destroy(wnd->w_buffer);
	if (wnd->pool)
		wlt_pool_free(wnd->pool);
	wl_shell_surface_destroy(wnd->w_shell_surface);
	wl_surface_destroy(wnd->w_surface);
	wlt_display_unref(wnd->disp);
	free(wnd);
}

int wlt_window_create_widget(struct wlt_window *wnd,
			     struct wlt_widget **out,
			     void *data)
{
	struct wlt_widget *widget;

	if (!wnd || !out)
		return -EINVAL;

	widget = malloc(sizeof(*widget));
	if (!widget)
		return -ENOMEM;
	memset(widget, 0, sizeof(*widget));
	widget->wnd = wnd;
	widget->data = data;

	wlt_window_set_size(wnd, wnd->buffer.width, wnd->buffer.height);
	shl_dlist_link_tail(&wnd->widget_list, &widget->list);
	*out = widget;
	return 0;
}

void wlt_window_schedule_redraw(struct wlt_window *wnd)
{
	if (!wnd)
		return;

	schedule_frame(wnd);
}

void wlt_window_damage(struct wlt_window *wnd,
		       struct wlt_rect *damage)
{
	if (!wnd || !damage || wnd->skip_damage)
		return;

	wl_surface_damage(wnd->w_surface, damage->x, damage->y,
			  damage->width, damage->height);
	wl_surface_commit(wnd->w_surface);
}

void wlt_window_get_buffer(struct wlt_window *wnd,
			   const struct wlt_rect *alloc,
			   struct wlt_shm_buffer *buf)
{
	struct wlt_shm_buffer *rbuf;

	if (!wnd || !buf)
		return;

	rbuf = &wnd->buffer;

	if (!alloc) {
		memcpy(buf, rbuf, sizeof(*buf));
		return;
	}

	if (alloc->x >= rbuf->width ||
	    alloc->y >= rbuf->height) {
		memset(buf, 0, sizeof(*buf));
		return;
	}

	/* set width */
	if (alloc->x + alloc->width > rbuf->width)
		buf->width = rbuf->width - alloc->x;
	else
		buf->width = alloc->width;

	/* set height */
	if (alloc->y + alloc->height > rbuf->height)
		buf->height = rbuf->height - alloc->y;
	else
		buf->height = alloc->height;

	/* set stride */
	buf->stride  = rbuf->stride;

	/* set data */
	buf->data  = rbuf->data;
	buf->data += alloc->y * rbuf->stride;
	buf->data += alloc->x * 4;
}

void wlt_window_move(struct wlt_window *wnd)
{
	if (!wnd)
		return;

	wl_shell_surface_move(wnd->w_shell_surface,
			      wnd->disp->w_seat,
			      wnd->disp->last_serial);
}

void wlt_window_resize(struct wlt_window *wnd, uint32_t edges)
{
	if (!wnd)
		return;

	wl_shell_surface_resize(wnd->w_shell_surface,
				wnd->disp->w_seat,
				wnd->disp->last_serial,
				edges);
}

int wlt_window_set_size(struct wlt_window *wnd,
			unsigned int width, unsigned int height)
{
	if (!wnd)
		return -EINVAL;

	wnd->new_width = width;
	wnd->new_height = height;
	wnd->need_resize = true;
	schedule_frame(wnd);

	return 0;
}

void wlt_window_set_cursor(struct wlt_window *wnd, unsigned int cursor)
{
	if (!wnd)
		return;

	set_cursor(wnd->disp, cursor);
}

void wlt_window_set_close_cb(struct wlt_window *wnd,
			     wlt_window_close_cb cb)
{
	if (!wnd)
		return;

	wnd->close_cb = cb;
}

void wlt_window_close(struct wlt_window *wnd)
{
	if (!wnd)
		return;

	wnd->close_pending = true;
	ev_eloop_register_idle_cb(wnd->disp->eloop, close_window, wnd,
				  EV_NORMAL);
}

void wlt_window_toggle_maximize(struct wlt_window *wnd)
{
	if (!wnd)
		return;

	if (wnd->maximized) {
		if (!wnd->fullscreen) {
			wl_shell_surface_set_toplevel(wnd->w_shell_surface);
			wlt_window_set_size(wnd, wnd->saved_width,
					    wnd->saved_height);
		}
	} else {
		if (!wnd->fullscreen) {
			wnd->saved_width = wnd->buffer.width;
			wnd->saved_height = wnd->buffer.height;
			wl_shell_surface_set_maximized(wnd->w_shell_surface,
						       NULL);
		}
	}

	wnd->maximized = !wnd->maximized;
}

bool wlt_window_is_maximized(struct wlt_window *wnd)
{
	return wnd && wnd->maximized;
}

void wlt_window_toggle_fullscreen(struct wlt_window *wnd)
{
	if (!wnd)
		return;

	if (wnd->fullscreen) {
		if (wnd->maximized) {
			wl_shell_surface_set_maximized(wnd->w_shell_surface,
						       NULL);
		} else {
			wl_shell_surface_set_toplevel(wnd->w_shell_surface);
			wlt_window_set_size(wnd, wnd->saved_width,
					    wnd->saved_height);
		}
	} else {
		if (!wnd->maximized) {
			wnd->saved_width = wnd->buffer.width;
			wnd->saved_height = wnd->buffer.height;
		}
		wl_shell_surface_set_fullscreen(wnd->w_shell_surface,
				WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT,
				0, NULL);
	}

	wnd->fullscreen = !wnd->fullscreen;
}

bool wlt_window_is_fullscreen(struct wlt_window *wnd)
{
	return wnd && wnd->fullscreen;
}

struct ev_eloop *wlt_window_get_eloop(struct wlt_window *wnd)
{
	if (!wnd)
		return NULL;

	return wnd->disp->eloop;
}

struct wlt_display *wlt_window_get_display(struct wlt_window *wnd)
{
	if (!wnd)
		return NULL;

	return wnd->disp;
}

void wlt_widget_destroy(struct wlt_widget *widget)
{
	if (!widget)
		return;

	if (widget->destroy_cb)
		widget->destroy_cb(widget, widget->data);
	shl_dlist_unlink(&widget->list);
	free(widget);
}

struct wlt_window *wlt_widget_get_window(struct wlt_widget *widget)
{
	if (!widget)
		return NULL;

	return widget->wnd;
}

void wlt_widget_set_redraw_cb(struct wlt_widget *widget,
			      wlt_widget_redraw_cb cb)
{
	if (!widget)
		return;

	widget->redraw_cb = cb;
}

void wlt_widget_set_destroy_cb(struct wlt_widget *widget,
			       wlt_widget_destroy_cb cb)
{
	if (!widget)
		return;

	widget->destroy_cb = cb;
}

void wlt_widget_set_resize_cb(struct wlt_widget *widget,
			      wlt_widget_prepare_resize_cb prepare_cb,
			      wlt_widget_resize_cb cb)
{
	if (!widget)
		return;

	widget->prepare_resize_cb = prepare_cb;
	widget->resize_cb = cb;
}

void wlt_widget_set_pointer_cb(struct wlt_widget *widget,
			       wlt_widget_pointer_enter_cb enter_cb,
			       wlt_widget_pointer_leave_cb leave_cb,
			       wlt_widget_pointer_motion_cb motion_cb,
			       wlt_widget_pointer_button_cb button_cb)
{
	if (!widget)
		return;

	widget->pointer_enter_cb = enter_cb;
	widget->pointer_leave_cb = leave_cb;
	widget->pointer_motion_cb = motion_cb;
	widget->pointer_button_cb = button_cb;
}

void wlt_widget_set_keyboard_cb(struct wlt_widget *widget,
				wlt_widget_keyboard_cb cb)
{
	if (!widget)
		return;

	widget->keyboard_cb = cb;
}
