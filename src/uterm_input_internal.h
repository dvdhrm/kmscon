/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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

/* Internal definitions */

#ifndef UTERM_INPUT_INTERNAL_H
#define UTERM_INPUT_INTERNAL_H

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "eloop.h"
#include "shl_dlist.h"
#include "shl_misc.h"
#include "uterm_input.h"

enum uterm_input_device_capability {
	UTERM_DEVICE_HAS_KEYS = (1 << 0),
	UTERM_DEVICE_HAS_LEDS = (1 << 1),
};

struct uterm_input_dev {
	struct shl_dlist list;
	struct uterm_input *input;

	unsigned int capabilities;
	int rfd;
	char *node;
	struct ev_fd *fd;
	struct xkb_state *state;
	/* Used in sleep/wake up to store the key's pressed/released state. */
	char key_state_bits[SHL_DIV_ROUND_UP(KEY_CNT, CHAR_BIT)];

	unsigned int num_syms;
	struct uterm_input_event event;
	struct uterm_input_event repeat_event;

	bool repeating;
	struct ev_timer *repeat_timer;
};

struct uterm_input {
	unsigned long ref;
	struct ev_eloop *eloop;
	int awake;
	unsigned int repeat_rate;
	unsigned int repeat_delay;

	struct shl_hook *hook;
	struct xkb_context *ctx;
	struct xkb_keymap *keymap;

	struct shl_dlist devices;
};

static inline bool input_bit_is_set(const unsigned long *array, int bit)
{
	return !!(array[bit / LONG_BIT] & (1LL << (bit % LONG_BIT)));
}

int uxkb_desc_init(struct uterm_input *input,
		   const char *model,
		   const char *layout,
		   const char *variant,
		   const char *options,
		   const char *keymap);
void uxkb_desc_destroy(struct uterm_input *input);

int uxkb_dev_init(struct uterm_input_dev *dev);
void uxkb_dev_destroy(struct uterm_input_dev *dev);
int uxkb_dev_process(struct uterm_input_dev *dev,
		     uint16_t key_state,
		     uint16_t code);
void uxkb_dev_sleep(struct uterm_input_dev *dev);
void uxkb_dev_wake_up(struct uterm_input_dev *dev);

#endif /* UTERM_INPUT_INTERNAL_H */
