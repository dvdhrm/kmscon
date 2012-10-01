/*
 * shl - Miscellaneous small helpers
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
 * Miscellaneous helpers
 */

#ifndef SHL_MISC_H
#define SHL_MISC_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

#define SHL_HAS_BITS(_bitmask, _bits) (((_bitmask) & (_bits)) == (_bits))

static inline int shl_strtou(const char *input, unsigned int *output)
{
	unsigned long val;
	unsigned int res;
	char *tmp = NULL;

	if (!input || !*input)
		return -EINVAL;

	errno = 0;
	val = strtoul(input, &tmp, 0);

	res = val;
	if (!tmp || *tmp || errno || (unsigned long)res != val)
		return -EINVAL;

	if (output)
		*output = res;
	return 0;
}

/* TODO: xkbcommon should provide these flags!
 * We currently copy them into each library API we use so we need  to keep
 * them in sync. Currently, they're used in uterm-input and tsm-vte. */
enum shl_xkb_mods {
	SHL_SHIFT_MASK		= (1 << 0),
	SHL_LOCK_MASK		= (1 << 1),
	SHL_CONTROL_MASK	= (1 << 2),
	SHL_ALT_MASK		= (1 << 3),
	SHL_LOGO_MASK		= (1 << 4),
};

static inline unsigned int shl_get_xkb_mods(struct xkb_state *state)
{
	unsigned int mods = 0;

	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT,
					 XKB_STATE_EFFECTIVE))
		mods |= SHL_SHIFT_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS,
					 XKB_STATE_EFFECTIVE))
		mods |= SHL_LOCK_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,
					 XKB_STATE_EFFECTIVE))
		mods |= SHL_CONTROL_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT,
					 XKB_STATE_EFFECTIVE))
		mods |= SHL_ALT_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO,
					 XKB_STATE_EFFECTIVE))
		mods |= SHL_LOGO_MASK;

	return mods;
}

#endif /* SHL_MISC_H */
