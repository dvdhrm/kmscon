/*
 * uvtd - User-space VT daemon
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@gmail.com>
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
 * Each set of input+output devices form a single seat. Each seat is independent
 * of each other and there can be exactly one user per seat interacting with the
 * system.
 * Per seat, we have multiple sessions. But only one session can be active at a
 * time per seat. We allow external sessions, so session activation/deactivation
 * may be asynchronous.
 *
 * A seat object manages all the sessions for a single seat. As long as a seat
 * is asleep, no session is active. If you wake it up, the seat manager
 * automatically schedules a session. You can then request other sessions to be
 * scheduled and the seat manager will try to deactivate the current session and
 * reactivate the new session.
 *
 * Note that session deactivation may be asynchronous (unless forced). So some
 * calls might return -EINPROGRESS if the session-deactivation is pending. This
 * shouldn't bother the user as the session will notify back soon that the
 * deactivation was successfull. However, if it doesn't the user can chose to
 * perform any other action and we will retry the operation. As a last resort,
 * you can always kill the session by unregistering it or forcing a
 * deactivation.
 * "async_schedule" tracks the task that requested the deactivation of a
 * session. So when the session notifies us that it got deactivated, we know
 * what the user wanted and can perform the requested task now.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_log.h"
#include "uvtd_seat.h"

#define LOG_SUBSYSTEM "seat"

struct uvtd_session {
	struct shl_dlist list;
	unsigned long ref;
	struct uvtd_seat *seat;
	unsigned int id;

	bool enabled;
	bool deactivating;

	uvtd_session_cb_t cb;
	void *data;
};

/* task that requested the pending session-deactivation */
enum uvtd_async_schedule {
	SCHEDULE_NONE,			/* default, causes a reschedule */
	SCHEDULE_SWITCH,		/* causes a reschedule */
	SCHEDULE_SLEEP,			/* puts the seat asleep */
	SCHEDULE_UNREGISTER,		/* unregisters the session */
};

struct uvtd_seat {
	struct ev_eloop *eloop;
	char *name;

	size_t session_count;
	struct shl_dlist sessions;

	bool awake;
	struct uvtd_session *current_sess;
	struct uvtd_session *scheduled_sess;
	struct uvtd_session *dummy_sess;

	unsigned int async_schedule;

	uvtd_seat_cb_t cb;
	void *data;
};

static int session_call(struct uvtd_session *sess, unsigned int event)
{
	if (!sess->cb)
		return 0;

	return sess->cb(sess, event, sess->data);
}

static int session_call_activate(struct uvtd_session *sess)
{
	log_debug("activate session %p", sess);
	return session_call(sess, UVTD_SESSION_ACTIVATE);
}

static int session_call_deactivate(struct uvtd_session *sess)
{
	log_debug("deactivate session %p", sess);
	return session_call(sess, UVTD_SESSION_DEACTIVATE);
}

/* drop the current session as if it was successfully deactivated */
static void seat_yield(struct uvtd_seat *seat)
{
	if (!seat->current_sess)
		return;

	seat->current_sess->deactivating = false;
	seat->current_sess = NULL;
	seat->async_schedule = SCHEDULE_NONE;
}

static int seat_go_asleep(struct uvtd_seat *seat, bool force)
{
	int ret = 0;

	if (!seat->awake)
		return 0;

	if (seat->current_sess) {
		ret = -EBUSY;
		if (!force)
			return ret;

		seat_yield(seat);
	}

	seat->awake = false;

	if (seat->cb)
		seat->cb(seat, UVTD_SEAT_SLEEP, seat->data);

	return ret;
}

static void seat_go_awake(struct uvtd_seat *seat)
{
	if (seat->awake)
		return;

	seat->awake = true;
}

static int seat_run(struct uvtd_seat *seat)
{
	int ret;
	struct uvtd_session *session;

	if (!seat->awake)
		return -EBUSY;
	if (seat->current_sess)
		return 0;

	if (!seat->scheduled_sess) {
		log_debug("no session scheduled to run (num: %zu)",
			  seat->session_count);
		return -ENOENT;
	}
	session = seat->scheduled_sess;

	/* TODO: unregister session and try next on failure */
	ret = session_call_activate(session);
	if (ret) {
		log_warning("cannot activate session %p: %d", session, ret);
		return ret;
	}

	seat->current_sess = session;

	return 0;
}

