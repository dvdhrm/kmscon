/*
 * kmscon - translating key presses to input events using libxkbcommon
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
 * This mostly involves things the X server does normally and libxkbcommon
 * doesn't provide us for free.
 * This implements a minimal subset of XKB, mostly ignoring stuff like:
 * - The protocol itself - we don't allow changing/querying and do everything
 *   at init.
 * - Everything to do with pointing devices, buttons..
 * - Indicators
 * - Controls
 * - Bells
 * - Dead keys
 * - All actions beside group- and modifier-related.
 * - Behaviours
 * - Geometries
 * - And various tweaks to what we do support.
 *
 * Some references to understand what's going on:
 *
 * [Lib] The X Keyboard Extension: Library Specification
 *	http://www.x.org/releases/current/doc/libX11/specs/XKB/xkblib.html
 * [Proto] The X Keyboard Extension: Protocol Specification
 *	http://www.x.org/releases/current/doc/kbproto/xkbproto.html
 * [xserver] The X server source code dealing with xkb
 *	<xserver source root>/xkb/
 * [xlib] The Xlib source code dealing with xkb and input methods
 *	<libX11 source root>/xkb/
 *	<libX11 source root>/modules/im/ximcp/
 * [Pascal] Some XKB documentation by its maintainer (not the best english)
 *	http://pascal.tsu.ru/en/xkb/
 * [Headers] Some XKB-related headers
 *	/usr/include/X11/extensions/XKBcommon.h
 *	/usr/include/X11/extensions/XKB.h
 *	/usr/include/X11/keysymdef.h
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <linux/input.h>
#include <xkbcommon/xkbcommon.h>

#include "kbd.h"
#include "log.h"
#include "imKStoUCS.h"

#define LOG_SUBSYSTEM "kbd_xkb"

struct kmscon_kbd_desc {
	unsigned long ref;

	struct xkb_desc *desc;
};

struct kmscon_kbd {
	unsigned long ref;
	struct kmscon_kbd_desc *desc;

	struct xkb_state state;
};

int kmscon_kbd_new(struct kmscon_kbd **out, struct kmscon_kbd_desc *desc)
{
	struct kmscon_kbd *kbd;

	kbd = malloc(sizeof(*kbd));
	if (!kbd)
		return -ENOMEM;

	memset(kbd, 0, sizeof(*kbd));
	kbd->ref = 1;

	kbd->desc = desc;
	kmscon_kbd_desc_ref(desc);

	*out = kbd;
	return 0;
}

void kmscon_kbd_ref(struct kmscon_kbd *kbd)
{
	if (!kbd)
		return;

	++kbd->ref;
}

void kmscon_kbd_unref(struct kmscon_kbd *kbd)
{
	if (!kbd || !kbd->ref)
		return;

	if (--kbd->ref)
		return;

	kmscon_kbd_desc_unref(kbd->desc);
	free(kbd);
}

static uint8_t virtual_to_real_mods(struct xkb_desc *desc, uint16_t vmods)
{
	int i;
	uint32_t bit;
	uint8_t mods;

	mods = 0x00;

	for (i=0, bit=0x01; i < XkbNumVirtualMods; i++, bit<<=1)
		if (vmods & bit)
			mods |= desc->server->vmods[i];

	return mods;
}

static uint8_t virtual_and_real_to_mask(struct xkb_desc *desc,
					uint16_t vmods, uint8_t real_mods)
{
	uint8_t mods = 0x00;

	mods |= real_mods;
	mods |= virtual_to_real_mods(desc, vmods);

	return mods;
}

/*
 * Helper function for the wrap_group_* functions.
 * See [Lib] 11.7.1 for the rules.
 */
