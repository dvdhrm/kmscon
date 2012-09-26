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
#include "log.h"
#include "tsm_unicode.h"
#include "tsm_screen.h"
#include "tsm_vte.h"
#include "wlt_toolkit.h"

#define LOG_SUBSYSTEM "wlt_terminal"

struct wlt_terminal {
	struct wlt_window *wnd;
	struct wlt_widget *widget;
	struct wlt_shm_buffer buffer;
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

static void widget_destroy(struct wlt_widget *widget, void *data)
{
	struct wlt_terminal *term = data;

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

	ret = wlt_window_create_widget(wnd, &term->widget, term);
	if (ret) {
		log_error("cannot create terminal widget");
		goto err_free;
	}

	wlt_widget_set_destroy_cb(term->widget, widget_destroy);
	wlt_widget_set_redraw_cb(term->widget, widget_redraw);
	wlt_widget_set_resize_cb(term->widget, NULL, widget_resize);
	*out = term;
	return 0;

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
