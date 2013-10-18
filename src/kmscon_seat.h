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

#ifndef KMSCON_SEAT_H
#define KMSCON_SEAT_H

#include <stdbool.h>
#include <stdlib.h>
#include "conf.h"
#include "eloop.h"
#include "uterm_input.h"
#include "uterm_video.h"
#include "uterm_vt.h"

struct kmscon_seat;
struct kmscon_session;

enum kmscon_seat_event {
	KMSCON_SEAT_WAKE_UP,
	KMSCON_SEAT_SLEEP,
	KMSCON_SEAT_BACKGROUND,
	KMSCON_SEAT_FOREGROUND,
	KMSCON_SEAT_HUP,
};

typedef int (*kmscon_seat_cb_t) (struct kmscon_seat *seat,
				 unsigned int event,
				 void *data);

enum kmscon_session_event_type {
	KMSCON_SESSION_DISPLAY_NEW,
	KMSCON_SESSION_DISPLAY_GONE,
	KMSCON_SESSION_DISPLAY_REFRESH,
	KMSCON_SESSION_ACTIVATE,
	KMSCON_SESSION_DEACTIVATE,
	KMSCON_SESSION_UNREGISTER,
};

struct kmscon_session_event {
	unsigned int type;
	struct uterm_display *disp;
};

typedef int (*kmscon_session_cb_t) (struct kmscon_session *session,
				    struct kmscon_session_event *event,
				    void *data);

int kmscon_seat_new(struct kmscon_seat **out,
		    struct conf_ctx *main_conf,
		    struct ev_eloop *eloop,
		    struct uterm_vt_master *vtm,
		    unsigned int vt_types,
		    const char *seatname,
		    kmscon_seat_cb_t cb,
		    void *data);
void kmscon_seat_free(struct kmscon_seat *seat);
void kmscon_seat_startup(struct kmscon_seat *seat);

int kmscon_seat_add_display(struct kmscon_seat *seat,
			    struct uterm_display *disp);
void kmscon_seat_remove_display(struct kmscon_seat *seat,
				struct uterm_display *disp);
void kmscon_seat_refresh_display(struct kmscon_seat *seat,
				 struct uterm_display *disp);
int kmscon_seat_add_input(struct kmscon_seat *seat, const char *node);
void kmscon_seat_remove_input(struct kmscon_seat *seat, const char *node);

const char *kmscon_seat_get_name(struct kmscon_seat *seat);
struct uterm_input *kmscon_seat_get_input(struct kmscon_seat *seat);
struct ev_eloop *kmscon_seat_get_eloop(struct kmscon_seat *seat);
struct conf_ctx *kmscon_seat_get_conf(struct kmscon_seat *seat);

void kmscon_seat_schedule(struct kmscon_seat *seat, unsigned int id);

int kmscon_seat_register_session(struct kmscon_seat *seat,
				 struct kmscon_session **out,
				 kmscon_session_cb_t cb,
				 void *data);

void kmscon_session_ref(struct kmscon_session *sess);
void kmscon_session_unref(struct kmscon_session *sess);
void kmscon_session_unregister(struct kmscon_session *sess);
bool kmscon_session_is_registered(struct kmscon_session *sess);

bool kmscon_session_is_active(struct kmscon_session *sess);
int kmscon_session_set_foreground(struct kmscon_session *sess);
int kmscon_session_set_background(struct kmscon_session *sess);
void kmscon_session_schedule(struct kmscon_session *sess);

void kmscon_session_enable(struct kmscon_session *sess);
void kmscon_session_disable(struct kmscon_session *sess);
bool kmscon_session_is_enabled(struct kmscon_session *sess);

void kmscon_session_notify_deactivated(struct kmscon_session *sess);

#endif /* KMSCON_SEAT_H */
