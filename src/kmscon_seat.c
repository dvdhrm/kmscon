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
#include "conf.h"
#include "eloop.h"
#include "kmscon_cdev.h"
#include "kmscon_compositor.h"
#include "kmscon_conf.h"
#include "kmscon_dummy.h"
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

	bool enabled;

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

	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;

	char *name;
	bool awake;
	struct uterm_input *input;
	struct uterm_vt *vt;
	struct shl_dlist displays;

	size_t session_count;
	struct shl_dlist sessions;
	struct kmscon_session *cur_sess;
	struct kmscon_session *dummy;

	kmscon_seat_cb_t cb;
	void *data;
};

static void session_call(struct kmscon_session *sess, unsigned int event,
			 struct uterm_display *disp)
{
	struct kmscon_session_event ev;

	if (!sess->cb)
		return;

	memset(&ev, 0, sizeof(ev));
	ev.type = event;
	ev.disp = disp;
	sess->cb(sess, &ev, sess->data);
}

static void session_call_activate(struct kmscon_session *sess)
{
	log_debug("activate session %p", sess);
	session_call(sess, KMSCON_SESSION_ACTIVATE, NULL);
}

static void session_call_deactivate(struct kmscon_session *sess)
{
	log_debug("deactivate session %p", sess);
	session_call(sess, KMSCON_SESSION_DEACTIVATE, NULL);
}

static void session_call_display_new(struct kmscon_session *sess,
				     struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_NEW, disp);
}

static void session_call_display_gone(struct kmscon_session *sess,
				      struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_GONE, disp);
}

static int session_activate(struct kmscon_session *sess)
{
	struct kmscon_seat *seat = sess->seat;

	if (seat->cur_sess == sess)
		return 0;
	if (!sess->enabled)
		return -EINVAL;

	if (seat->cur_sess) {
		if (seat->awake)
			session_call_deactivate(seat->cur_sess);
		seat->cur_sess = NULL;
	}

	seat->cur_sess = sess;
	if (seat->awake)
		session_call_activate(sess);

	return 0;
}

static void session_deactivate(struct kmscon_session *sess)
{
	struct kmscon_seat *seat = sess->seat;
	struct shl_dlist *iter;
	struct kmscon_session *s;

	if (seat->cur_sess != sess)
		return;

	if (seat->awake)
		session_call_deactivate(sess);
	seat->cur_sess = NULL;

	shl_dlist_for_each_but_one(iter, &sess->list, &seat->sessions) {
		s = shl_dlist_entry(iter, struct kmscon_session, list);
		if (!s->enabled || s == seat->dummy)
			continue;

		seat->cur_sess = s;
		break;
	}

	if (!seat->cur_sess && seat->dummy)
		seat->cur_sess = seat->dummy;

	if (seat->cur_sess && seat->awake)
		session_call_activate(seat->cur_sess);
}

static void seat_activate_next(struct kmscon_seat *seat)
{
	struct shl_dlist *iter;
	struct kmscon_session *sess;

	if (!seat->cur_sess)
		return;

	shl_dlist_for_each_but_one(iter, &seat->cur_sess->list,
				   &seat->sessions) {
		sess = shl_dlist_entry(iter, struct kmscon_session, list);
		if (sess == seat->dummy)
			continue;
		if (!session_activate(sess))
			break;
	}
}

static void seat_activate_prev(struct kmscon_seat *seat)
{
	struct shl_dlist *iter;
	struct kmscon_session *sess;

	if (!seat->cur_sess)
		return;

	shl_dlist_for_each_reverse_but_one(iter, &seat->cur_sess->list,
					   &seat->sessions) {
		sess = shl_dlist_entry(iter, struct kmscon_session, list);
		if (sess == seat->dummy)
			continue;
		if (!session_activate(sess))
			break;
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
			session_call_display_new(s, d->disp);
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
			session_call_display_gone(s, d->disp);
		}
	}

	uterm_display_unref(d->disp);
	free(d);
}

static int seat_vt_event(struct uterm_vt *vt, struct uterm_vt_event *ev,
			 void *data)
{
	struct kmscon_seat *seat = data;
	struct shl_dlist *iter;
	struct kmscon_display *d;

	switch (ev->action) {
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
			session_call_activate(seat->cur_sess);
		break;
	case UTERM_VT_DEACTIVATE:
		if (seat->cur_sess)
			session_call_deactivate(seat->cur_sess);

		uterm_input_sleep(seat->input);

		if (seat->cb)
			seat->cb(seat, KMSCON_SEAT_SLEEP, seat->data);
		seat->awake = false;
		break;
	}

	return 0;
}

