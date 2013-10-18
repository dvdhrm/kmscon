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
#include "kmscon_conf.h"
#include "kmscon_dummy.h"
#include "kmscon_seat.h"
#include "kmscon_terminal.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "uterm_input.h"
#include "uterm_video.h"
#include "uterm_vt.h"

#define LOG_SUBSYSTEM "seat"

struct kmscon_session {
	struct shl_dlist list;
	unsigned long ref;
	struct kmscon_seat *seat;

	bool enabled;
	bool foreground;
	bool deactivating;

	struct ev_timer *timer;

	kmscon_session_cb_t cb;
	void *data;
};

struct kmscon_display {
	struct shl_dlist list;
	struct kmscon_seat *seat;
	struct uterm_display *disp;
	bool activated;
};

enum kmscon_async_schedule {
	SCHEDULE_SWITCH,
	SCHEDULE_VT,
	SCHEDULE_UNREGISTER,
};

struct kmscon_seat {
	struct ev_eloop *eloop;
	struct uterm_vt_master *vtm;
	struct conf_ctx *conf_ctx;
	struct kmscon_conf_t *conf;

	char *name;
	struct uterm_input *input;
	struct uterm_vt *vt;
	struct shl_dlist displays;

	size_t session_count;
	struct shl_dlist sessions;

	bool awake;
	bool foreground;
	struct kmscon_session *current_sess;
	struct kmscon_session *scheduled_sess;
	struct kmscon_session *dummy_sess;

	unsigned int async_schedule;

	kmscon_seat_cb_t cb;
	void *data;
};

static int session_call(struct kmscon_session *sess, unsigned int event,
			struct uterm_display *disp)
{
	struct kmscon_session_event ev;

	if (!sess->cb)
		return 0;

	memset(&ev, 0, sizeof(ev));
	ev.type = event;
	ev.disp = disp;
	return sess->cb(sess, &ev, sess->data);
}

static int session_call_activate(struct kmscon_session *sess)
{
	log_debug("activate session %p", sess);
	return session_call(sess, KMSCON_SESSION_ACTIVATE, NULL);
}

