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

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#define SHL_EXPORT __attribute__((visibility("default")))
#define SHL_HAS_BITS(_bitmask, _bits) (((_bitmask) & (_bits)) == (_bits))
#define SHL_DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define SHL_ULONG_BITS (sizeof(unsigned long) * 8)

static inline int shl_dirent(const char *path, struct dirent **ent)
{
	size_t len;
	struct dirent *tmp;

	len = offsetof(struct dirent, d_name) +
					pathconf(path, _PC_NAME_MAX) + 1;
	tmp = malloc(len);
	if (!tmp)
		return -ENOMEM;

	*ent = tmp;
	return 0;
}

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

static inline int shl_dup(void **out, const void *data, size_t size)
{
	void *cpy;

	if (!data || !size)
		return -EINVAL;

	cpy = malloc(size);
	if (!cpy)
		return -ENOMEM;

	memcpy(cpy, data, size);
	*out = cpy;
	return 0;
}

static inline bool shl_ends_with(const char *str, const char *suffix)
{
	size_t len, slen;

	len = strlen(str);
	slen = strlen(suffix);

	if (len < slen)
		return false;

	return !memcmp(str + len - slen, suffix, slen);
}

static inline unsigned long shl_next_pow2(unsigned long num)
{
	unsigned int i;

	if (!num)
		return 0;

	--num;
	for (i = 1; i < sizeof(unsigned long) * CHAR_BIT; i <<= 1)
		num = num | num >> i;

	return num + 1;
}

/* This parses \arg and splits the string into a new allocated array. The array
 * is stored in \out and is NULL terminated. Empty entries are removed from the
 * array if \keep_empty is false. \out_num is the number of entries in the
 * array. You can set it to NULL to not retrieve this value.
 * \sep is the separator character which must be a valid ASCII character,
 * otherwise this will not be UTF8 safe. */
static inline int shl_split_string(const char *arg, char ***out,
				   unsigned int *out_num, char sep,
				   bool keep_empty)
{
	unsigned int i;
	unsigned int num, len, size, pos;
	char **list, *off;

	if (!arg || !out || !sep)
		return -EINVAL;

	num = 0;
	size = 0;
	len = 0;
	for (i = 0; arg[i]; ++i) {
		if (arg[i] != sep) {
			++len;
			continue;
		}

		if (keep_empty || len) {
			++num;
			size += len + 1;
			len = 0;
		}
	}

	if (len > 0 || (keep_empty && (!i || arg[i - 1] == sep))) {
		++num;
		size += len + 1;
	}

	list = malloc(sizeof(char*) * (num + 1) + size);
	if (!list)
		return -ENOMEM;

	off = (void*)(((char*)list) + (sizeof(char*) * (num + 1)));
	i = 0;
	for (pos = 0; pos < num; ) {
		list[pos] = off;
		while (arg[i] && arg[i] != sep)
			*off++ = arg[i++];
		if (arg[i])
			++i;
		if (list[pos] == off && !keep_empty)
			continue;
		*off++ = 0;
		pos++;
	}
	list[pos] = NULL;

	*out = list;
	if (out_num)
		*out_num = num;
	return 0;
}

static inline int shl_dup_array_size(char ***out, char **argv, size_t len)
{
	char **t, *off;
	unsigned int size, i;

	if (!out || !argv)
		return -EINVAL;

	size = 0;
	for (i = 0; i < len; ++i) {
		++size;
		if (argv[i])
			size += strlen(argv[i]);
	}
	++i;

	size += i * sizeof(char*);

	t = malloc(size);
	if (!t)
		return -ENOMEM;
	*out = t;

	off = (char*)t + i * sizeof(char*);
	while (len--) {
		*t++ = off;
		for (i = 0; *argv && argv[0][i]; ++i)
			*off++ = argv[0][i];
		*off++ = 0;
		argv++;
	}
	*t = NULL;

	return 0;
}

static inline int shl_dup_array(char ***out, char **argv)
{
	unsigned int i;

	if (!out || !argv)
		return -EINVAL;

	for (i = 0; argv[i]; ++i)
		/* empty */ ;

	return shl_dup_array_size(out, argv, i);
}

