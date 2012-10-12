/*
 * Seats
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
 * Seats
 * A seat is a single session that is self-hosting and provides all the
 * interaction for a single logged-in user.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "eloop.h"
#include "kmscon_conf.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "log.h"
#include "shl_dlist.h"
#include "uterm.h"

#define LOG_SUBSYSTEM "seat"

struct kmscon_session {
	struct shl_dlist list;
	unsigned long ref;
	struct kmscon_seat *seat;

	kmscon_session_cb_t cb;
	void *data;
};

struct kmscon_display {
	struct shl_dlist list;
	struct kmscon_seat *seat;
	struct uterm_display *disp;
	bool activated;
};

struct kmscon_seat {
	struct ev_eloop *eloop;
	struct uterm_vt_master *vtm;

	char *name;
	bool awake;
	struct uterm_input *input;
	struct uterm_vt *vt;
	struct shl_dlist displays;

	struct shl_dlist sessions;
	struct kmscon_session *cur_sess;

	kmscon_seat_cb_t cb;
	void *data;
};

static void session_call(struct kmscon_session *sess, unsigned int event,
			 struct uterm_display *disp)
{
	if (!sess->cb)
		return;

	sess->cb(sess, event, disp, sess->data);
}

static void session_wake_up(struct kmscon_session *sess)
{
	session_call(sess, KMSCON_SESSION_WAKE_UP, NULL);
}

static void session_sleep(struct kmscon_session *sess)
{
	session_call(sess, KMSCON_SESSION_SLEEP, NULL);
}

static void session_display_new(struct kmscon_session *sess,
				struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_NEW, disp);
}

static void session_display_gone(struct kmscon_session *sess,
				 struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_GONE, disp);
}

static void session_activate(struct kmscon_session *sess)
{
	struct kmscon_seat *seat = sess->seat;

	if (seat->cur_sess == sess)
		return;

	if (seat->cur_sess) {
		if (seat->awake)
			session_sleep(seat->cur_sess);
		seat->cur_sess = NULL;
	}

	seat->cur_sess = sess;
	if (seat->awake)
		session_wake_up(sess);
}

static void session_deactivate(struct kmscon_session *sess)
{
	struct kmscon_seat *seat = sess->seat;

	if (seat->cur_sess != sess)
		return;

	if (seat->awake)
		session_sleep(sess);

	if (shl_dlist_empty(&seat->sessions)) {
		seat->cur_sess = NULL;
	} else {
		seat->cur_sess = shl_dlist_entry(seat->sessions.next,
						 struct kmscon_session,
						 list);
		if (seat->awake)
			session_wake_up(seat->cur_sess);
	}
}

static void activate_display(struct kmscon_display *d)
{
	int ret;
	struct shl_dlist *iter, *tmp;
	struct kmscon_session *s;
	struct kmscon_seat *seat = d->seat;

	if (d->activated)
		return;

	if (uterm_display_get_state(d->disp) == UTERM_DISPLAY_INACTIVE) {
		ret = uterm_display_activate(d->disp, NULL);
		if (ret)
			return;

		d->activated = true;

		shl_dlist_for_each_safe(iter, tmp, &seat->sessions) {
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			session_display_new(s, d->disp);
		}

		ret = uterm_display_set_dpms(d->disp, UTERM_DPMS_ON);
		if (ret)
			log_warning("cannot set DPMS state to on for display: %d",
				    ret);
	}
}

static int seat_add_display(struct kmscon_seat *seat,
			    struct uterm_display *disp)
{
	struct kmscon_display *d;

	log_debug("add display %p to seat %s", disp, seat->name);

	d = malloc(sizeof(*d));
	if (!d)
		return -ENOMEM;
	memset(d, 0, sizeof(*d));
	d->disp = disp;
	d->seat = seat;

	uterm_display_ref(d->disp);
	shl_dlist_link(&seat->displays, &d->list);
	activate_display(d);
	return 0;
}

static void seat_remove_display(struct kmscon_seat *seat,
				struct kmscon_display *d)
{
	struct shl_dlist *iter, *tmp;
	struct kmscon_session *s;

	log_debug("remove display %p from seat %s", d->disp, seat->name);

	shl_dlist_unlink(&d->list);

	if (d->activated) {
		shl_dlist_for_each_safe(iter, tmp, &seat->sessions) {
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			session_display_gone(s, d->disp);
		}
	}

	uterm_display_unref(d->disp);
	free(d);
}

static int seat_vt_event(struct uterm_vt *vt, unsigned int event, void *data)
{
	struct kmscon_seat *seat = data;
	struct shl_dlist *iter;
	struct kmscon_display *d;

	switch (event) {
	case UTERM_VT_ACTIVATE:
		seat->awake = true;
		if (seat->cb)
			seat->cb(seat, KMSCON_SEAT_WAKE_UP, seat->data);

		uterm_input_wake_up(seat->input);

		shl_dlist_for_each(iter, &seat->displays) {
			d = shl_dlist_entry(iter, struct kmscon_display, list);
			activate_display(d);
		}

		if (seat->cur_sess)
			session_wake_up(seat->cur_sess);
		break;
	case UTERM_VT_DEACTIVATE:
		if (seat->cur_sess)
			session_sleep(seat->cur_sess);

		uterm_input_sleep(seat->input);

		if (seat->cb)
			seat->cb(seat, KMSCON_SEAT_SLEEP, seat->data);
		seat->awake = false;
		break;
	}

	return 0;
}

int kmscon_seat_new(struct kmscon_seat **out,
		    struct ev_eloop *eloop,
		    struct uterm_vt_master *vtm,
		    const char *seatname,
		    kmscon_seat_cb_t cb,
		    void *data)
{
	struct kmscon_seat *seat;
	int ret;
	struct kmscon_session *s;

	if (!out || !eloop || !vtm || !seatname)
		return -EINVAL;

	seat = malloc(sizeof(*seat));
	if (!seat)
		return -ENOMEM;
	memset(seat, 0, sizeof(*seat));
	seat->eloop = eloop;
	seat->vtm = vtm;
	seat->cb = cb;
	seat->data = data;
	shl_dlist_init(&seat->displays);
	shl_dlist_init(&seat->sessions);

	seat->name = strdup(seatname);
	if (!seat->name) {
		log_error("cannot copy string");
		ret = -ENOMEM;
		goto err_free;
	}

	ret = uterm_input_new(&seat->input, seat->eloop,
			      kmscon_conf.xkb_layout,
			      kmscon_conf.xkb_variant,
			      kmscon_conf.xkb_options,
			      kmscon_conf.xkb_repeat_delay,
			      kmscon_conf.xkb_repeat_rate);
	if (ret)
		goto err_name;

	ret = uterm_vt_allocate(seat->vtm, &seat->vt, seat->name,
				seat->input, kmscon_conf.vt, seat_vt_event,
				seat);
	if (ret)
		goto err_input;

	ret = kmscon_terminal_register(&s, seat);
	if (ret)
		goto err_vt;

	ev_eloop_ref(seat->eloop);
	uterm_vt_master_ref(seat->vtm);
	*out = seat;
	return 0;

err_vt:
	uterm_vt_deallocate(seat->vt);
err_input:
	uterm_input_unref(seat->input);
err_name:
	free(seat->name);
err_free:
	free(seat);
	return ret;
}

void kmscon_seat_free(struct kmscon_seat *seat)
{
	struct kmscon_display *d;
	struct kmscon_session *s;

	if (!seat)
		return;

	while (!shl_dlist_empty(&seat->sessions)) {
		s = shl_dlist_entry(seat->sessions.next,
				    struct kmscon_session,
				    list);
		kmscon_session_unregister(s);
	}

	while (!shl_dlist_empty(&seat->displays)) {
		d = shl_dlist_entry(seat->displays.next,
				    struct kmscon_display,
				    list);
		seat_remove_display(seat, d);
	}

	uterm_vt_deallocate(seat->vt);
	uterm_input_unref(seat->input);
	free(seat->name);
	uterm_vt_master_unref(seat->vtm);
	ev_eloop_unref(seat->eloop);
	free(seat);
}

int kmscon_seat_add_display(struct kmscon_seat *seat,
			    struct uterm_display *disp)
{
	if (!seat || !disp)
		return -EINVAL;

	return seat_add_display(seat, disp);
}

void kmscon_seat_remove_display(struct kmscon_seat *seat,
				struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct kmscon_display *d;

	shl_dlist_for_each(iter, &seat->displays) {
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		if (d->disp != disp)
			continue;

		seat_remove_display(seat, d);
		break;
	}
}

int kmscon_seat_add_input(struct kmscon_seat *seat, const char *node)
{
	if (!seat || !node)
		return -EINVAL;

	uterm_input_add_dev(seat->input, node);
	return 0;
}

void kmscon_seat_remove_input(struct kmscon_seat *seat, const char *node)
{
	if (!seat || !node)
		return;

	uterm_input_remove_dev(seat->input, node);
}

const char *kmscon_seat_get_name(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->name;
}

struct uterm_input *kmscon_seat_get_input(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->input;
}

struct ev_eloop *kmscon_seat_get_eloop(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->eloop;
}

int kmscon_seat_register_session(struct kmscon_seat *seat,
				 struct kmscon_session **out,
				 kmscon_session_cb_t cb,
				 void *data)
{
	struct kmscon_session *sess;

	if (!seat || !out)
		return -EINVAL;

	sess = malloc(sizeof(*sess));
	if (!sess) {
		log_error("cannot allocate memory for new session on seat %s",
			  seat->name);
		return -ENOMEM;
	}
	memset(sess, 0, sizeof(*sess));
	sess->ref = 1;
	sess->seat = seat;
	sess->cb = cb;
	sess->data = data;

	shl_dlist_link(&seat->sessions, &sess->list);
	*out = sess;

	if (!seat->cur_sess)
		session_activate(sess);

	return 0;
}

void kmscon_session_ref(struct kmscon_session *sess)
{
	if (!sess || !sess->ref)
		return;

	++sess->ref;
}

void kmscon_session_unref(struct kmscon_session *sess)
{
	if (!sess || !sess->ref || --sess->ref)
		return;

	kmscon_session_unregister(sess);
	free(sess);
}

void kmscon_session_unregister(struct kmscon_session *sess)
{
	if (!sess || !sess->seat)
		return;

	shl_dlist_unlink(&sess->list);
	session_deactivate(sess);
	sess->seat = NULL;
	session_call(sess, KMSCON_SESSION_UNREGISTER, NULL);
}

bool kmscon_session_is_registered(struct kmscon_session *sess)
{
	return sess && sess->seat;
}

void kmscon_session_activate(struct kmscon_session *sess)
{
	if (!sess || !sess->seat)
		return;

	session_activate(sess);
}

void kmscon_session_deactivate(struct kmscon_session *sess)
{
	if (!sess || !sess->seat)
		return;

	session_deactivate(sess);
}
