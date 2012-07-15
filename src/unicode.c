/*
 * kmscon - Unicode Handling
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011-2012 University of Tuebingen
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
 * This kmscon-utf8-state-machine is based on the wayland-compositor demos:
 *
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Unicode Handling
 * Main implementation of the symbol datatype. The symbol table contains two-way
 * references. The Hash Table contains all the symbols with the symbol ucs4
 * string as key and the symbol ID as value.
 * The index array contains the symbol ID as key and a pointer to the ucs4
 * string as value. But the hash table owns the ucs4 string.
 * This allows fast implementations of *_get() and *_append() without long
 * search intervals.
 *
 * When creating a new symbol, we simply return the UCS4 value as new symbol. We
 * do not add it to our symbol table as it is only one character. However, if a
 * character is appended to an existing symbol, we create a new ucs4 string and
 * push the new symbol into the symbol table.
 */

/* TODO: Remove the glib dependencies */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "static_misc.h"
#include "unicode.h"

#define LOG_SUBSYSTEM "unicode"

#define KMSCON_UCS4_MAXLEN 10
#define KMSCON_UCS4_MAX 0x7fffffffUL
#define KMSCON_UCS4_INVALID 0xfffd

const kmscon_symbol_t kmscon_symbol_default = 0;
static const char default_u8[] = { 0 };

static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t table_next_id;
static struct kmscon_array *table_index;
static struct kmscon_hashtable *table_symbols;

static unsigned int hash_ucs4(const void *key)
{
	unsigned int val = 5381;
	size_t i;
	const uint32_t *ucs4 = key;

	i = 0;
	while (ucs4[i] <= KMSCON_UCS4_MAX) {
		val = val * 33 + ucs4[i];
		++i;
	}

	return val;
}

static bool cmp_ucs4(const void *a, const void *b)
{
	size_t i;
	const uint32_t *v1, *v2;

	v1 = a;
	v2 = b;
	i = 0;

	while (1) {
		if (v1[i] > KMSCON_UCS4_MAX && v2[i] > KMSCON_UCS4_MAX)
			return true;
		if (v1[i] > KMSCON_UCS4_MAX && v2[i] <= KMSCON_UCS4_MAX)
			return false;
		if (v1[i] <= KMSCON_UCS4_MAX && v2[i] > KMSCON_UCS4_MAX)
			return false;
		if (v1[i] != v2[i])
			return false;

		++i;
	}
}

static void table_lock()
{
	pthread_mutex_lock(&table_mutex);
}

static void table_unlock()
{
	pthread_mutex_unlock(&table_mutex);
}

static int table__init()
{
	static const uint32_t *val = NULL; /* we need a valid lvalue */
	int ret;

	if (table_symbols)
		return 0;

	table_next_id = KMSCON_UCS4_MAX + 2;

	ret = kmscon_array_new(&table_index, sizeof(uint32_t*), 4);
	if (ret) {
		log_err("cannot allocate table-index");
		return ret;
	}

	/* first entry is not used so add dummy */
	kmscon_array_push(table_index, &val);

	ret = kmscon_hashtable_new(&table_symbols, hash_ucs4, cmp_ucs4,
					free, NULL);
	if (ret) {
		kmscon_array_free(table_index);
		return -ENOMEM;
	}

	return 0;
}

kmscon_symbol_t kmscon_symbol_make(uint32_t ucs4)
{
	if (ucs4 > KMSCON_UCS4_MAX) {
		log_warn("invalid ucs4 character");
		return 0;
	} else {
		return ucs4;
	}
}

/*
 * This decomposes a symbol into a ucs4 string and a size value. If \sym is a
 * valid UCS4 character, this returns a pointer to \sym and writes 1 into \size.
 * Therefore, the returned value may get destroyed if your \sym argument gets
 * destroyed.
 * If \sym is a composed ucs4 string, then the returned value points into the
 * hash table of the symbol table and lives as long as the symbol table does.
 *
 * This always returns a valid value. If an error happens, the default character
 * is returned. If \size is NULL, then the size value is omitted.
 */