static uint8_t wrap_group(int16_t group, int num_groups, uint8_t group_info)
{
	/* No need for wrapping. */
	if (XkbIsLegalGroup(group) && group < num_groups)
		return group;

	switch (XkbOutOfRangeGroupAction(group_info)) {
	case XkbWrapIntoRange:
		/*
		 * C99 says a negative dividend in a modulo operation
		 * will always give a negative result.
		 */
		if (group < 0)
			return num_groups + (group % num_groups);
		else
			return group % num_groups;

	case XkbClampIntoRange:
		/* This one seems to be unused. */
		return num_groups - 1;

	case XkbRedirectIntoRange:
		/* This one seems to be unused. */
		group = XkbOutOfRangeGroupNumber(group_info);
		/* If it's _still_ out of range, use the first group. */
		if (group >= num_groups)
			return 0;
	}

	return 0;
}

/*
 * Wrap an arbitrary group into a legal effective global group according to
 * the GroupsWrap control.
 * (Group actions mostly act on the group number in a relative manner [e.g.
 * +1, -1]. So if we have N groups, the effective group is N-1, and we get a
 * SetGroup +1, this tells us what to do.)
 */
static uint8_t wrap_group_control(struct xkb_desc *desc, int16_t group)
{
	int num_groups;
	uint8_t group_info;

	num_groups = desc->ctrls->num_groups;
	group_info = desc->ctrls->groups_wrap;

	return wrap_group(group, num_groups, group_info);
}

/*
 * Wrap the effective global group to a legal group for the keycode, according
 * to the rule specified for the key.
 * (Some keycodes may have more groups than others, and so the effective
 * group may not make sense for a certain keycode).
 */
static uint8_t wrap_group_keycode(struct xkb_desc *desc, xkb_keycode_t keycode,
								int16_t group)
{
	int num_groups;
	uint8_t group_info;

	num_groups = XkbKeyNumGroups(desc, keycode);
	group_info = XkbKeyGroupInfo(desc, keycode);

	return wrap_group(group, num_groups, group_info);
}

/*
 * Need to update the effective mods after any changes to the base, latched or
 * locked mods.
 */
static void update_effective_mods(struct xkb_desc *desc,
						struct xkb_state *state)
{
	state->mods = state->base_mods | state->latched_mods |
							state->locked_mods;
}

/*
 * Need to update the effective group after any changes to the base, latched or
 * locked group.
 */
static void update_effective_group(struct xkb_desc *desc,
						struct xkb_state *state)
{
	int16_t group;

	/* Update the effective group. */
	group = state->base_group + state->locked_group + state->latched_group;
	state->group = wrap_group_control(desc, group);
}

/*
 * Updates the group state.
 * See [Lib] Table 17.4 for logic.
 */
static bool process_group_action(struct xkb_desc *desc, struct xkb_state *state,
			xkb_keycode_t keycode, enum kmscon_key_state key_state,
					struct xkb_group_action *action)
{
	int16_t group = action->group;
	uint8_t flags = action->flags;

	/*
	 * action->group is signed and may be negative if GroupAbsolute
	 * is not set. A group itself cannot be negative and is unsigend.
	 * Therefore we extend these to int16 to avoid underflow and
	 * signedness issues. Be careful!
	 */
	int16_t base_group = state->base_group;
	int16_t latched_group = state->latched_group;
	int16_t locked_group = state->locked_group;

	/*
	 * FIXME: Some actions here should be conditioned "and no keys are
	 * physically depressed when this key is released".
	 */

	switch (action->type) {
	case XkbSA_SetGroup:
		if (key_state == KMSCON_KEY_PRESSED) {
			if (flags & XkbSA_GroupAbsolute)
				base_group = group;
			else
				base_group += group;
		} else if (key_state == KMSCON_KEY_RELEASED) {
			if (flags & XkbSA_ClearLocks)
				locked_group = 0;
		}

		break;
	case XkbSA_LatchGroup:
		if (key_state == KMSCON_KEY_PRESSED) {
			if (flags & XkbSA_GroupAbsolute)
				base_group = group;
			else
				base_group += group;
		} else if (key_state == KMSCON_KEY_RELEASED) {
			if ((flags & XkbSA_LatchToLock) && latched_group) {
				locked_group += group;
				latched_group -= group;
			} else {
				latched_group += group;
			}
		}

		break;
	case XkbSA_LockGroup:
		if (key_state == KMSCON_KEY_PRESSED) {
			if (flags & XkbSA_GroupAbsolute)
				locked_group = group;
			else
				locked_group += group;
		}

		break;
	}

