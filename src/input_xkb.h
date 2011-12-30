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

#ifndef KMSCON_INPUT_XKB_H
#define KMSCON_INPUT_XKB_H

#include <inttypes.h>
#include <X11/extensions/XKBcommon.h>
#include "input.h"

int kmscon_xkb_new_desc(const char *layout, const char *variant,
						const char *options,
						struct xkb_desc **out);
void kmscon_xkb_free_desc(struct xkb_desc *desc);

void kmscon_xkb_reset_state(struct xkb_desc *desc,
						struct xkb_state *state,
						int evdev_fd);

bool kmscon_xkb_process_evdev_key(struct xkb_desc *desc,
						struct xkb_state *state,
						enum kmscon_key_state key_state,
						uint16_t code,
						struct kmscon_input_event *out);

#endif /* KMSCON_INPUT_XKB_H */
