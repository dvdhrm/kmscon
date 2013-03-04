/*
 * wlt - Terminals
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
 * Wayland Terminal console helpers
 */

#include <errno.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "conf.h"
#include "eloop.h"
#include "font.h"
#include "pty.h"
#include "shl_log.h"
#include "shl_misc.h"
#include "tsm_unicode.h"
#include "tsm_screen.h"
#include "tsm_vte.h"
#include "uterm_video.h"
#include "wlt_main.h"
#include "wlt_terminal.h"
#include "wlt_toolkit.h"

#define LOG_SUBSYSTEM "wlt_terminal"

struct wlt_terminal {
	struct ev_eloop *eloop;
	struct wlt_window *wnd;
	struct wlt_display *disp;
	struct wlt_widget *widget;
	struct wlt_shm_buffer buffer;
	struct wlt_rect alloc;

	struct tsm_screen *scr;
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	struct ev_fd *pty_fd;
	bool pty_open;

	struct kmscon_font_attr font_attr;
	struct kmscon_font *font_normal;
	unsigned int cols;
	unsigned int rows;

	wlt_terminal_cb cb;
	void *data;

	int pointer_x;
	int pointer_y;
	bool in_selection;
	bool selection_started;
	int sel_start_x;
	int sel_start_y;

	int paste_fd;
	struct ev_fd *paste;
	struct wl_data_source *copy;
	char *copy_buf;
	int copy_len;
};

static int draw_cell(struct tsm_screen *scr,
		     uint32_t id, const uint32_t *ch, size_t len,
		     unsigned int chwidth,
		     unsigned int posx, unsigned int posy,
		     const struct tsm_screen_attr *attr, void *data)
{
	struct wlt_terminal *term = data;
	const struct kmscon_glyph *glyph;
	int ret;
	unsigned int x, y, tmp, width, height, i, r, g, b;
	uint8_t *dst, *src;
	const struct uterm_video_buffer *buf;
	unsigned int fr, fg, fb, br, bg, bb;
	uint32_t val;

	if (!chwidth)
		return 0;

	if (!len)
		ret = kmscon_font_render_empty(term->font_normal, &glyph);
	else
		ret = kmscon_font_render(term->font_normal, id, ch, len,
					 &glyph);

	if (ret) {
		ret = kmscon_font_render_inval(term->font_normal, &glyph);
		if (ret)
			return ret;
	}

	buf = &glyph->buf;
	x = posx * term->font_normal->attr.width;
	y = posy * term->font_normal->attr.height;

	if (attr->inverse) {
		fr = attr->br;
		fg = attr->bg;
		fb = attr->bb;
		br = attr->fr;
		bg = attr->fg;
		bb = attr->fb;
	} else {
		fr = attr->fr;
		fg = attr->fg;
		fb = attr->fb;
		br = attr->br;
		bg = attr->bg;
		bb = attr->bb;
	}

	tmp = x + buf->width;
	if (tmp < x || x >= term->buffer.width)
		return 0;
	if (tmp > term->buffer.width)
		width = term->buffer.width - x;
	else
		width = buf->width;

	tmp = y + buf->height;
	if (tmp < y || y >= term->buffer.height)
		return 0;
	if (tmp > term->buffer.height)
		height = term->buffer.height - y;
	else
		height = buf->height;

	dst = term->buffer.data;
	dst = &dst[y * term->buffer.stride + x * 4];
	src = buf->data;

	/* Division by 256 instead of 255 increases
	 * speed by like 20% on slower machines.
	 * Downside is, full white is 254/254/254
	 * instead of 255/255/255. */
	while (height--) {
		for (i = 0; i < width; ++i) {
			if (src[i] == 0) {
				r = br;
				g = bg;
				b = bb;
			} else if (src[i] == 255) {
				r = fr;
				g = fg;
				b = fb;
			} else {
				r = fr * src[i] +
				    br * (255 - src[i]);
				r /= 256;
				g = fg * src[i] +
				    bg * (255 - src[i]);
				g /= 256;
				b = fb * src[i] +
				    bb * (255 - src[i]);
				b /= 256;
			}
			val = (0xff << 24) | (r << 16) | (g << 8) | b;
			((uint32_t*)dst)[i] = val;
		}
		dst += term->buffer.stride;
		src += buf->stride;
	}

	return 0;
}