	/* Bring what was changed back into range. */
	state->base_group = wrap_group_control(desc, base_group);
	state->locked_group = wrap_group_control(desc, locked_group);
	state->latched_group = wrap_group_control(desc, latched_group);
	update_effective_group(desc, state);
	return true;
}

/*
 * Updates the modifiers state.
 * See [Lib] Table 17.1 for logic.
 * */
static bool process_mod_action(struct xkb_desc *desc, struct xkb_state *state,
			xkb_keycode_t keycode, enum kmscon_key_state key_state,
						struct xkb_mod_action *action)
{
	uint8_t mods;
	uint8_t saved_mods;
	uint8_t flags = action->flags;

	if (flags & XkbSA_UseModMapMods)
		mods = desc->map->modmap[keycode];
	else
		mods = action->mask;

	/*
	 * FIXME: Some actions here should be conditioned "and no keys are
	 * physically depressed when this key is released".
	 */

	switch (action->type) {
	case XkbSA_SetMods:
		if (key_state == KMSCON_KEY_PRESSED) {
			state->base_mods |= mods;
		} else if (key_state == KMSCON_KEY_RELEASED) {
			state->base_mods &= ~mods;
			if (flags & XkbSA_ClearLocks)
				state->locked_mods &= ~mods;
		}

		break;
	case XkbSA_LatchMods:
		if (key_state == KMSCON_KEY_PRESSED) {
			state->base_mods |= mods;
		} else if (key_state == KMSCON_KEY_RELEASED) {
			if (flags & XkbSA_ClearLocks) {
				saved_mods = state->locked_mods;
				state->locked_mods &= ~mods;
				mods &= ~(mods & saved_mods);
			}
			if (flags & XkbSA_LatchToLock) {
				saved_mods = mods;
				mods = (mods & state->latched_mods);
				state->locked_mods |= mods;
				state->latched_mods &= ~mods;
				mods = saved_mods & (~mods);
			}
			state->latched_mods |= mods;
		}

		break;
	case XkbSA_LockMods:
		/* We fake a little here and toggle both on and off on keypress. */
		if (key_state == KMSCON_KEY_PRESSED) {
			state->base_mods |= mods;
			state->locked_mods ^= mods;
		} else if (key_state == KMSCON_KEY_RELEASED) {
			state->base_mods &= ~mods;
		}

		break;
	}

	update_effective_mods(desc, state);
	return true;
}

/*
 * An action dispatcher. The return value indicates whether the keyboard state
 * was changed.
 */
static bool process_action(struct xkb_desc *desc, struct xkb_state *state,
			xkb_keycode_t keycode, enum kmscon_key_state key_state,
						union xkb_action *action)
{
	if (!action)
		return false;

	switch (action->type) {
	case XkbSA_NoAction:
		break;
	case XkbSA_SetMods:
	case XkbSA_LatchMods:
	case XkbSA_LockMods:
		return process_mod_action(desc, state, keycode, key_state,
							&action->mods);
		break;
	case XkbSA_SetGroup:
	case XkbSA_LatchGroup:
	case XkbSA_LockGroup:
		return process_group_action(desc, state, keycode, key_state,
							&action->group);
		break;
	default:
		/*
		 * Don't handle other actions.
		 * Note: There may be useful stuff here, like TerminateServer
		 * or SwitchScreen.
		 */
		break;
	}

	return false;
}

/*
 * The shift level to use for the keycode (together with the group) is
 * determined by the modifier state. There are various "types" of ways to use
 * the modifiers to shift the keycode; this is determined by the key_type
 * object mapped to the (keycode, group) pair.
 */