static int seat_pause(struct uvtd_seat *seat, bool force, unsigned int async)
{
	int ret;

	if (!seat->current_sess)
		return 0;

	/* TODO: pass \force to the session */
	seat->current_sess->deactivating = true;
	ret = session_call_deactivate(seat->current_sess);
	if (ret) {
		if (!force && ret == -EINPROGRESS) {
			seat->async_schedule = async;
			log_debug("pending deactivation for session %p",
				  seat->current_sess);
		} else {
			log_warning("cannot deactivate session %p (%d): %d",
				    seat->current_sess, force, ret);
		}

		if (!force)
			return ret;
	}

	seat_yield(seat);
	return ret;
}

static void seat_reschedule(struct uvtd_seat *seat)
{
	struct shl_dlist *iter, *start;
	struct uvtd_session *sess;

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
		sess = shl_dlist_entry(iter, struct uvtd_session, list);

		if (sess != seat->dummy_sess && sess->enabled) {
			seat->scheduled_sess = sess;
			return;
		}
	}

	if (seat->dummy_sess && seat->dummy_sess->enabled)
		seat->scheduled_sess = seat->dummy_sess;
	else
		seat->scheduled_sess = NULL;
}

static bool seat_has_schedule(struct uvtd_seat *seat)
{
	return seat->scheduled_sess &&
	       seat->scheduled_sess != seat->current_sess;
}

static int seat_switch(struct uvtd_seat *seat)
{
	int ret;

	ret = seat_pause(seat, false, SCHEDULE_SWITCH);
	if (ret)
		return ret;

	return seat_run(seat);
}

static void seat_schedule(struct uvtd_seat *seat, struct uvtd_session *sess)
{
	seat->scheduled_sess = sess;
	seat_reschedule(seat);
	if (seat_has_schedule(seat))
		seat_switch(seat);
}

static void seat_next(struct uvtd_seat *seat, bool reverse)
{
	struct shl_dlist *cur, *iter;
	struct uvtd_session *s, *next;

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

	if (reverse) {
		shl_dlist_for_each_reverse_but_one(iter, cur,
						   &seat->sessions) {
			s = shl_dlist_entry(iter, struct uvtd_session, list);

			if (s->enabled && seat->dummy_sess != s) {
				next = s;
				break;
			}
		}
	} else {
		shl_dlist_for_each_but_one(iter, cur, &seat->sessions) {
			s = shl_dlist_entry(iter, struct uvtd_session, list);

			if (s->enabled && seat->dummy_sess != s) {
				next = s;
				break;
			}
		}
	}

	if (!next)
		return;

	seat_schedule(seat, next);
}

int uvtd_seat_new(struct uvtd_seat **out, const char *seatname,
		  struct ev_eloop *eloop, uvtd_seat_cb_t cb, void *data)
{
	struct uvtd_seat *seat;
	int ret;

	if (!out || !eloop || !seatname)
		return -EINVAL;

	seat = malloc(sizeof(*seat));
	if (!seat)
		return -ENOMEM;
	memset(seat, 0, sizeof(*seat));
	seat->eloop = eloop;
	seat->cb = cb;
	seat->data = data;
	shl_dlist_init(&seat->sessions);

	seat->name = strdup(seatname);
	if (!seat->name) {
		ret = -ENOMEM;
		goto err_free;
	}

	ev_eloop_ref(seat->eloop);
	*out = seat;
	return 0;

err_free:
	free(seat);
	return ret;
}

void uvtd_seat_free(struct uvtd_seat *seat)
{
	struct uvtd_session *s;
	int ret;

	if (!seat)
		return;

	ret = seat_pause(seat, true, SCHEDULE_NONE);
	if (ret)
		log_warning("destroying seat %s while session %p is active",
			    seat->name, seat->current_sess);

	ret = seat_go_asleep(seat, true);
	if (ret)
		log_warning("destroying seat %s while still awake: %d",
			    seat->name, ret);

	while (!shl_dlist_empty(&seat->sessions)) {
		s = shl_dlist_entry(seat->sessions.next, struct uvtd_session,
				    list);
		uvtd_session_unregister(s);
	}

	free(seat->name);
	ev_eloop_unref(seat->eloop);
	free(seat);
}

const char *uvtd_seat_get_name(struct uvtd_seat *seat)
{
	return seat ? seat->name : NULL;
}

struct ev_eloop *uvtd_seat_get_eloop(struct uvtd_seat *seat)
{
	return seat ? seat->eloop : NULL;
}

int uvtd_seat_sleep(struct uvtd_seat *seat, bool force)
{
	int ret, err = 0;

	if (!seat)
		return -EINVAL;

	ret = seat_pause(seat, force, SCHEDULE_SLEEP);
	if (ret) {
		if (force)
			err = ret;
		else
			return ret;
	}

	ret = seat_go_asleep(seat, force);
	if (ret) {
		if (force)
			err = ret;
		else
			return ret;
	}

	return err;
}