static void draw_background(struct wlt_terminal *term)
{
	uint8_t *dst;
	uint32_t *line;
	unsigned int i, j, w, h;

	/* when maximized, we might have a right and bottom border. So draw
	 * a black background for everything beyond grid-size.
	 * TODO: we should catch the color from tsm_screen instead of using
	 * black background by default. */
	w = term->buffer.width;
	w /= term->font_normal->attr.width;
	w *= term->font_normal->attr.width;

	h = term->buffer.height;
	h /= term->font_normal->attr.height;
	h *= term->font_normal->attr.height;

	dst = term->buffer.data;
	for (i = 0; i < term->buffer.height; ++i) {
		line = (uint32_t*)dst;
		if (i >= h)
			j = 0;
		else
			j = w;
		for ( ; j < term->buffer.width; ++j)
			line[j] = 0xff << 24;
		dst += term->buffer.stride;
	}
}

static void widget_redraw(struct wlt_widget *widget, unsigned int flags,
			  void *data)
{
	struct wlt_terminal *term = data;

	draw_background(term);
	tsm_screen_draw(term->scr, NULL, draw_cell, NULL, term);
}

static void widget_resize(struct wlt_widget *widget, unsigned int flags,
			  struct wlt_rect *alloc, void *data)
{
	struct wlt_terminal *term = data;
	int ret;

	wlt_window_get_buffer(term->wnd, alloc, &term->buffer);
	memcpy(&term->alloc, alloc, sizeof(*alloc));

	/* don't allow children */
	alloc->width = 0;
	alloc->height = 0;

	term->cols = term->buffer.width / term->font_normal->attr.width;
	if (term->cols < 1)
		term->cols = 1;
	term->rows = term->buffer.height / term->font_normal->attr.height;
	if (term->rows < 1)
		term->rows = 1;

	ret = tsm_screen_resize(term->scr, term->cols, term->rows);
	if (ret)
		log_error("cannot resize TSM screen: %d", ret);
	kmscon_pty_resize(term->pty, term->cols, term->rows);
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
	struct wlt_terminal *term = data;
	unsigned int w, h;

	/* We are a catch-all handler. That is, we use all space that is
	 * available. We must be called _last_, which is guaranteed by
	 * registering the widget as last widget.
	 * All previous handlers put their size constraints into the arguments
	 * and we need to make sure to not break them.
	 * Every redraw-handler is guaranteed to work for every size, but still,
	 * we should try to avoid invalid-sizes to not generate artifacts. */

	if (flags & WLT_WINDOW_MAXIMIZED ||
	    flags & WLT_WINDOW_FULLSCREEN) {
		/* if maximized, always use requested size */
		*new_width = width;
		*new_height = height;
	} else {
		/* In normal mode, we want the console to "snap" to grid-sizes.
		 * That is, resizing is in steps instead of smooth. To guarantee
		 * that, we use the font-width/height and try to make the
		 * console as big as possible to fit the needed size.
		 * However, we also must make sure the minimal size is always
		 * guaranteed. */

		if (*new_width >= width) {
			*new_width += term->font_normal->attr.width;
		} else {
			w = width - *new_width;
			w /= term->font_normal->attr.width;
			if (!w)
				w = 1;
			w *= term->font_normal->attr.width;
			*new_width += w;
		}

		if (*new_width < *min_width) {
			w = *min_width - *new_width;
			w /= term->font_normal->attr.width;
			w += 1;
			w *= term->font_normal->attr.width;
			*new_width += w;
		}

		if (*new_height >= height) {
			*new_height += term->font_normal->attr.height;
		} else {
			h = height - *new_height;
			h /= term->font_normal->attr.height;
			if (!h)
				h = 1;
			h *= term->font_normal->attr.height;
			*new_height += h;
		}

		if (*new_height < *min_height) {
			h = *min_height - *new_height;
			h /= term->font_normal->attr.height;
			h += 1;
			h *= term->font_normal->attr.height;
			*new_height += h;
		}
	}
}

static void paste_event(struct ev_fd *fd, int mask, void *data)
{
	struct wlt_terminal *term = data;
	char buf[4096];
	int ret;

	if (mask & EV_READABLE) {
		ret = read(term->paste_fd, buf, sizeof(buf));
		if (ret == 0) {
			goto err_close;
		} else if (ret < 0) {
			if (errno == EAGAIN)
				return;
			log_error("error on paste-fd (%d): %m", errno);
			goto err_close;
		}

		kmscon_pty_write(term->pty, buf, ret);
		return;
	}

	if (mask & EV_ERR) {
		log_error("error on paste FD");
		goto err_close;
	}

	if (mask & EV_HUP)
		goto err_close;

	return;

err_close:
	close(term->paste_fd);
	ev_eloop_rm_fd(term->paste);
	term->paste = NULL;
}