static uint16_t find_shift_level(struct xkb_desc *desc, xkb_keycode_t keycode,
						uint8_t mods, uint8_t group)
{
	int i;
	struct xkb_key_type *type;
	struct xkb_kt_map_entry *entry;
	uint8_t masked_mods;

	type = XkbKeyType(desc, keycode, group);

	masked_mods = type->mods.mask & mods;

	for (i=0; i < type->map_count; i++) {
		entry = &type->map[i];

		if (!entry->active)
			continue;

		/*
		 * Must match exactly after we masked it with the key_type's
		 * mask.
		 */
		if (entry->mods.mask == masked_mods)
			return entry->level;
	}

	/* The default is LevelOne. */
	return 0;
}

/* Whether to send out a repeat event for the key. */
static bool should_key_repeat(struct xkb_desc *desc, xkb_keycode_t keycode)
{
	unsigned const char *pkr;

	/* Repeats globally disabled. */
	if (!(desc->ctrls->enabled_ctrls & XkbRepeatKeysMask))
		return false;

	/* Repeats disabled for the specific key. */
	pkr = desc->ctrls->per_key_repeat;
	if (!(pkr[keycode / 8] & (0x01 << (keycode % 8))))
		return false;

	/* Don't repeat modifiers. */
	if (desc->map->modmap[keycode] != 0)
		return false;

	return true;
}

int kmscon_kbd_process_key(struct kmscon_kbd *kbd,
					enum kmscon_key_state key_state,
					uint16_t code,
					struct kmscon_input_event *out)
{
	struct xkb_desc *desc;
	struct xkb_state *state;
	xkb_keycode_t keycode;
	uint8_t group;
	uint16_t shift_level;
	uint32_t sym;
	union xkb_action *action;
	bool state_changed, event_filled;

	if (!kbd)
		return -EINVAL;

	desc = kbd->desc->desc;
	state = &kbd->state;

	keycode = code + desc->min_key_code;

	/* Valid keycode. */
	if (!XkbKeycodeInRange(desc, keycode))
		return -ENOKEY;
	/* Active keycode. */
	if (XkbKeyNumSyms(desc, keycode) == 0)
		return -ENOKEY;
	/* Unwanted repeat. */
	if (key_state == KMSCON_KEY_REPEATED &&
					!should_key_repeat(desc, keycode))
		return -ENOKEY;

	group = wrap_group_keycode(desc, keycode, state->group);
	shift_level = find_shift_level(desc, keycode, state->mods, group);
	sym = XkbKeySymEntry(desc, keycode, shift_level, group);

	state_changed = false;
	if (key_state != KMSCON_KEY_REPEATED) {
		action = XkbKeyActionEntry(desc, keycode, shift_level, group);
		state_changed = process_action(desc, state, keycode,
							key_state, action);
	}

	event_filled = false;
	if (key_state != KMSCON_KEY_RELEASED && !state_changed) {
		out->keycode = code;
		out->keysym = sym;
		/* 1-to-1 match - this might change. */
		out->mods = state->mods;
		out->unicode = KeysymToUcs4(sym);

		if (out->unicode == 0)
			out->unicode = KMSCON_INPUT_INVALID;

		event_filled = true;
	}

	if (state_changed) {
		/* Release latches. */
		state->latched_mods = 0;
		update_effective_mods(desc, state);
		state->latched_group = 0;
		update_effective_group(desc, state);
	}

	return event_filled ? 0 : -ENOKEY;
}

static struct xkb_indicator_map *find_indicator_map(struct xkb_desc *desc,
						const char *indicator_name)
{
	int i;

	for (i=0; i < XkbNumIndicators; i++)
		if (!strcmp(desc->names->indicators[i], indicator_name))
			return &desc->indicators->maps[i];

	return NULL;
}

/*
 * Call this when we regain control of the keyboard after losing it.
 * We don't reset the locked group, this should survive a VT switch, etc. The
 * locked modifiers are reset according to the keyboard LEDs.
 */