/* returns true if the string-list contains only a single entry \entry */
static inline bool shl_string_list_is(char **list, const char *entry)
{
	if (!list || !entry)
		return false;
	if (!list[0] || list[1])
		return false;
	return !strcmp(list[0], entry);
}

static inline unsigned int shl_string_list_count(char **list, bool ignore_empty)
{
	unsigned int num;

	if (!list)
		return 0;

	for (num = 0; *list; ++list)
		if (**list || !ignore_empty)
			++num;

	return num;
}

/* reads a whole file into a buffer with 0-termination */
static inline int shl_read_file(const char *path, char **out, size_t *size)
{
	FILE *ffile;
	ssize_t len;
	char *buf;
	int ret;

	if (!path || !out)
		return -EINVAL;

	errno = 0;

	ffile = fopen(path, "rb");
	if (!ffile)
		return -errno;

	if (fseek(ffile, 0, SEEK_END) != 0) {
		ret = -errno;
		goto err_close;
	}

	len = ftell(ffile);
	if (len < 0) {
		ret = -errno;
		goto err_close;
	}

	rewind(ffile);

	buf = malloc(len + 1);
	if (!buf) {
		ret = -ENOMEM;
		goto err_close;
	}

	errno = 0;
	if (len && len != fread(buf, 1, len, ffile)) {
		ret = errno ? -errno : -EFAULT;
		goto err_free;
	}

	buf[len] = 0;
	*out = buf;
	if (size)
		*size = len;
	ret = 0;
	goto err_close;

err_free:
	free(buf);
err_close:
	fclose(ffile);
	return ret;
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
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		mods |= SHL_SHIFT_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		mods |= SHL_LOCK_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CTRL,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		mods |= SHL_CONTROL_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_ALT,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		mods |= SHL_ALT_MASK;
	if (xkb_state_mod_name_is_active(state, XKB_MOD_NAME_LOGO,
					 XKB_STATE_MODS_EFFECTIVE) > 0)
		mods |= SHL_LOGO_MASK;

	return mods;
}

static inline uint32_t shl_get_ascii(struct xkb_state *state, uint32_t keycode,
				     const uint32_t *keysyms,
				     unsigned int num_keysyms)
{
	struct xkb_keymap *keymap;
	xkb_layout_index_t num_layouts;
	xkb_layout_index_t layout;
	xkb_level_index_t level;
	const xkb_keysym_t *syms;
	int num_syms;

	if (num_keysyms == 1 && keysyms[0] < 128)
		return keysyms[0];

	keymap = xkb_state_get_keymap(state);
	num_layouts = xkb_keymap_num_layouts_for_key(keymap, keycode);

	for (layout = 0; layout < num_layouts; layout++) {
		level = xkb_state_key_get_level(state, keycode, layout);
		num_syms = xkb_keymap_key_get_syms_by_level(keymap, keycode,
							layout, level, &syms);
		if (num_syms != 1)
			continue;

		if (syms[0] < 128)
			return syms[0];
	}

	return XKB_KEY_NoSymbol;
}

static inline bool shl_grab_matches(unsigned int ev_mods,
				    unsigned int ev_num_syms,
				    const uint32_t *ev_syms,
				    unsigned int grab_mods,
				    unsigned int grab_num_syms,
				    const uint32_t *grab_syms)
{
	if (!SHL_HAS_BITS(ev_mods, grab_mods))
		return false;

	if (grab_num_syms != 0) {
		if (ev_num_syms != grab_num_syms)
			return false;
		if (memcmp(ev_syms, grab_syms, sizeof(uint32_t) * ev_num_syms))
			return false;
	}

	return true;
}

static inline bool shl_grab_has_match(unsigned int ev_mods,
				      unsigned int ev_num_syms,
				      const uint32_t *ev_syms,
				      unsigned int grab_num,
				      const unsigned int *grab_mods,
				      const unsigned int *grab_num_syms,
				      uint32_t **grab_syms)
{
	unsigned int i;

	for (i = 0; i < grab_num; ++i) {
		if (shl_grab_matches(ev_mods, ev_num_syms, ev_syms,
				     grab_mods[i], grab_num_syms[i],
				     grab_syms[i]))
			return true;
	}

	return false;
}

#endif /* SHL_MISC_H */