static void copy_target(void *data, struct wl_data_source *w_source,
			const char *target)
{
}

static void copy_send(void *data, struct wl_data_source *w_source,
		      const char *mime, int32_t fd)
{
	struct wlt_terminal *term = data;
	int ret;

	/* TODO: make this non-blocking */
	ret = write(fd, term->copy_buf, term->copy_len);
	if (ret != term->copy_len)
		log_warning("cannot write whole selection: %d/%d", ret,
			    term->copy_len);
	close(fd);
}

static void copy_cancelled(void *data, struct wl_data_source *w_source)
{
	struct wlt_terminal *term = data;

	wl_data_source_destroy(w_source);
	if (term->copy == w_source)
		term->copy = NULL;
}

static const struct wl_data_source_listener copy_listener = {
	.target = copy_target,
	.send = copy_send,
	.cancelled = copy_cancelled,
};

static bool widget_key(struct wlt_widget *widget, unsigned int mask,
		       uint32_t sym, uint32_t ascii, uint32_t state,
		       bool handled, void *data)
{
	struct wlt_terminal *term = data;
	uint32_t ucs4;
	struct kmscon_font *font;
	int ret;

	if (handled || state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return false;

	ucs4 = xkb_keysym_to_utf32(sym) ? : TSM_VTE_INVALID;

	if (conf_grab_matches(wlt_conf.grab_scroll_up,
			      mask, 1, &sym)) {
		tsm_screen_sb_up(term->scr, 1);
		wlt_window_schedule_redraw(term->wnd);
		return true;
	}
	if (conf_grab_matches(wlt_conf.grab_scroll_down,
			      mask, 1, &sym)) {
		tsm_screen_sb_down(term->scr, 1);
		wlt_window_schedule_redraw(term->wnd);
		return true;
	}
	if (conf_grab_matches(wlt_conf.grab_page_up,
			      mask, 1, &sym)) {
		tsm_screen_sb_page_up(term->scr, 1);
		wlt_window_schedule_redraw(term->wnd);
		return true;
	}
	if (conf_grab_matches(wlt_conf.grab_page_down,
			      mask, 1, &sym)) {
		tsm_screen_sb_page_down(term->scr, 1);
		wlt_window_schedule_redraw(term->wnd);
		return true;
	}

	if (conf_grab_matches(wlt_conf.grab_zoom_in,
			      mask, 1, &sym)) {
		if (term->font_attr.points + 1 < term->font_attr.points)
			return true;

		++term->font_attr.points;
		ret = kmscon_font_find(&font, &term->font_attr,
				       wlt_conf.font_engine);
		if (ret) {
			--term->font_attr.points;
			log_error("cannot create font");
		} else {
			kmscon_font_unref(term->font_normal);
			term->font_normal = font;
			wlt_window_schedule_redraw(term->wnd);
		}
		return true;
	}
	if (conf_grab_matches(wlt_conf.grab_zoom_out,
			      mask, 1, &sym)) {
		if (term->font_attr.points - 1 < 1)
			return true;

		--term->font_attr.points;
		ret = kmscon_font_find(&font, &term->font_attr,
				       wlt_conf.font_engine);
		if (ret) {
			++term->font_attr.points;
			log_error("cannot create font");
		} else {
			kmscon_font_unref(term->font_normal);
			term->font_normal = font;
			wlt_window_schedule_redraw(term->wnd);
		}
		return true;
	}

	if (conf_grab_matches(wlt_conf.grab_paste,
			      mask, 1, &sym)) {
		if (term->paste) {
			log_debug("cannot paste selection, previous paste still in progress");
			return true;
		}

		ret = wlt_display_get_selection_fd(term->disp,
						"text/plain;charset=utf-8");
		if (ret == -ENOENT) {
			log_debug("no selection to paste");
			return true;
		} else if (ret == -EAGAIN) {
			log_debug("unknown mime-time for pasting selection");
			return true;
		} else if (ret < 0) {
			log_error("cannot paste selection: %d", ret);
			return true;
		}

		term->paste_fd = ret;
		ret = ev_eloop_new_fd(term->eloop, &term->paste, ret,
				      EV_READABLE, paste_event, term);
		if (ret) {
			close(ret);
			log_error("cannot create eloop fd: %d", ret);
			return true;
		}

		return true;
	}

	if (conf_grab_matches(wlt_conf.grab_copy,
			      mask, 1, &sym)) {
		if (term->copy) {
			wl_data_source_destroy(term->copy);
			free(term->copy_buf);
			term->copy = NULL;
		}

		ret = wlt_display_new_data_source(term->disp, &term->copy);
		if (ret) {
			log_error("cannot create data source");
			return true;
		}

		term->copy_len = tsm_screen_selection_copy(term->scr,
							   &term->copy_buf);
		if (term->copy_len < 0) {
			if (term->copy_len != -ENOENT)
				log_error("cannot copy TSM selection: %d",
					  term->copy_len);
			wl_data_source_destroy(term->copy);
			term->copy = NULL;
			return true;
		}

		wl_data_source_offer(term->copy, "text/plain;charset=utf-8");
		wl_data_source_add_listener(term->copy, &copy_listener, term);
		wlt_display_set_selection(term->disp, term->copy);

		return true;
	}

	if (tsm_vte_handle_keyboard(term->vte, sym, ascii, mask, ucs4)) {
		tsm_screen_sb_reset(term->scr);
		wlt_window_schedule_redraw(term->wnd);
		return true;
	}

	return false;
}

static void pointer_motion(struct wlt_widget *widget,
			   unsigned int x, unsigned int y, void *data)
{
	struct wlt_terminal *term = data;
	unsigned int posx, posy;

	if (!wlt_rect_contains(&term->alloc, x, y)) {
		term->pointer_x = -1;
		term->pointer_y = -1;
		return;
	} else if (term->pointer_x == x - term->alloc.x &&
		   term->pointer_y == y - term->alloc.y) {
		return;
	} else {
		term->pointer_x = x - term->alloc.x;
		term->pointer_y = y - term->alloc.y;
	}

	if (term->in_selection) {
		if (!term->selection_started) {
			term->selection_started = true;
			posx = term->sel_start_x / term->font_normal->attr.width;
			posy = term->sel_start_y / term->font_normal->attr.height;

			tsm_screen_selection_start(term->scr, posx, posy);
		} else {
			posx = term->pointer_x / term->font_normal->attr.width;
			posy = term->pointer_y / term->font_normal->attr.height;

			tsm_screen_selection_target(term->scr, posx, posy);
		}

		wlt_window_schedule_redraw(term->wnd);
	}
}

static void pointer_enter(struct wlt_widget *widget,
			  unsigned int x, unsigned int y, void *data)
{
	struct wlt_terminal *term = data;

	pointer_motion(widget, x, y, term);
}

static void pointer_leave(struct wlt_widget *widget, void *data)
{
	struct wlt_terminal *term = data;

	term->pointer_x = -1;
	term->pointer_y = -1;
}

static void pointer_button(struct wlt_widget *widget,
			   uint32_t button, uint32_t state, void *data)
{
	struct wlt_terminal *term = data;

	if (button != BTN_LEFT)
		return;

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (!term->in_selection &&
		    term->pointer_x >= 0 && term->pointer_y >= 0) {
			term->in_selection = true;
			term->selection_started = false;

			term->sel_start_x = term->pointer_x;
			term->sel_start_y = term->pointer_y;
		}
	} else {
		if (term->pointer_x == term->sel_start_x &&
		    term->pointer_y == term->sel_start_y) {
			tsm_screen_selection_reset(term->scr);
			wlt_window_schedule_redraw(term->wnd);
		}

		term->in_selection = false;
	}
}

