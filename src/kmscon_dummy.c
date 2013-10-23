/*
 * kmscon - Dummy Session
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
 * Dummy Session
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "kmscon_dummy.h"
#include "kmscon_seat.h"
#include "shl_dlist.h"
#include "shl_log.h"

#define LOG_SUBSYSTEM "dummy"

struct display {
	struct shl_dlist list;
	struct uterm_display *disp;
};

struct kmscon_dummy {
	struct kmscon_session *session;
	struct shl_dlist displays;
	bool active;
};

static void dummy_redraw(struct kmscon_dummy *dummy, struct display *d)
{
	struct uterm_mode *mode;
	unsigned int w, h;

	mode = uterm_display_get_current(d->disp);
	w = uterm_mode_get_width(mode);
	h = uterm_mode_get_height(mode);

	uterm_display_fill(d->disp, 0, 0, 0, 0, 0, w, h);
	uterm_display_swap(d->disp, false);
}

static int dummy_session_event(struct kmscon_session *session,
			       struct kmscon_session_event *ev, void *data)
{
	struct kmscon_dummy *dummy = data;
	struct display *d;
	struct shl_dlist *iter;

	switch (ev->type) {
	case KMSCON_SESSION_DISPLAY_NEW:
		d = malloc(sizeof(*d));
		if (!d) {
			log_error("cannot allocate memory for new display");
			break;
		}
		memset(d, 0, sizeof(*d));
		d->disp = ev->disp;
		shl_dlist_link_tail(&dummy->displays, &d->list);
		if (dummy->active)
			dummy_redraw(dummy, d);
		break;
	case KMSCON_SESSION_DISPLAY_GONE:
		shl_dlist_for_each(iter, &dummy->displays) {
			d = shl_dlist_entry(iter, struct display, list);
			if (d->disp != ev->disp)
				continue;

			shl_dlist_unlink(&d->list);
			free(d);
			break;
		}
		break;
	case KMSCON_SESSION_DISPLAY_REFRESH:
		shl_dlist_for_each(iter, &dummy->displays) {
			d = shl_dlist_entry(iter, struct display, list);
			if (d->disp != ev->disp)
				continue;

			if (dummy->active)
				dummy_redraw(dummy, d);
			break;
		}
		break;
	case KMSCON_SESSION_ACTIVATE:
		dummy->active = true;
		shl_dlist_for_each(iter, &dummy->displays) {
			d = shl_dlist_entry(iter, struct display, list);
			dummy_redraw(dummy, d);
		}
		break;
	case KMSCON_SESSION_DEACTIVATE:
		dummy->active = false;
		break;
	case KMSCON_SESSION_UNREGISTER:
		while (!shl_dlist_empty(&dummy->displays)) {
			d = shl_dlist_entry(dummy->displays.prev,
					    struct display, list);
			shl_dlist_unlink(&d->list);
			free(d);
		}

		free(dummy);
		break;
	}

	return 0;
}

int kmscon_dummy_register(struct kmscon_session **out,
			  struct kmscon_seat *seat)
{
	struct kmscon_dummy *dummy;
	int ret;

	if (!out || !seat)
		return -EINVAL;

	dummy = malloc(sizeof(*dummy));
	if (!dummy)
		return -ENOMEM;
	memset(dummy, 0, sizeof(*dummy));
	shl_dlist_init(&dummy->displays);

	ret = kmscon_seat_register_session(seat, &dummy->session,
					   dummy_session_event, dummy);
	if (ret) {
		log_error("cannot register session for dummy: %d", ret);
		goto err_free;
	}

	*out = dummy->session;
	log_debug("new dummy object %p", dummy);
	return 0;

err_free:
	free(dummy);
	return ret;
}
