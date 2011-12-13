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

#include <inttypes.h>
#include <stdbool.h>

#include "eloop.h"

struct kmscon_input;
struct kmscon_input_device;
typedef void (*kmscon_input_cb) (uint16_t type, uint16_t code, int32_t value);

int kmscon_input_new(struct kmscon_input **out, kmscon_input_cb cb);
void kmscon_input_ref(struct kmscon_input *input);
void kmscon_input_unref(struct kmscon_input *input);

int kmscon_input_connect_eloop(struct kmscon_input *input, struct kmscon_eloop *loop);
void kmscon_input_disconnect_eloop(struct kmscon_input *input);

void kmscon_input_sleep(struct kmscon_input *input);
void kmscon_input_wake_up(struct kmscon_input *input);
bool kmscon_input_is_asleep(struct kmscon_input *input);