static void seat_input_event(struct uterm_input *input,
			     struct uterm_input_event *ev,
			     void *data)
{
	struct kmscon_seat *seat = data;
	struct kmscon_session *s;
	int ret;

	if (ev->handled)
		return;

	if (conf_grab_matches(seat->conf->grab_session_next,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		seat_activate_next(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_prev,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		seat_activate_prev(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_close,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (seat->cur_sess == seat->dummy)
			return;

		kmscon_session_unregister(seat->cur_sess);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_terminal_new,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		ret = kmscon_terminal_register(&s, seat);
		if (ret == -EOPNOTSUPP) {
			log_notice("terminal support not compiled in");
		} else if (ret) {
			log_error("cannot register terminal session: %d", ret);
		} else {
			kmscon_session_enable(s);
			kmscon_session_activate(s);
		}
		return;
	}
}

int kmscon_seat_new(struct kmscon_seat **out,
		    struct conf_ctx *main_conf,
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

	ret = kmscon_conf_new(&seat->conf_ctx);
	if (ret) {
		log_error("cannot create seat configuration object: %d", ret);
		goto err_name;
	}
	seat->conf = conf_ctx_get_mem(seat->conf_ctx);

	ret = kmscon_conf_load_seat(seat->conf_ctx, main_conf, seat->name);
	if (ret) {
		log_error("cannot parse seat configuration on seat %s: %d",
			  seat->name, ret);
		goto err_conf;
	}

	ret = uterm_input_new(&seat->input, seat->eloop,
			      seat->conf->xkb_model,
			      seat->conf->xkb_layout,
			      seat->conf->xkb_variant,
			      seat->conf->xkb_options,
			      seat->conf->xkb_repeat_delay,
			      seat->conf->xkb_repeat_rate);
	if (ret)
		goto err_conf;

	ret = uterm_input_register_cb(seat->input, seat_input_event, seat);
	if (ret)
		goto err_input;

	ret = uterm_vt_allocate(seat->vtm, &seat->vt, seat->name,
				seat->input, seat->conf->vt, seat_vt_event,
				seat);
	if (ret)
		goto err_input_cb;

	ev_eloop_ref(seat->eloop);
	uterm_vt_master_ref(seat->vtm);
	*out = seat;

	/* register built-in sessions */

	ret = kmscon_dummy_register(&s, seat);
	if (ret == -EOPNOTSUPP) {
		log_notice("dummy sessions not compiled in");
	} else if (ret) {
		log_error("cannot register dummy session: %d", ret);
	} else {
		seat->dummy = s;
		kmscon_session_enable(s);
	}

	ret = kmscon_terminal_register(&s, seat);
	if (ret == -EOPNOTSUPP)
		log_notice("terminal support not compiled in");
	else if (ret)
		goto err_sessions;
	else
		kmscon_session_enable(s);

	ret = kmscon_cdev_register(&s, seat);
	if (ret == -EOPNOTSUPP)
		log_notice("cdev sessions not compiled in");
	else if (ret)
		log_error("cannot register cdev session: %d", ret);

	ret = kmscon_compositor_register(&s, seat);
	if (ret == -EOPNOTSUPP)
		log_notice("compositor support not compiled in");
	else if (ret)
		log_error("cannot register kmscon compositor: %d", ret);

	return 0;

err_sessions:
	kmscon_seat_free(seat);
	return ret;

err_input_cb:
	uterm_input_unregister_cb(seat->input, seat_input_event, seat);
err_input:
	uterm_input_unref(seat->input);
err_conf:
	kmscon_conf_free(seat->conf_ctx);
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
	uterm_input_unregister_cb(seat->input, seat_input_event, seat);
	uterm_input_unref(seat->input);
	kmscon_conf_free(seat->conf_ctx);
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

struct conf_ctx *kmscon_seat_get_conf(struct kmscon_seat *seat)
{
	if (!seat)
		return NULL;

	return seat->conf_ctx;
}

int kmscon_seat_register_session(struct kmscon_seat *seat,
				 struct kmscon_session **out,
				 kmscon_session_cb_t cb,
				 void *data)
{
	struct kmscon_session *sess;
	struct shl_dlist *iter;
	struct kmscon_display *d;

	if (!seat || !out)
		return -EINVAL;

	if (seat->conf->session_max &&
	    seat->session_count >= seat->conf->session_max) {
		log_warning("maximum number of sessions reached (%d), dropping new session",
			    seat->conf->session_max);
		return -EOVERFLOW;
	}

	sess = malloc(sizeof(*sess));
	if (!sess) {
		log_error("cannot allocate memory for new session on seat %s",
			  seat->name);
		return -ENOMEM;
	}

	log_debug("register session %p", sess);

	memset(sess, 0, sizeof(*sess));
	sess->ref = 1;
	sess->seat = seat;
	sess->cb = cb;
	sess->data = data;

	shl_dlist_link_tail(&seat->sessions, &sess->list);
	++seat->session_count;
	*out = sess;

	shl_dlist_for_each(iter, &seat->displays) {
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		session_call_display_new(sess, d->disp);
	}

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

	log_debug("unregister session %p", sess);

	if (sess->seat->dummy == sess)
		sess->seat->dummy = NULL;

	session_deactivate(sess);
	shl_dlist_unlink(&sess->list);
	--sess->seat->session_count;
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

bool kmscon_session_is_active(struct kmscon_session *sess)
{
	return sess && sess->seat && sess->seat->cur_sess == sess;
}

void kmscon_session_enable(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;

	if (!sess)
		return;

	log_debug("enable session %p", sess);

	seat = sess->seat;
	sess->enabled = true;
	if (!sess->seat)
		return;

	if (!seat->cur_sess || seat->cur_sess == seat->dummy)
		session_activate(sess);
}

void kmscon_session_disable(struct kmscon_session *sess)
{
	if (!sess)
		return;

	log_debug("disable session %p", sess);

	if (sess->seat)
		session_deactivate(sess);
	sess->enabled = false;
}

bool kmscon_session_is_enabled(struct kmscon_session *sess)
{
	return sess && sess->enabled;
}
