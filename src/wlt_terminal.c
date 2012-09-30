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
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "eloop.h"
#include "log.h"
#include "pty.h"
#include "text.h"
#include "tsm_unicode.h"
#include "tsm_screen.h"
#include "tsm_vte.h"
#include "wlt_main.h"
#include "wlt_terminal.h"
#include "wlt_toolkit.h"

#define LOG_SUBSYSTEM "wlt_terminal"

struct wlt_terminal {
	struct ev_eloop *eloop;
	struct wlt_window *wnd;
	struct wlt_widget *widget;
	struct wlt_shm_buffer buffer;

	struct tsm_screen *scr;
	struct tsm_vte *vte;
	struct kmscon_pty *pty;
	struct ev_fd *pty_fd;
	bool pty_open;

	struct kmscon_font *font_normal;
	unsigned int cols;
	unsigned int rows;

	wlt_terminal_cb cb;
	void *data;
};

static int draw_cell(struct tsm_screen *scr,
		     uint32_t id, const uint32_t *ch, size_t len,
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

static void widget_redraw(struct wlt_widget *widget, void *data)
{
	struct wlt_terminal *term = data;
	unsigned int i, j;
	uint8_t *dst;
	uint32_t *line;

	/* black background */
	dst = term->buffer.data;
	for (i = 0; i < term->buffer.height; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < term->buffer.width; ++j)
			line[j] = 0xff << 24;
		dst += term->buffer.stride;
	}

	tsm_screen_draw(term->scr, NULL, draw_cell, NULL, term);
}

static void widget_resize(struct wlt_widget *widget, struct wlt_rect *alloc,
			  void *data)
{
	struct wlt_terminal *term = data;
	int ret;

	wlt_window_get_buffer(term->wnd, alloc, &term->buffer);

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
				  unsigned int width, unsigned int height,
				  unsigned int *new_width,
				  unsigned int *new_height,
				  void *data)
{
	static const bool do_snap = true;
	struct wlt_terminal *term = data;
	unsigned int w, h;

	/* TODO: allow disabling this via command-line */
	if (do_snap) {
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
	} else {
		if (*new_width < width)
			*new_width = width;
		if (*new_height < height)
			*new_height = height;
	}
}

static void widget_key(struct wlt_widget *widget, unsigned int mask,
		       uint32_t sym, uint32_t state, void *data)
{
	struct wlt_terminal *term = data;
	uint32_t ucs4;

	if (state != WL_KEYBOARD_KEY_STATE_PRESSED)
		return;

	ucs4 = xkb_keysym_to_utf32(sym) ? : TSM_VTE_INVALID;

	if (tsm_vte_handle_keyboard(term->vte, sym, mask, ucs4))
		wlt_window_schedule_redraw(term->wnd);
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

	tsm_vte_unref(term->vte);
	tsm_screen_unref(term->scr);
	free(term);
}

int wlt_terminal_new(struct wlt_terminal **out, struct wlt_window *wnd)
{
	struct wlt_terminal *term;
	int ret;
	struct kmscon_font_attr attr = { "", 0, 20, false, false, 0, 0 };

	if (!out || !wnd)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;
	memset(term, 0, sizeof(*term));
	term->wnd = wnd;
	term->eloop = wlt_window_get_eloop(wnd);
	term->cols = 80;
	term->rows = 24;

	attr.ppi = wlt_conf.font_ppi;
	attr.points = wlt_conf.font_size;
	strncpy(attr.name, wlt_conf.font_name, KMSCON_FONT_MAX_NAME - 1);
	attr.name[KMSCON_FONT_MAX_NAME - 1] = 0;

	ret = kmscon_font_find(&term->font_normal, &attr, wlt_conf.font_engine);
	if (ret) {
		log_error("cannot create font");
		goto err_free;
	}

	ret = tsm_screen_new(&term->scr, log_llog);
	if (ret) {
		log_error("cannot create tsm-screen object");
		goto err_font;
	}

	ret = tsm_vte_new(&term->vte, term->scr, vte_event, term, log_llog);
	if (ret) {
		log_error("cannot create tsm-vte object");
		goto err_scr;
	}

	ret = kmscon_pty_new(&term->pty, pty_input, term);
	if (ret) {
		log_error("cannot create pty object");
		goto err_vte;
	}
	kmscon_pty_set_term(term->pty, "xterm-256color");

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