void kmscon_kbd_reset(struct kmscon_kbd *kbd, const unsigned long *ledbits)
{
	int i;
	struct xkb_desc *desc;
	struct xkb_state *state;
	struct xkb_indicator_map *im;
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

	desc = kbd->desc->desc;
	state = &kbd->state;

	state->group = 0;
	state->base_group = 0;
	state->latched_group = 0;

	state->mods = 0;
	state->base_mods = 0;
	state->latched_mods = 0;
	state->locked_mods = 0;

	for (i = 0; i < sizeof(led_names) / sizeof(*led_names); i++) {
		if (!kmscon_evdev_bit_is_set(ledbits, led_names[i].led))
			continue;

		im = find_indicator_map(desc, led_names[i].indicator_name);

		/* Only locked modifiers really matter here. */
		if (im && im->which_mods == XkbIM_UseLocked)
			state->locked_mods |= im->mods.mask;
	}

	update_effective_mods(desc, state);
	update_effective_group(desc, state);
}

/*
 * We don't do soft repeat currently, but we use the controls to filter out
 * which evdev repeats to send.
 */
static void init_autorepeat(struct xkb_desc *desc)
{
	/*
	 * This is taken from <xserver>/include/site.h
	 * If a bit is off for a keycode, it should not repeat.
	 */
	static const char DEFAULT_AUTOREPEATS[XkbPerKeyBitArraySize] = {
		0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	memcpy(desc->ctrls->per_key_repeat,
				DEFAULT_AUTOREPEATS, XkbPerKeyBitArraySize);

	desc->ctrls->enabled_ctrls |= XkbRepeatKeysMask;
}

/*
 * Update to the effective modifier mask of the indicator objects. We use them
 * to dicover which modifiers to match with which leds.
 */
static void init_indicators(struct xkb_desc *desc)
{
	int i;
	struct xkb_indicator_map *im;
	struct xkb_mods *mods;

	for (i=0; i < XkbNumIndicators; i++) {
		im = &desc->indicators->maps[i];
		mods = &im->mods;

		mods->mask = virtual_and_real_to_mask(desc, mods->vmods,
								mods->real_mods);
	}
}

static void init_action(struct xkb_desc *desc, union xkb_action *action)
{
	struct xkb_mod_action *mod_act;

	switch (action->type) {
	case XkbSA_SetMods:
	case XkbSA_LatchMods:
	case XkbSA_LockMods:
		mod_act = &action->mods;

		mod_act->mask = virtual_and_real_to_mask(desc, mod_act->vmods,
							mod_act->real_mods);
		break;
	}
}

/*
 * Update the effective modifer mask of the various action objects after we
 * initialized the virtual modifiers from compat. The only actions we change
 * here are the mod_action types.
 */
static void init_actions(struct xkb_desc *desc)
{
	int i;
	union xkb_action *action;
	struct xkb_sym_interpret *si;

	for (i=0; i < desc->server->num_acts; i++) {
		action = &desc->server->acts[i];
		init_action(desc, action);
	}

	for (i=0; i < desc->compat->num_si; i++) {
		si = &desc->compat->sym_interpret[i];
		action = (union xkb_action *)&si->act;
		init_action(desc, action);
	}
}

/*
 * After we figured out the virtual mods from the compat component, we update
 * the effective modifiers in the key_types component accordingly, because we
 * use it extensively to find the correct shift level.
 */
static void init_key_types(struct xkb_desc *desc)
{
	int i, j;
	struct xkb_key_type *type;
	struct xkb_kt_map_entry *entry;
	struct xkb_mods *mods;

	for (i=0; i < desc->map->num_types; i++) {
		type = &desc->map->types[i];
		mods = &type->mods;

		mods->mask = virtual_and_real_to_mask(desc, mods->vmods,
							mods->real_mods);

		for (j=0; j < type->map_count; j++) {
			entry = &type->map[j];
			mods = &entry->mods;

			mods->mask = virtual_and_real_to_mask(desc,
						mods->vmods, mods->real_mods);

			/*
			 * If the entry's vmods are bound to something, it
			 * should be active.
			 */
			if (virtual_to_real_mods(desc, mods->vmods))
				entry->active = true;
		}
	}
}

/*
 * Check a sym interpret match condition.
 * See [Lib] Table 18.1 for the logic.
 */
static bool are_modifiers_matching(uint8_t mods, unsigned char match,
							uint8_t to_mods)
{
	switch (match & XkbSI_OpMask) {
	case XkbSI_NoneOf:
		return (mods & to_mods) == 0;
	case XkbSI_AnyOfOrNone:
		return true;
	case XkbSI_AnyOf:
		return (mods & to_mods) != 0;
	case XkbSI_AllOf:
		return (mods & to_mods) == mods;
	case XkbSI_Exactly:
		return mods == to_mods;
	}

	return false;
}

/*
 * Look for the most specific symbol interpretation for the keysym.
 * See [xserver] XKBMisc.c:_XkbFindMatchingInterp() for the equivalent.
 */
static struct xkb_sym_interpret *find_sym_interpret(struct xkb_desc *desc,
			uint32_t sym, uint16_t level, uint8_t key_modmap)
{
	int i;
	struct xkb_sym_interpret *si;
	struct xkb_sym_interpret *all_syms_si;

	all_syms_si = NULL;

	/*
	 * If we find a matching interpret specific to our symbol, we return
	 * it immediatly.
	 * If we didn't find any, we return the first matching all-catching
	 * interpret.
	 */

	for (i=0; i < desc->compat->num_si; i++) {
		si = &desc->compat->sym_interpret[i];

		if (si->sym != sym && si->sym != 0)
			continue;

		/*
		 * If the interpret specified UseModMapMods=level1, the sym
		 * must be in the first level of its group.
		 * Note: [xserver] and [Lib] do different things here, and it
		 * doesn't seem to matter much. So it's commented for now.
		 */
		/* if (si->match&XkbSI_LevelOneOnly && level != 0) */
		/* 	continue; */

		if (!are_modifiers_matching(si->mods, si->match, key_modmap))
			continue;

		if (si->sym != 0)
			return si;
		else if (all_syms_si == NULL)
			all_syms_si = si;
	}

	return all_syms_si;
}

/*
 * Allocate slots for a keycode in the key-action mapping array. xkbcommon
 * doesn't do this by itself for actions from compat (that is, almost all of
 * them).
 * See [xserver] XKBMAlloc.c:XkbResizeKeyActions() for the equivalent.
 */
static int allocate_key_acts(struct xkb_desc *desc, uint8_t keycode)
{
	struct xkb_server_map *server;
	int sym_count;
	unsigned short index, new_size_acts;
	union xkb_action *acts;

	server = desc->server;
	sym_count = XkbKeyNumSyms(desc, keycode);

	if (XkbKeyHasActions(desc, keycode))
		return 0;

	index = server->num_acts;

	/* num_acts is the occupied slots, size_acts is the capacity. */
	if (server->num_acts + sym_count > server->size_acts) {
		/*
		 * Don't have enough space, need to allocate. We add some
		 * extra to avoid repeated reallocs.
		 */
		new_size_acts = server->num_acts + sym_count + 8;
		acts = realloc(server->acts, new_size_acts * sizeof (*acts));
		if (!acts)
			return -ENOMEM;
		server->acts = acts;
		server->size_acts = new_size_acts;
	}

	/* XkbSA_NoAction is 0x00 so we're good. */
	memset(&server->acts[index], 0, sym_count * sizeof(*server->acts));
	server->key_acts[keycode] = index;
	server->num_acts += sym_count;

	return 0;
}

static int init_compat_for_keysym(struct xkb_desc *desc, xkb_keycode_t keycode,
						uint8_t group, uint16_t level)
{
	int ret;
	uint8_t key_modmap;
	uint32_t sym;
	struct xkb_sym_interpret *si;
	union xkb_action *action;

	key_modmap = desc->map->modmap[keycode];
	sym = XkbKeySymEntry(desc, keycode, level, group);
	si = find_sym_interpret(desc, sym, level, key_modmap);

	if (!si)
		return 0;

	/* Set the key action mapping. */
	if (si->act.type != XkbSA_NoAction) {
		ret = allocate_key_acts(desc, keycode);
		if (ret)
			return ret;

		action = XkbKeyActionEntry(desc, keycode, level, group);
		*action = (union xkb_action)si->act;
	}

	/* Set the key virtual modifier mapping. */
	if (si->virtual_mod != XkbNoModifier)
		desc->server->vmodmap[keycode] |= 0x01 << si->virtual_mod;

	return 0;
}

static int init_compat_for_keycode(struct xkb_desc *desc, xkb_keycode_t keycode)
{
	int ret;
	int i, bit;

	uint8_t group;
	uint16_t level;
	int num_groups;
	int num_levels;

	/*
	 * It's possible that someone had set some actions for the keycode
	 * through the symbols file, and so we shouldn't override with the
	 * compat. This is very uncommon though, only used by the breaks_caps
	 * option here.
	 */
	if (XkbKeyHasActions(desc, keycode))
		return 0;

	num_groups = XkbKeyNumGroups(desc, keycode);

	/*
	 * We need to track the sym level in order to support LevelOneOnly,
	 * which is used in some symbol interpretations.
	 */

	for (group=0, i=0; group < num_groups; group++) {
		num_levels = XkbKeyGroupWidth(desc, keycode, group);

		for (level=0; level < num_levels; level++) {
			ret = init_compat_for_keysym(desc, keycode,
								group, level);
			if (ret)
				return ret;
		}
	}

	/*
	 * Translate the virtual modifiers bound to this key to the real
	 * modifiers bound to this key.
	 * See [Lib] 17.4 for vmodmap and friends.
	 */
	for (i=0, bit=0x01; i < XkbNumVirtualMods; i++, bit<<=1)
		if (bit&desc->server->vmodmap[keycode])
			desc->server->vmods[i] |= desc->map->modmap[keycode];

	return 0;
}

/*
 * This mostly fills out the keycode-action mapping and puts the virtual
 * modifier mappings in the right place.
 */
static void init_compat(struct xkb_desc *desc)
{
	/* If we use KeyCode it overflows. */
	unsigned int keycode;

	for (keycode = desc->min_key_code; keycode <= desc->max_key_code; keycode++)
		init_compat_for_keycode(desc, keycode);
}

/*
 * Create a ready-to-use xkb description object. It is used in most places
 * having to do with XKB.
 */
int kmscon_kbd_desc_new(struct kmscon_kbd_desc **out, const char *layout,
				const char *variant, const char *options)
{
	struct kmscon_kbd_desc *desc;
	struct xkb_rule_names rmlvo = {
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

	desc->desc = xkb_compile_keymap_from_rules(&rmlvo);
	if (!desc->desc) {
		log_err("cannot compile keymap from rules");
		free(desc);
		return -EFAULT;
	}

	/* The order of these is important! */
	init_compat(desc->desc);
	init_key_types(desc->desc);
	init_actions(desc->desc);
	init_indicators(desc->desc);
	init_autorepeat(desc->desc);

	log_debug("new keyboard description (%s, %s, %s)",
						layout, variant, options);
	*out = desc;
	return 0;
}

void kmscon_kbd_desc_ref(struct kmscon_kbd_desc *desc)
{
	if (!desc)
		return;

	++desc->ref;
}

void kmscon_kbd_desc_unref(struct kmscon_kbd_desc *desc)
{
	if (!desc || !desc->ref)
		return;

	if (--desc->ref)
		return;

	log_debug("destroying keyboard description");
	xkb_free_keymap(desc->desc);
	free(desc);
}

void kmscon_kbd_keysym_to_string(uint32_t keysym, char *str, size_t size)
{
	xkb_keysym_to_string(keysym, str, size);
}
