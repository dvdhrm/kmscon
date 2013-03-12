/*
 * uterm - Linux User-Space Terminal Input Handling
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
 * Input Devices
 * This input object can combine multiple linux input devices into a single
 * device and notifies the application about events. It has several different
 * keyboard backends so the full XKB feature set is available.
 */

#ifndef UTERM_UTERM_INPUT_H
#define UTERM_UTERM_INPUT_H

#include <eloop.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

struct uterm_input;

/* keep in sync with shl_xkb_mods */
enum uterm_input_modifier {
	UTERM_SHIFT_MASK	= (1 << 0),
	UTERM_LOCK_MASK		= (1 << 1),
	UTERM_CONTROL_MASK	= (1 << 2),
	UTERM_ALT_MASK		= (1 << 3),
	UTERM_LOGO_MASK		= (1 << 4),
};

/* keep in sync with TSM_VTE_INVALID */
#define UTERM_INPUT_INVALID 0xffffffff

struct uterm_input_event {
	bool handled;		/* user-controlled, default is false */
	uint16_t keycode;	/* linux keycode - KEY_* - linux/input.h */
	uint32_t ascii;		/* ascii keysym for @keycode */
	unsigned int mods;	/* active modifiers - uterm_modifier mask */

	unsigned int num_syms;	/* number of keysyms */
	uint32_t *keysyms;	/* XKB-common keysym-array - XKB_KEY_* */
	uint32_t *codepoints;	/* ucs4 unicode value or UTERM_INPUT_INVALID */
};

#define UTERM_INPUT_HAS_MODS(_ev, _mods) (((_ev)->mods & (_mods)) == (_mods))

typedef void (*uterm_input_cb) (struct uterm_input *input,
				struct uterm_input_event *ev,
				void *data);

int uterm_input_new(struct uterm_input **out, struct ev_eloop *eloop,
		    const char *model, const char *layout, const char *variant,
		    const char *options, const char *keymap,
		    unsigned int repeat_delay, unsigned int repeat_rate);
void uterm_input_ref(struct uterm_input *input);
void uterm_input_unref(struct uterm_input *input);

void uterm_input_add_dev(struct uterm_input *input, const char *node);
void uterm_input_remove_dev(struct uterm_input *input, const char *node);

int uterm_input_register_cb(struct uterm_input *input, uterm_input_cb cb,
			    void *data);
void uterm_input_unregister_cb(struct uterm_input *input, uterm_input_cb cb,
			       void *data);

void uterm_input_sleep(struct uterm_input *input);
void uterm_input_wake_up(struct uterm_input *input);
bool uterm_input_is_awake(struct uterm_input *input);

#endif /* UTERM_UTERM_INPUT_H */