void uvtd_seat_wake_up(struct uvtd_seat *seat)
{
	if (!seat)
		return;

	seat_go_awake(seat);
	seat_run(seat);
}

void uvtd_seat_schedule(struct uvtd_seat *seat, unsigned int id)
{
	struct shl_dlist *iter;
	struct uvtd_session *session;
	unsigned int i;

	if (!seat || !id)
		return;

	session = NULL;
	i = id;
	shl_dlist_for_each(iter, &seat->sessions) {
		session = shl_dlist_entry(iter, struct uvtd_session, list);
		if (!--i)
			break;
		if (session->id >= id)
			break;
	}

	if (session)
		seat_schedule(seat, session);
}

int uvtd_seat_register_session(struct uvtd_seat *seat,
			       struct uvtd_session **out,
			       unsigned int id, uvtd_session_cb_t cb,
			       void *data)
{
	struct uvtd_session *sess, *s;
	struct shl_dlist *iter;

	if (!seat || !out)
		return -EINVAL;

	sess = malloc(sizeof(*sess));
	if (!sess)
		return -ENOMEM;

	log_debug("register session %p with id %u on seat %p",
		  sess, id, seat);

	memset(sess, 0, sizeof(*sess));
	sess->ref = 1;
	sess->seat = seat;
	sess->cb = cb;
	sess->data = data;
	sess->id = id;

	++seat->session_count;
	*out = sess;

	if (sess->id) {
		shl_dlist_for_each(iter, &seat->sessions) {
			s = shl_dlist_entry(iter, struct uvtd_session, list);
			if (!s->id || s->id > sess->id) {
				shl_dlist_link_tail(iter, &sess->list);
				return 0;
			}

			if (s->id == sess->id)
				log_warning("session %p shadowed by %p",
					    sess, s);
		}
	}

	shl_dlist_link_tail(&seat->sessions, &sess->list);
	return 0;
}

void uvtd_session_ref(struct uvtd_session *sess)
{
	if (!sess || !sess->ref)
		return;

	++sess->ref;
}

void uvtd_session_unref(struct uvtd_session *sess)
{
	if (!sess || !sess->ref || --sess->ref)
		return;

	uvtd_session_unregister(sess);
	free(sess);
}

void uvtd_session_unregister(struct uvtd_session *sess)
{
	struct uvtd_seat *seat;
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
		ret = seat_pause(seat, true, SCHEDULE_NONE);
		if (ret) {
			forced = true;
			log_warning("unregistering active session %p; skipping automatic session-switch",
				    sess);
		}
	}

	shl_dlist_unlink(&sess->list);
	--seat->session_count;
	sess->seat = NULL;

	session_call(sess, UVTD_SESSION_UNREGISTER);
	uvtd_session_unref(sess);

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

bool uvtd_session_is_registered(struct uvtd_session *sess)
{
	return sess && sess->seat;
}

bool uvtd_session_is_active(struct uvtd_session *sess)
{
	return sess && sess->seat && sess->seat->current_sess == sess;
}

void uvtd_session_schedule(struct uvtd_session *sess)
{
	if (!sess || !sess->seat)
		return;

	seat_schedule(sess->seat, sess);
}

void uvtd_session_enable(struct uvtd_session *sess)
{
	if (!sess || sess->enabled)
		return;

	log_debug("enable session %p", sess);
	sess->enabled = true;

	if (sess->seat &&
	    (!sess->seat->current_sess ||
	     sess->seat->current_sess == sess->seat->dummy_sess))
		seat_schedule(sess->seat, sess);
}

void uvtd_session_disable(struct uvtd_session *sess)
{
	if (!sess || !sess->enabled)
		return;

	log_debug("disable session %p", sess);
	sess->enabled = false;
}

bool uvtd_session_is_enabled(struct uvtd_session *sess)
{
	return sess && sess->enabled;
}

void uvtd_session_notify_deactivated(struct uvtd_session *sess)
{
	struct uvtd_seat *seat;
	unsigned int sched;

	if (!sess || !sess->seat)
		return;

	seat = sess->seat;
	if (seat->current_sess != sess)
		return;

	sched = seat->async_schedule;
	log_debug("session %p notified core about deactivation (schedule: %u)",
		  sess, sched);
	seat_yield(seat);
	seat_reschedule(seat);

	if (sched == SCHEDULE_SLEEP)
		seat_go_asleep(seat, false);
	else if (sched == SCHEDULE_UNREGISTER)
		uvtd_session_unregister(sess);
	else
		seat_run(seat);
}