static const uint32_t *table__get(kmscon_symbol_t *sym, size_t *size)
{
	uint32_t *ucs4;

	if (*sym <= KMSCON_UCS4_MAX) {
		if (size)
			*size = 1;
		return sym;
	}

	if (table__init()) {
		if (size)
			*size = 1;
		return &kmscon_symbol_default;
	}

	ucs4 = *KMSCON_ARRAY_AT(table_index, uint32_t*,
				*sym - (KMSCON_UCS4_MAX + 1));
	if (!ucs4) {
		if (size)
			*size = 1;
		return &kmscon_symbol_default;
	}

	if (size) {
		*size = 0;
		while (ucs4[*size] <= KMSCON_UCS4_MAX)
			++*size;
	}

	return ucs4;
}

const uint32_t *kmscon_symbol_get(kmscon_symbol_t *sym, size_t *size)
{
	const uint32_t *res;

	table_lock();
	res = table__get(sym, size);
	table_unlock();

	return res;
}

kmscon_symbol_t kmscon_symbol_append(kmscon_symbol_t sym, uint32_t ucs4)
{
	uint32_t buf[KMSCON_UCS4_MAXLEN + 1], nsym, *nval;
	const uint32_t *ptr;
	size_t s;
	kmscon_symbol_t rsym;
	void *tmp;
	bool res;

	table_lock();

	if (table__init()) {
		rsym = sym;
		goto unlock;
	}

	if (ucs4 > KMSCON_UCS4_MAX) {
		log_warn("invalid ucs4 character");
		rsym = sym;
		goto unlock;
	}

	ptr = table__get(&sym, &s);
	if (s >= KMSCON_UCS4_MAXLEN) {
		rsym = sym;
		goto unlock;
	}

	memcpy(buf, ptr, s * sizeof(uint32_t));
	buf[s++] = ucs4;
	buf[s++] = KMSCON_UCS4_MAX + 1;

	res = kmscon_hashtable_find(table_symbols, &tmp, buf);
	if (res) {
		rsym = (uint32_t)(long)tmp;
		goto unlock;
	}

	log_debug("adding new composed symbol");

	nval = malloc(sizeof(uint32_t) * s);
	if (!nval) {
		rsym = sym;
		goto unlock;
	}

	memcpy(nval, buf, s * sizeof(uint32_t));
	nsym = table_next_id++;
	kmscon_hashtable_insert(table_symbols, nval, (void*)(long)nsym);
	kmscon_array_push(table_index, &nval);
	rsym = nsym;

unlock:
	table_unlock();
	return rsym;
}

/*
 * Convert UCS4 character to UTF-8. This creates one of:
 *   0xxxxxxx
 *   110xxxxx 10xxxxxx
 *   1110xxxx 10xxxxxx 10xxxxxx
 *   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
 * This is based on the same function from "terminology" from the Enlightenment
 * project. See COPYING for more information.
 */
static size_t ucs4_to_utf8(uint32_t g, char *txt)
{
	if (g < (1 << 7)) {
		txt[0] = g & 0x7f;
		return 1;
	} else if (g < (1 << (5 + 6))) {
		txt[0] = 0xc0 | ((g >> 6) & 0x1f);
		txt[1] = 0x80 | ((g     ) & 0x3f);
		return 2;
	} else if (g < (1 << (4 + 6 + 6))) {
		txt[0] = 0xe0 | ((g >> 12) & 0x0f);
		txt[1] = 0x80 | ((g >>  6) & 0x3f);
		txt[2] = 0x80 | ((g      ) & 0x3f);
		return 3;
	} else if (g < (1 << (3 + 6 + 6 + 6))) {
		txt[0] = 0xf0 | ((g >> 18) & 0x07);
		txt[1] = 0x80 | ((g >> 12) & 0x3f);
		txt[2] = 0x80 | ((g >>  6) & 0x3f);
		txt[3] = 0x80 | ((g      ) & 0x3f);
		return 4;
	} else {
		return 0;
	}
}

