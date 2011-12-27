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
#include <stdint.h>

#include "input.h"

/*
 * These are the values sent by the kernel in the /value/ field of the
 * /input_event/ struct.
 * See Documentation/input/event-codes.txt in the kernel tree.
 */
enum key_state {
	KEY_STATE_RELEASED = 0,
	KEY_STATE_PRESSED = 1,
	KEY_STATE_REPEATED = 2,
};

int new_xkb_desc(const char *layout, const char *variant, const char *options,
							struct xkb_desc **out);
void free_xkb_desc(struct xkb_desc *desc);

void reset_xkb_state(struct xkb_desc *desc, struct xkb_state *state,
								int evdev_fd);

bool process_evdev_key(struct xkb_desc *desc, struct xkb_state *state,
				enum key_state key_state, uint16_t code,
				struct kmscon_input_event *out);
