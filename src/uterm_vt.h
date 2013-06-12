/*
 * uterm - Linux User-Space Terminal VT API
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * Virtual Terminals
 * Virtual terminals allow controlling multiple virtual terminals on one real
 * terminal. It is multi-seat capable and fully asynchronous.
 */

#ifndef UTERM_UTERM_VT_H
#define UTERM_UTERM_VT_H

#include <eloop.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <uterm_input.h>

struct uterm_vt;
struct uterm_vt_master;

enum uterm_vt_action {
	UTERM_VT_ACTIVATE,
	UTERM_VT_DEACTIVATE,
	UTERM_VT_HUP,
};

enum uterm_vt_flags {
	UTERM_VT_FORCE = 0x01,
};

struct uterm_vt_event {
	unsigned int action;
	unsigned int flags;
	int target;
};

enum uterm_vt_type {
	UTERM_VT_REAL = 0x01,
	UTERM_VT_FAKE = 0x02,
};

typedef int (*uterm_vt_cb) (struct uterm_vt *vt, struct uterm_vt_event *ev,
			    void *data);

int uterm_vt_master_new(struct uterm_vt_master **out,
			struct ev_eloop *eloop);
void uterm_vt_master_ref(struct uterm_vt_master *vtm);
void uterm_vt_master_unref(struct uterm_vt_master *vtm);

int uterm_vt_master_activate_all(struct uterm_vt_master *vtm);
int uterm_vt_master_deactivate_all(struct uterm_vt_master *vtm);

int uterm_vt_allocate(struct uterm_vt_master *vt, struct uterm_vt **out,
		      unsigned int allowed_types,
		      const char *seat, struct uterm_input *input,
		      const char *vt_name, uterm_vt_cb cb, void *data);
void uterm_vt_deallocate(struct uterm_vt *vt);
void uterm_vt_ref(struct uterm_vt *vt);
void uterm_vt_unref(struct uterm_vt *vt);

int uterm_vt_activate(struct uterm_vt *vt);
int uterm_vt_deactivate(struct uterm_vt *vt);
void uterm_vt_retry(struct uterm_vt *vt);
unsigned int uterm_vt_get_type(struct uterm_vt *vt);
unsigned int uterm_vt_get_num(struct uterm_vt *vt);

#endif /* UTERM_UTERM_VT_H */