const char *kmscon_symbol_get_u8(kmscon_symbol_t sym, size_t *size)
{
	const uint32_t *ucs4;
	char *val;
	size_t i, pos, len;

	ucs4 = kmscon_symbol_get(&sym, &len);
	val = malloc(4 * len);
	if (!val)
		goto err_out;

	pos = 0;
	for (i = 0; i < len; ++i)
		pos += ucs4_to_utf8(ucs4[i], &val[pos]);

	if (!pos)
		goto err_out;

	if (size)
		*size = pos;
	return val;

err_out:
	if (size)
		*size = sizeof(default_u8);
	return default_u8;
}

void kmscon_symbol_free_u8(const char *s)
{
	if (s != default_u8)
		free((void*)s);
}

struct kmscon_utf8_mach {
	int state;
	uint32_t ch;
};

int kmscon_utf8_mach_new(struct kmscon_utf8_mach **out)
{
	struct kmscon_utf8_mach *mach;

	if (!out)
		return -EINVAL;

	mach = malloc(sizeof(*mach));
	if (!mach)
		return -ENOMEM;

	memset(mach, 0, sizeof(*mach));
	mach->state = KMSCON_UTF8_START;

	*out = mach;
	return 0;
}

void kmscon_utf8_mach_free(struct kmscon_utf8_mach *mach)
{
	if (!mach)
		return;

	free(mach);
}

int kmscon_utf8_mach_feed(struct kmscon_utf8_mach *mach, char ci)
{
	uint32_t c;

	if (!mach)
		return KMSCON_UTF8_START;

	c = ci;

	switch (mach->state) {
	case KMSCON_UTF8_START:
	case KMSCON_UTF8_ACCEPT:
	case KMSCON_UTF8_REJECT:
		if (c == 0xC0 || c == 0xC1) {
			/* overlong encoding for ASCII, reject */
			mach->state = KMSCON_UTF8_REJECT;
		} else if ((c & 0x80) == 0) {
			/* single byte, accept */
			mach->ch = c;
			mach->state = KMSCON_UTF8_ACCEPT;
		} else if ((c & 0xC0) == 0x80) {
			/* parser out of sync, ignore byte */
			mach->state = KMSCON_UTF8_START;
		} else if ((c & 0xE0) == 0xC0) {
			/* start of two byte sequence */
			mach->ch = (c & 0x1F) << 6;
			mach->state = KMSCON_UTF8_EXPECT1;
		} else if ((c & 0xF0) == 0xE0) {
			/* start of three byte sequence */
			mach->ch = (c & 0x0F) << 12;
			mach->state = KMSCON_UTF8_EXPECT2;
		} else if ((c & 0xF8) == 0xF0) {
			/* start of four byte sequence */
			mach->ch = (c & 0x07) << 18;
			mach->state = KMSCON_UTF8_EXPECT3;
		} else {
			/* overlong encoding, reject */
			mach->state = KMSCON_UTF8_REJECT;
		}
		break;
	case KMSCON_UTF8_EXPECT3:
		mach->ch |= (c & 0x3F) << 12;
		if ((c & 0xC0) == 0x80)
			mach->state = KMSCON_UTF8_EXPECT2;
		else
			mach->state = KMSCON_UTF8_REJECT;
		break;
	case KMSCON_UTF8_EXPECT2:
		mach->ch |= (c & 0x3F) << 6;
		if ((c & 0xC0) == 0x80)
			mach->state = KMSCON_UTF8_EXPECT1;
		else
			mach->state = KMSCON_UTF8_REJECT;
		break;
	case KMSCON_UTF8_EXPECT1:
		mach->ch |= c & 0x3F;
		if ((c & 0xC0) == 0x80)
			mach->state = KMSCON_UTF8_ACCEPT;
		else
			mach->state = KMSCON_UTF8_REJECT;
		break;
	default:
		mach->state = KMSCON_UTF8_REJECT;
		break;
	}

	return mach->state;
}

uint32_t kmscon_utf8_mach_get(struct kmscon_utf8_mach *mach)
{
	if (!mach || mach->state != KMSCON_UTF8_ACCEPT)
		return KMSCON_UCS4_INVALID;

	return mach->ch;
}

void kmscon_utf8_mach_reset(struct kmscon_utf8_mach *mach)
{
	if (!mach)
		return;

	mach->state = KMSCON_UTF8_START;
}
