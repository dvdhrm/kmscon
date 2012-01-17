/*
 * kmscon - translating key presses to input events
 *
 * Copyright (c) 2012 Ran Benita <ran234@gmail.com>
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
 * This defines the API the keyboard backends need to implement. The main
 * function of a keyboard backend is to translate a kernel input event into a
 * kmscon_input_event (see kmscon_kbd_process_key function).
 *
 * The two exported object are a "keyboard" object and a "keyboard
 * description" object. The keyboard object holds all the device specific
 * private state (e.g. active groups, modifiers). The description object
 * holds all the global information (e.g. layouts, mapping tables).
 */

#ifndef KMSCON_KBD_H
#define KMSCON_KBD_H

#include <inttypes.h>
#include "input.h"

struct kmscon_kbd_desc;
struct kmscon_kbd;

/*
 * These are the values sent by the kernel in the /value/ field of the
 * /input_event/ struct.
 * See Documentation/input/event-codes.txt in the kernel tree.
 */
enum kmscon_key_state {
	KMSCON_KEY_RELEASED = 0,
	KMSCON_KEY_PRESSED = 1,
	KMSCON_KEY_REPEATED = 2,
};

int kmscon_kbd_desc_new(struct kmscon_kbd_desc **out, const char *layout,
				const char *variant, const char *options);
void kmscon_kbd_desc_ref(struct kmscon_kbd_desc *desc);
void kmscon_kbd_desc_unref(struct kmscon_kbd_desc *desc);

int kmscon_kbd_new(struct kmscon_kbd **out, struct kmscon_kbd_desc *desc);
void kmscon_kbd_ref(struct kmscon_kbd *state);
void kmscon_kbd_unref(struct kmscon_kbd *state);

/*
 * This resets the keyboard state in case it got out of sync. It's mainly used
 * to sync our notion of the keyboard state with what the keyboard LEDs show.
 */
void kmscon_kbd_reset(struct kmscon_kbd *kbd, int evdev_fd);

/*
 * This is the entry point to the keyboard processing.
 * We get an evdev scancode and the keyboard state, and should put out a
 * proper input event.
 * Some evdev input events shouldn't result in us sending an input event
 * (e.g. a key release):
 * - If the event was filled out, 0 is returned.
 * - Otherwise, if there was no error, -ENOKEY is returned.
 */
int kmscon_kbd_process_key(struct kmscon_kbd *kbd,
					enum kmscon_key_state key_state,
					uint16_t code,
					struct kmscon_input_event *out);

void kmscon_kbd_keysym_to_string(uint32_t keysym, char *str, size_t size);

#endif /* KMSCON_KBD_H */
