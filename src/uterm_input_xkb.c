/*
 * uterm - Linux User-Space Terminal
 *
 * Copyright (c) 2011 Ran Benita <ran234@gmail.com>
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

#include <errno.h>
#include <inttypes.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>
#include "imKStoUCS.h"
#include "log.h"
#include "uterm.h"
#include "uterm_internal.h"

#define LOG_SUBSYSTEM "input_xkb"

struct kbd_desc {
	unsigned long ref;
	struct xkb_keymap *keymap;
};

struct kbd_dev {
	unsigned long ref;
	struct kbd_desc *desc;
	struct xkb_state *state;
};

int kbd_dev_new(struct kbd_dev **out, struct kbd_desc *desc)
{
	struct kbd_dev *kbd;

	kbd = malloc(sizeof(*kbd));
	if (!kbd)
		return -ENOMEM;

	memset(kbd, 0, sizeof(*kbd));
	kbd->ref = 1;
	kbd->desc = desc;

	kbd_desc_ref(desc);
	*out = kbd;
	return 0;
}

void kbd_dev_ref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref)
		return;

	++kbd->ref;
}

void kbd_dev_unref(struct kbd_dev *kbd)
{
	if (!kbd || !kbd->ref || --kbd->ref)
		return;

	kbd_desc_unref(kbd->desc);
	free(kbd);
}

#define EVDEV_KEYCODE_OFFSET 8

int kbd_dev_process_key(struct kbd_dev *kbd,
			uint16_t key_state,
			uint16_t code,
			struct uterm_input_event *out)
{
	struct xkb_state *state;
	xkb_keycode_t keycode;

	if (!kbd)
		return -EINVAL;

	state = kbd->state;
	keycode = code + EVDEV_KEYCODE_OFFSET;

	(void)keycode; (void)state;

	return -ENOKEY;
}

/*
 * Call this when we regain control of the keyboard after losing it.
 * We don't reset the locked group, this should survive a VT switch, etc. The
 * locked modifiers are reset according to the keyboard LEDs.
 */
void kbd_dev_reset(struct kbd_dev *kbd, const unsigned long *ledbits)
{
	unsigned int i;
	struct xkb_state *state;
	static const struct {
		int led;
		const char *indicator_name;
	} led_names[] = {
		{ LED_NUML, "Num Lock" },
		{ LED_CAPSL, "Caps Lock" },
		{ LED_SCROLLL, "Scroll Lock" },
		{ LED_COMPOSE, "Compose" },
	};

	if (!kbd)
		return;

	state = kbd->state;

	for (i = 0; i < sizeof(led_names) / sizeof(*led_names); i++) {
		if (!input_bit_is_set(ledbits, led_names[i].led))
			continue;
	}

	(void)state;
}

int kbd_desc_new(struct kbd_desc **out,
			const char *layout,
			const char *variant,
			const char *options)
{
	struct kbd_desc *desc;
	const struct xkb_rule_names rmlvo = {
		.rules = "evdev",
		.model = "evdev",
		.layout = layout,
		.variant = variant,
		.options = options,
	};

	if (!out)
		return -EINVAL;

	desc = malloc(sizeof(*desc));
	if (!desc)
		return -ENOMEM;

	memset(desc, 0, sizeof(*desc));
	desc->ref = 1;

	(void)rmlvo;
	desc->keymap = NULL;

	log_debug("new keyboard description (%s, %s, %s)",
			layout, variant, options);
	*out = desc;
	return 0;
}

void kbd_desc_ref(struct kbd_desc *desc)
{
	if (!desc || !desc->ref)
		return;

	++desc->ref;
}

void kbd_desc_unref(struct kbd_desc *desc)
{
	if (!desc || !desc->ref || --desc->ref)
		return;

	log_debug("destroying keyboard description");
	free(desc);
}

void kbd_keysym_to_string(uint32_t keysym, char *str, size_t size)
{
	xkb_keysym_get_name(keysym, str, size);
}
