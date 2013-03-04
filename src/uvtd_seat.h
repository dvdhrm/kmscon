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
 */

#ifndef UVTD_SEAT_H
#define UVTD_SEAT_H

#include <stdlib.h>
#include "eloop.h"

/* sessions */

struct uvtd_session;

enum uvtd_session_event_type {
	UVTD_SESSION_ACTIVATE,
	UVTD_SESSION_DEACTIVATE,
	UVTD_SESSION_UNREGISTER,
};

typedef int (*uvtd_session_cb_t) (struct uvtd_session *session,
				  unsigned int event,
				  void *data);

void uvtd_session_ref(struct uvtd_session *sess);
void uvtd_session_unref(struct uvtd_session *sess);
void uvtd_session_unregister(struct uvtd_session *sess);
bool uvtd_session_is_registered(struct uvtd_session *sess);

bool uvtd_session_is_active(struct uvtd_session *sess);
void uvtd_session_schedule(struct uvtd_session *sess);

void uvtd_session_enable(struct uvtd_session *sess);
void uvtd_session_disable(struct uvtd_session *sess);
bool uvtd_session_is_enabled(struct uvtd_session *sess);

void uvtd_session_notify_deactivated(struct uvtd_session *sess);

/* seats */

struct uvtd_seat;

enum uvtd_seat_event {
	UVTD_SEAT_SLEEP,
};

typedef void (*uvtd_seat_cb_t) (struct uvtd_seat *seat, unsigned int event,
			        void *data);

int uvtd_seat_new(struct uvtd_seat **out, const char *seatname,
		  struct ev_eloop *eloop, uvtd_seat_cb_t cb, void *data);
void uvtd_seat_free(struct uvtd_seat *seat);

const char *uvtd_seat_get_name(struct uvtd_seat *seat);
struct ev_eloop *uvtd_seat_get_eloop(struct uvtd_seat *seat);
int uvtd_seat_sleep(struct uvtd_seat *seat, bool force);
void uvtd_seat_wake_up(struct uvtd_seat *seat);
void uvtd_seat_schedule(struct uvtd_seat *seat, unsigned int id);

int uvtd_seat_register_session(struct uvtd_seat *seat,
			       struct uvtd_session **out,
			       unsigned int id, uvtd_session_cb_t cb,
			       void *data);

#endif /* UVTD_SEAT_H */