static void vte_event(struct tsm_vte *vte, const char *u8, size_t len,
		      void *data)
{
	struct wlt_terminal *term = data;

	kmscon_pty_write(term->pty, u8, len);
}

static void pty_input(struct kmscon_pty *pty, const char *u8, size_t len,
		      void *data)
{
	struct wlt_terminal *term = data;

	if (!len) {
		term->pty_open = false;
		if (term->cb)
			term->cb(term, WLT_TERMINAL_HUP, term->data);
	} else {
		tsm_vte_input(term->vte, u8, len);
		wlt_window_schedule_redraw(term->wnd);
	}
}

static void pty_event(struct ev_fd *fd, int mask, void *data)
{
	struct wlt_terminal *term = data;

	kmscon_pty_dispatch(term->pty);
}

static void widget_destroy(struct wlt_widget *widget, void *data)
{
	struct wlt_terminal *term = data;

	if (term->paste) {
		ev_eloop_rm_fd(term->paste);
		close(term->paste_fd);
	}
	tsm_vte_unref(term->vte);
	tsm_screen_unref(term->scr);
	free(term);
}

int wlt_terminal_new(struct wlt_terminal **out, struct wlt_window *wnd)
{
	struct wlt_terminal *term;
	int ret;

	if (!out || !wnd)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;
	memset(term, 0, sizeof(*term));
	term->wnd = wnd;
	term->disp = wlt_window_get_display(wnd);
	term->eloop = wlt_window_get_eloop(wnd);
	term->cols = 80;
	term->rows = 24;

	term->font_attr.ppi = wlt_conf.font_ppi;
	term->font_attr.points = wlt_conf.font_size;
	term->font_attr.bold = false;
	term->font_attr.italic = false;
	term->font_attr.width = 0;
	term->font_attr.height = 0;
	strncpy(term->font_attr.name, wlt_conf.font_name,
		KMSCON_FONT_MAX_NAME - 1);
	term->font_attr.name[KMSCON_FONT_MAX_NAME - 1] = 0;

	ret = kmscon_font_find(&term->font_normal, &term->font_attr,
			       wlt_conf.font_engine);
	if (ret) {
		log_error("cannot create font");
		goto err_free;
	}

	ret = tsm_screen_new(&term->scr, log_llog, NULL);
	if (ret) {
		log_error("cannot create tsm-screen object");
		goto err_font;
	}
	tsm_screen_set_max_sb(term->scr, wlt_conf.sb_size);

	ret = tsm_vte_new(&term->vte, term->scr, vte_event, term, log_llog,
			  NULL);
	if (ret) {
		log_error("cannot create tsm-vte object");
		goto err_scr;
	}
	tsm_vte_set_palette(term->vte, wlt_conf.palette);

	ret = kmscon_pty_new(&term->pty, pty_input, term);
	if (ret) {
		log_error("cannot create pty object");
		goto err_vte;
	}
	kmscon_pty_set_term(term->pty, "xterm-256color");

	ret = kmscon_pty_set_term(term->pty, wlt_conf.term);
	if (ret)
		goto err_pty;

	ret = kmscon_pty_set_argv(term->pty, wlt_conf.argv);
	if (ret)
		goto err_pty;

	ret = ev_eloop_new_fd(term->eloop, &term->pty_fd,
			      kmscon_pty_get_fd(term->pty),
			      EV_READABLE, pty_event, term);
	if (ret)
		goto err_pty;

	ret = wlt_window_create_widget(wnd, &term->widget, term);
	if (ret) {
		log_error("cannot create terminal widget");
		goto err_pty_fd;
	}

	wlt_widget_set_destroy_cb(term->widget, widget_destroy);
	wlt_widget_set_redraw_cb(term->widget, widget_redraw);
	wlt_widget_set_resize_cb(term->widget, widget_prepare_resize,
				 widget_resize);
	wlt_widget_set_keyboard_cb(term->widget, widget_key);
	wlt_widget_set_pointer_cb(term->widget, pointer_enter, pointer_leave,
				  pointer_motion, pointer_button);
	*out = term;
	return 0;

err_pty_fd:
	ev_eloop_rm_fd(term->pty_fd);
err_pty:
	kmscon_pty_unref(term->pty);
err_vte:
	tsm_vte_unref(term->vte);
err_scr:
	tsm_screen_unref(term->scr);
err_font:
	kmscon_font_unref(term->font_normal);
err_free:
	free(term);
	return ret;
}

void wlt_terminal_destroy(struct wlt_terminal *term)
{
	if (!term)
		return;

	wlt_widget_destroy(term->widget);
}

int wlt_terminal_open(struct wlt_terminal *term, wlt_terminal_cb cb,
		      void *data)
{
	int ret;

	if (!term)
		return -EINVAL;

	if (term->pty_open)
		return -EALREADY;

	term->cb = cb;
	term->data = data;

	kmscon_pty_close(term->pty);
	ret = kmscon_pty_open(term->pty, term->cols, term->rows);
	if (ret)
		return ret;

	term->pty_open = true;
	return 0;
}