static int session_call_deactivate(struct kmscon_session *sess)
{
	log_debug("deactivate session %p", sess);
	return session_call(sess, KMSCON_SESSION_DEACTIVATE, NULL);
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

static void session_call_display_refresh(struct kmscon_session *sess,
					 struct uterm_display *disp)
{
	session_call(sess, KMSCON_SESSION_DISPLAY_REFRESH, disp);
}

static void activate_display(struct kmscon_display *d)
{
	int ret;
	struct shl_dlist *iter, *tmp;
	struct kmscon_session *s;
	struct kmscon_seat *seat = d->seat;

	if (d->activated || !d->seat->awake || !d->seat->foreground)
		return;

	/* TODO: We always use the default mode for new displays but we should
	 * rather allow the user to specify different modes in the configuration
	 * files. */
	if (uterm_display_get_state(d->disp) == UTERM_DISPLAY_INACTIVE) {
		ret = uterm_display_activate(d->disp, NULL);
		if (ret)
			return;

		d->activated = true;

		ret = uterm_display_set_dpms(d->disp, UTERM_DPMS_ON);
		if (ret)
			log_warning("cannot set DPMS state to on for display: %d",
				    ret);

		shl_dlist_for_each_safe(iter, tmp, &seat->sessions) {
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			session_call_display_new(s, d->disp);
		}
	}
}

static int seat_go_foreground(struct kmscon_seat *seat, bool force)
{
	int ret;
	struct shl_dlist *iter;
	struct kmscon_display *d;

	if (seat->foreground)
		return 0;
	if (!seat->awake || (!force && seat->current_sess))
		return -EBUSY;

	if (seat->cb) {
		ret = seat->cb(seat, KMSCON_SEAT_FOREGROUND, seat->data);
		if (ret) {
			log_warning("cannot put seat %s into foreground: %d",
				    seat->name, ret);
			return ret;
		}
	}

	seat->foreground = true;

	shl_dlist_for_each(iter, &seat->displays) {
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		activate_display(d);
	}

	return 0;
}

static int seat_go_background(struct kmscon_seat *seat, bool force)
{
	int ret;

	if (!seat->foreground)
		return 0;
	if (!seat->awake || (!force && seat->current_sess))
		return -EBUSY;

	if (seat->cb) {
		ret = seat->cb(seat, KMSCON_SEAT_BACKGROUND, seat->data);
		if (ret) {
			log_warning("cannot put seat %s into background: %d",
				    seat->name, ret);
			return ret;
		}
	}

	seat->foreground = false;
	return 0;
}

static int seat_go_asleep(struct kmscon_seat *seat, bool force)
{
	int ret, err = 0;

	if (!seat->awake)
		return 0;
	if (seat->current_sess || seat->foreground) {
		if (force) {
			seat->foreground = false;
			seat->current_sess = NULL;
			err = -EBUSY;
		} else {
			return -EBUSY;
		}
	}

	if (seat->cb) {
		ret = seat->cb(seat, KMSCON_SEAT_SLEEP, seat->data);
		if (ret) {
			log_warning("cannot put seat %s asleep: %d",
				    seat->name, ret);
			if (!force)
				return ret;
		}
	}

	seat->awake = false;
	uterm_input_sleep(seat->input);

	return err;
}

static int seat_go_awake(struct kmscon_seat *seat)
{
	int ret;

	if (seat->awake)
		return 0;

	if (seat->cb) {
		ret = seat->cb(seat, KMSCON_SEAT_WAKE_UP, seat->data);
		if (ret) {
			log_warning("cannot wake up seat %s: %d", seat->name,
				    ret);
			return ret;
		}
	}

	seat->awake = true;
	uterm_input_wake_up(seat->input);

	return 0;
}

static int seat_run(struct kmscon_seat *seat)
{
	int ret;
	struct kmscon_session *session;

	if (!seat->awake)
		return -EBUSY;
	if (seat->current_sess)
		return 0;

	if (!seat->scheduled_sess) {
		log_debug("no session scheduled to run (num %zu)",
			  seat->session_count);
		return -ENOENT;
	}
	session = seat->scheduled_sess;

	if (session->foreground && !seat->foreground) {
		ret = seat_go_foreground(seat, false);
		if (ret) {
			log_warning("cannot put seat %s into foreground for session %p",
				    seat->name, session);
			return ret;
		}
	} else if (!session->foreground && seat->foreground) {
		ret = seat_go_background(seat, false);
		if (ret) {
			log_warning("cannot put seat %s into background for session %p",
				    seat->name, session);
			return ret;
		}
	}

	ret = session_call_activate(session);
	if (ret) {
		log_warning("cannot activate session %p: %d", session, ret);
		return ret;
	}

	seat->current_sess = session;

	return 0;
}

static void session_deactivate(struct kmscon_session *sess)
{
	if (sess->seat->current_sess != sess)
		return;

	sess->seat->async_schedule = SCHEDULE_SWITCH;
	sess->deactivating = false;
	sess->seat->current_sess = NULL;
}

static int seat_pause(struct kmscon_seat *seat, bool force)
{
	int ret;

	if (!seat->current_sess)
		return 0;

	seat->current_sess->deactivating = true;
	ret = session_call_deactivate(seat->current_sess);
	if (ret) {
		if (ret == -EINPROGRESS)
			log_debug("pending deactivation for session %p",
				  seat->current_sess);
		else
			log_warning("cannot deactivate session %p: %d",
				    seat->current_sess, ret);
		if (!force)
			return ret;
	}

	session_deactivate(seat->current_sess);

	return ret;
}

static void seat_reschedule(struct kmscon_seat *seat)
{
	struct shl_dlist *iter, *start;
	struct kmscon_session *sess;

	if (seat->scheduled_sess && seat->scheduled_sess->enabled)
		return;

	if (seat->current_sess && seat->current_sess->enabled) {
		seat->scheduled_sess = seat->current_sess;
		return;
	}

	if (seat->current_sess)
		start = &seat->current_sess->list;
	else
		start = &seat->sessions;

	shl_dlist_for_each_but_one(iter, start, &seat->sessions) {
		sess = shl_dlist_entry(iter, struct kmscon_session, list);
		if (sess == seat->dummy_sess || !sess->enabled)
			continue;
		seat->scheduled_sess = sess;
		return;
	}

	if (seat->dummy_sess && seat->dummy_sess->enabled)
		seat->scheduled_sess = seat->dummy_sess;
	else
		seat->scheduled_sess = NULL;
}

static bool seat_has_schedule(struct kmscon_seat *seat)
{
	return seat->scheduled_sess &&
	       seat->scheduled_sess != seat->current_sess;
}

static int seat_switch(struct kmscon_seat *seat)
{
	int ret;

	seat->async_schedule = SCHEDULE_SWITCH;
	ret = seat_pause(seat, false);
	if (ret)
		return ret;

	return seat_run(seat);
}

static void seat_next(struct kmscon_seat *seat)
{
	struct shl_dlist *cur, *iter;
	struct kmscon_session *s, *next;

	if (seat->current_sess)
		cur = &seat->current_sess->list;
	else if (seat->session_count)
		cur = &seat->sessions;
	else
		return;

	next = NULL;
	if (!seat->current_sess && seat->dummy_sess &&
	    seat->dummy_sess->enabled)
		next = seat->dummy_sess;

	shl_dlist_for_each_but_one(iter, cur, &seat->sessions) {
		s = shl_dlist_entry(iter, struct kmscon_session, list);
		if (!s->enabled || seat->dummy_sess == s)
			continue;

		next = s;
		break;
	}

	if (!next)
		return;

	seat->scheduled_sess = next;
	seat_switch(seat);
}

static void seat_prev(struct kmscon_seat *seat)
{
	struct shl_dlist *cur, *iter;
	struct kmscon_session *s, *prev;

	if (seat->current_sess)
		cur = &seat->current_sess->list;
	else if (seat->session_count)
		cur = &seat->sessions;
	else
		return;

	prev = NULL;
	if (!seat->current_sess && seat->dummy_sess &&
	    seat->dummy_sess->enabled)
		prev = seat->dummy_sess;

	shl_dlist_for_each_reverse_but_one(iter, cur, &seat->sessions) {
		s = shl_dlist_entry(iter, struct kmscon_session, list);
		if (!s->enabled || seat->dummy_sess == s)
			continue;

		prev = s;
		break;
	}

	if (!prev)
		return;

	seat->scheduled_sess = prev;
	seat_switch(seat);
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

static void seat_refresh_display(struct kmscon_seat *seat,
				 struct kmscon_display *d)
{
	struct shl_dlist *iter;
	struct kmscon_session *s;

	log_debug("refresh display %p from seat %s", d->disp, seat->name);

	if (d->activated) {
		shl_dlist_for_each(iter, &seat->sessions) {
			s = shl_dlist_entry(iter, struct kmscon_session, list);
			session_call_display_refresh(s, d->disp);
		}
	}
}

static int seat_vt_event(struct uterm_vt *vt, struct uterm_vt_event *ev,
			 void *data)
{
	struct kmscon_seat *seat = data;
	int ret;

	switch (ev->action) {
	case UTERM_VT_ACTIVATE:
		ret = seat_go_awake(seat);
		if (ret)
			return ret;
		seat_run(seat);
		break;
	case UTERM_VT_DEACTIVATE:
		seat->async_schedule = SCHEDULE_VT;
		ret = seat_pause(seat, false);
		if (ret)
			return ret;
		ret = seat_go_background(seat, false);
		if (ret)
			return ret;
		ret = seat_go_asleep(seat, false);
		if (ret)
			return ret;
		break;
	case UTERM_VT_HUP:
		if (seat->cb)
			seat->cb(seat, KMSCON_SEAT_HUP, seat->data);
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

	if (ev->handled || !seat->awake)
		return;

	if (conf_grab_matches(seat->conf->grab_session_next,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		seat_next(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_prev,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		seat_prev(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_dummy,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		seat->scheduled_sess = seat->dummy_sess;
		seat_switch(seat);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_session_close,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		s = seat->current_sess;
		if (!s)
			return;
		if (s == seat->dummy_sess)
			return;

		/* First time this is invoked on a session, we simply try
		 * unloading it. If it fails, we give it some time. If this is
		 * invoked a second time, we notice that we already tried
		 * removing it and so we go straight to unregistering the
		 * session unconditionally. */
		if (!s->deactivating) {
			seat->async_schedule = SCHEDULE_UNREGISTER;
			ret = seat_pause(seat, false);
			if (ret)
				return;
		}

		kmscon_session_unregister(s);
		return;
	}
	if (conf_grab_matches(seat->conf->grab_terminal_new,
			      ev->mods, ev->num_syms, ev->keysyms)) {
		ev->handled = true;
		if (!seat->conf->session_control)
			return;
		ret = kmscon_terminal_register(&s, seat,
					       uterm_vt_get_num(seat->vt));
		if (ret == -EOPNOTSUPP) {
			log_notice("terminal support not compiled in");
		} else if (ret) {
			log_error("cannot register terminal session: %d", ret);
		} else {
			s->enabled = true;
			seat->scheduled_sess = s;
			seat_switch(seat);
		}
		return;
	}
}

int kmscon_seat_new(struct kmscon_seat **out,
		    struct conf_ctx *main_conf,
		    struct ev_eloop *eloop,
		    struct uterm_vt_master *vtm,
		    unsigned int vt_types,
		    const char *seatname,
		    kmscon_seat_cb_t cb,
		    void *data)
{
	struct kmscon_seat *seat;
	int ret;
	char *keymap;

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

	/* TODO: The XKB-API currently requires zero-terminated strings as
	 * keymap input. Hence, we have to read it in instead of using mmap().
	 * We should fix this upstream! */
	keymap = NULL;
	if (seat->conf->xkb_keymap && *seat->conf->xkb_keymap) {
		ret = shl_read_file(seat->conf->xkb_keymap, &keymap, NULL);
		if (ret)
			log_error("cannot read keymap file %s: %d",
				  seat->conf->xkb_keymap, ret);
	}

	ret = uterm_input_new(&seat->input, seat->eloop,
			      seat->conf->xkb_model,
			      seat->conf->xkb_layout,
			      seat->conf->xkb_variant,
			      seat->conf->xkb_options,
			      keymap,
			      seat->conf->xkb_repeat_delay,
			      seat->conf->xkb_repeat_rate);
	free(keymap);

	if (ret)
		goto err_conf;

	ret = uterm_input_register_cb(seat->input, seat_input_event, seat);
	if (ret)
		goto err_input;

	ret = uterm_vt_allocate(seat->vtm, &seat->vt,
				vt_types, seat->name,
				seat->input, seat->conf->vt, seat_vt_event,
				seat);
	if (ret)
		goto err_input_cb;

	ev_eloop_ref(seat->eloop);
	uterm_vt_master_ref(seat->vtm);
	*out = seat;
	return 0;

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
	int ret;

	if (!seat)
		return;

	ret = seat_pause(seat, true);
	if (ret)
		log_warning("destroying seat %s while session %p is active",
			    seat->name, seat->current_sess);

	ret = seat_go_asleep(seat, true);
	if (ret)
		log_warning("destroying seat %s while still awake: %d",
			    seat->name, ret);

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

void kmscon_seat_startup(struct kmscon_seat *seat)
{
	int ret;
	struct kmscon_session *s;

	if (!seat)
		return;

	ret = kmscon_dummy_register(&s, seat);
	if (ret == -EOPNOTSUPP) {
		log_notice("dummy sessions not compiled in");
	} else if (ret) {
		log_error("cannot register dummy session: %d", ret);
	} else {
		seat->dummy_sess = s;
		kmscon_session_enable(s);
	}

	if (seat->conf->terminal_session) {
		ret = kmscon_terminal_register(&s, seat,
					       uterm_vt_get_num(seat->vt));
		if (ret == -EOPNOTSUPP)
			log_notice("terminal support not compiled in");
		else if (ret)
			log_error("cannot register terminal session");
		else
			kmscon_session_enable(s);
	}

	if (seat->conf->cdev_session) {
		ret = kmscon_cdev_register(&s, seat);
		if (ret == -EOPNOTSUPP)
			log_notice("cdev sessions not compiled in");
		else if (ret)
			log_error("cannot register cdev session");
	}

	if (seat->conf->switchvt ||
	    uterm_vt_get_type(seat->vt) == UTERM_VT_FAKE)
		uterm_vt_activate(seat->vt);
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

	if (!seat || !disp)
		return;

	shl_dlist_for_each(iter, &seat->displays) {
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		if (d->disp != disp)
			continue;

		seat_remove_display(seat, d);
		break;
	}
}

void kmscon_seat_refresh_display(struct kmscon_seat *seat,
				 struct uterm_display *disp)
{
	struct shl_dlist *iter;
	struct kmscon_display *d;

	if (!seat || !disp)
		return;

	shl_dlist_for_each(iter, &seat->displays) {
		d = shl_dlist_entry(iter, struct kmscon_display, list);
		if (d->disp != disp)
			continue;

		seat_refresh_display(seat, d);
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

void kmscon_seat_schedule(struct kmscon_seat *seat, unsigned int id)
{
	struct shl_dlist *iter;
	struct kmscon_session *s, *next;

	if (!seat)
		return;

	next = seat->dummy_sess;
	shl_dlist_for_each(iter, &seat->sessions) {
		s = shl_dlist_entry(iter, struct kmscon_session, list);
		if (!s->enabled || seat->dummy_sess == s ||
		    seat->current_sess == s)
			continue;

		next = s;
		if (!id--)
			break;
	}

	seat->scheduled_sess = next;
	if (seat_has_schedule(seat))
		seat_switch(seat);
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
	sess->foreground = true;

	/* register new sessions next to the current one */
	if (seat->current_sess)
		shl_dlist_link(&seat->current_sess->list, &sess->list);
	else
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
	struct kmscon_seat *seat;
	int ret;
	bool forced = false;

	if (!sess || !sess->seat)
		return;

	log_debug("unregister session %p", sess);

	seat = sess->seat;
	sess->enabled = false;
	if (seat->dummy_sess == sess)
		seat->dummy_sess = NULL;
	seat_reschedule(seat);

	if (seat->current_sess == sess) {
		ret = seat_pause(seat, true);
		if (ret) {
			forced = true;
			log_warning("unregistering active session %p; skipping automatic session-switch",
				    sess);
		}
	}

	shl_dlist_unlink(&sess->list);
	--seat->session_count;
	sess->seat = NULL;

	session_call(sess, KMSCON_SESSION_UNREGISTER, NULL);
	kmscon_session_unref(sess);

	/* If this session was active and we couldn't deactivate it, then it
	 * might still have resources allocated that couldn't get freed. In this
	 * case we should not automatically switch to the next session as it is
	 * very likely that it will not be able to start.
	 * Instead, we stay inactive and wait for user/external input to switch
	 * to another session. This delay will then hopefully be long enough so
	 * all resources got freed. */
	if (!forced)
		seat_run(seat);
}

bool kmscon_session_is_registered(struct kmscon_session *sess)
{
	return sess && sess->seat;
}

bool kmscon_session_is_active(struct kmscon_session *sess)
{
	return sess && sess->seat && sess->seat->current_sess == sess;
}

int kmscon_session_set_foreground(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;
	int ret;

	if (!sess)
		return -EINVAL;
	if (sess->foreground)
		return 0;

	seat = sess->seat;
	if (seat && seat->current_sess == sess && !seat->foreground) {
		ret = seat_go_foreground(seat, true);
		if (ret)
			return ret;
	}

	sess->foreground = true;
	return 0;
}

int kmscon_session_set_background(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;
	int ret;

	if (!sess)
		return -EINVAL;
	if (!sess->foreground)
		return 0;

	seat = sess->seat;
	if (seat && seat->current_sess == sess && seat->foreground) {
		ret = seat_go_background(seat, true);
		if (ret)
			return ret;
	}

	sess->foreground = false;
	return 0;
}

void kmscon_session_schedule(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;

	if (!sess || !sess->seat)
		return;

	seat = sess->seat;
	seat->scheduled_sess = sess;
	seat_reschedule(seat);
	if (seat_has_schedule(seat))
		seat_switch(seat);
}

void kmscon_session_enable(struct kmscon_session *sess)
{
	if (!sess || sess->enabled)
		return;

	log_debug("enable session %p", sess);
	sess->enabled = true;
	if (sess->seat &&
	    (!sess->seat->current_sess ||
	     sess->seat->current_sess == sess->seat->dummy_sess)) {
		sess->seat->scheduled_sess = sess;
		if (seat_has_schedule(sess->seat))
			seat_switch(sess->seat);
	}
}

void kmscon_session_disable(struct kmscon_session *sess)
{
	if (!sess || !sess->enabled)
		return;

	log_debug("disable session %p", sess);
	sess->enabled = false;
}

bool kmscon_session_is_enabled(struct kmscon_session *sess)
{
	return sess && sess->enabled;
}

void kmscon_session_notify_deactivated(struct kmscon_session *sess)
{
	struct kmscon_seat *seat;
	int ret;
	unsigned int sched;

	if (!sess || !sess->seat)
		return;

	seat = sess->seat;
	if (seat->current_sess != sess)
		return;

	sched = seat->async_schedule;
	log_debug("session %p notified core about deactivation (schedule: %u)",
		  sess, sched);
	session_deactivate(sess);
	seat_reschedule(seat);

	if (sched == SCHEDULE_VT) {
		ret = seat_go_background(seat, false);
		if (ret)
			return;
		ret = seat_go_asleep(seat, false);
		if (ret)
			return;
		uterm_vt_retry(seat->vt);
	} else if (sched == SCHEDULE_UNREGISTER) {
		kmscon_session_unregister(sess);
	} else {
		seat_switch(seat);
	}
}
