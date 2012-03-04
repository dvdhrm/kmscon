/*
 * kmscon - udev input hotplug and evdev handling
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
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
 * This module provides an input object which can deliver all useful input
 * events to the program.
 *
 * Its use should be as simple as the following (but also see below):
 * - Create a new input object.
 * - Provide a callback function to receive the events.
 * - Connect the input object to a kmscon_eloop.
 * - Wake up the input object to begin receiving input events through the
 *   event loop.
 *
 * A few things to note:
 * - This module uses evdev for input, and reads from input devices directly.
 *   This requires root privileges; waking up the input object will fail
 *   without them.
 * - evdev  has no inhert notion of "focus" like tty input. In other words,
 *   it will deliver input events whether they are intended for the program
 *   or not. This may also pose a security risk. Therefore, make sure to put
 *   the object to sleep when the program is not active, for example by
 *   reacting to VT changes.
 */

#ifndef KMSCON_INPUT_H
#define KMSCON_INPUT_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include "eloop.h"

struct kmscon_input;

enum kmscon_modifier {
	KMSCON_SHIFT_MASK	= (1 << 0),
	KMSCON_LOCK_MASK	= (1 << 1),
	KMSCON_CONTROL_MASK	= (1 << 2),
	KMSCON_MOD1_MASK	= (1 << 3),
	KMSCON_MOD2_MASK	= (1 << 4),
	KMSCON_MOD3_MASK	= (1 << 5),
	KMSCON_MOD4_MASK	= (1 << 6),
	KMSCON_MOD5_MASK	= (1 << 7),
};

#define KMSCON_INPUT_INVALID 0xffffffff

struct kmscon_input_event {
	uint16_t keycode;  /* linux keycode - KEY_* - linux/input.h */
	uint32_t keysym;   /* X keysym - XK_* - X11/keysym.h */
	unsigned int mods; /* active modifiers - kmscon_modifier mask */
	uint32_t unicode;  /* UCS-4 unicode value or KMSCON_INPUT_INVALID */
};

typedef void (*kmscon_input_cb) (struct kmscon_input *input,
				struct kmscon_input_event *ev, void *data);

int kmscon_input_new(struct kmscon_input **out);
void kmscon_input_ref(struct kmscon_input *input);
void kmscon_input_unref(struct kmscon_input *input);

int kmscon_input_connect_eloop(struct kmscon_input *input,
		struct kmscon_eloop *eloop, kmscon_input_cb cb, void *data);
void kmscon_input_disconnect_eloop(struct kmscon_input *input);

void kmscon_input_sleep(struct kmscon_input *input);
void kmscon_input_wake_up(struct kmscon_input *input);
bool kmscon_input_is_asleep(struct kmscon_input *input);

void kmscon_input_stop_bell(struct kmscon_input *input);
void kmscon_input_sound_bell(struct kmscon_input *input,
					unsigned int hz, unsigned int msec);

/* Querying the results of evdev ioctl's. Also used by kbd backends. */
static inline bool kmscon_evdev_bit_is_set(const unsigned long *array, int bit)
{
	return !!(array[bit / LONG_BIT] & (1LL << (bit % LONG_BIT)));
}

#endif /* KMSCON_INPUT_H */
