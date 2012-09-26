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
#include <stdlib.h>
#include <string.h>
#include "eloop.h"
#include "log.h"
#include "pty.h"
#include "tsm_unicode.h"
#include "tsm_screen.h"
#include "tsm_vte.h"
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
};

static void widget_redraw(struct wlt_widget *widget, void *data)
{
	struct wlt_terminal *term = data;
	unsigned int i, j;
	uint8_t *dst;
	uint32_t *line;

	dst = term->buffer.data;
	for (i = 0; i < term->buffer.height; ++i) {
		line = (uint32_t*)dst;
		for (j = 0; j < term->buffer.width; ++j)
			line[j] = 0xffffffff;
		dst += term->buffer.stride;
	}
}

static void widget_resize(struct wlt_widget *widget, struct wlt_rect *alloc,
			  void *data)
{
	struct wlt_terminal *term = data;

	wlt_window_get_buffer(term->wnd, alloc, &term->buffer);

	/* don't allow children */
	alloc->width = 0;
	alloc->height = 0;
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
		log_debug("hangup on PTY");
		/* TODO: signal that to caller */
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

	if (!out || !wnd)
		return -EINVAL;

	term = malloc(sizeof(*term));
	if (!term)
		return -ENOMEM;
	memset(term, 0, sizeof(*term));
	term->wnd = wnd;
	term->eloop = wlt_window_get_eloop(wnd);

	ret = tsm_screen_new(&term->scr, NULL);
	if (ret) {
		log_error("cannot create tsm-screen object");
		goto err_free;
	}

	ret = tsm_vte_new(&term->vte, term->scr, vte_event, term, NULL);
	if (ret) {
		log_error("cannot create tsm-vte object");
		goto err_scr;
	}

	ret = kmscon_pty_new(&term->pty, pty_input, term);
	if (ret) {
		log_error("cannot create pty object");
		goto err_vte;
	}

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
	wlt_widget_set_resize_cb(term->widget, NULL, widget_resize);
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
