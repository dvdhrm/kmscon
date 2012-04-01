/*
 * kmscon - Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * Terminal
 * This provides the basic terminal object. This ties together the vt emulation
 * and the output console.
 */

#ifndef KMSCON_TERMINAL_H
#define KMSCON_TERMINAL_H

#include <stdlib.h>
#include "console.h"
#include "eloop.h"
#include "gl.h"
#include "input.h"
#include "uterm.h"

struct kmscon_terminal;

enum kmscon_terminal_etype {
	KMSCON_TERMINAL_HUP,		/* child closed */
	KMSCON_TERMINAL_NO_DISPLAY,	/* no more display connected */
};

typedef void (*kmscon_terminal_event_cb)
		(struct kmscon_terminal *term,
		enum kmscon_terminal_etype type,
		void *data);

int kmscon_terminal_new(struct kmscon_terminal **out,
			struct ev_eloop *loop,
			struct uterm_video *video,
			struct kmscon_input *input);
void kmscon_terminal_ref(struct kmscon_terminal *term);
void kmscon_terminal_unref(struct kmscon_terminal *term);

int kmscon_terminal_open(struct kmscon_terminal *term,
			kmscon_terminal_event_cb event_cb, void *data);
void kmscon_terminal_close(struct kmscon_terminal *term);

int kmscon_terminal_add_display(struct kmscon_terminal *term,
				struct uterm_display *disp);

#endif /* KMSCON_TERMINAL_H */
