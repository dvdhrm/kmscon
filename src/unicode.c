/*
 * kmscon - Unicode Handling
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
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

#include <errno.h>
#include <glib.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "unicode.h"

#define LOG_SUBSYSTEM "unicode"

#define KMSCON_UCS4_MAXLEN 10
#define KMSCON_UCS4_MAX 0x7fffffffUL
#define KMSCON_UCS4_INVALID 0xfffd

const kmscon_symbol_t kmscon_symbol_default = 0;
static const char default_u8[] = { 0 };

struct kmscon_symbol_table {
	unsigned long ref;
	GArray *index;
	GHashTable *symbols;
	uint32_t next_id;
};

static guint hash_ucs4(gconstpointer key)
{
	guint val = 5381;
	size_t i;
	const uint32_t *ucs4 = key;

	i = 0;
	while (ucs4[i] <= KMSCON_UCS4_MAX) {
		val = val * 33 + ucs4[i];
		++i;
	}

	return val;
}

static gboolean cmp_ucs4(gconstpointer a, gconstpointer b)
{
	size_t i;
	const uint32_t *v1, *v2;

	v1 = a;
	v2 = b;
	i = 0;

	while (1) {
		if (v1[i] > KMSCON_UCS4_MAX && v2[i] > KMSCON_UCS4_MAX)
			return TRUE;
		if (v1[i] > KMSCON_UCS4_MAX && v2[i] <= KMSCON_UCS4_MAX)
			return FALSE;
		if (v1[i] <= KMSCON_UCS4_MAX && v2[i] > KMSCON_UCS4_MAX)
			return FALSE;
		if (v1[i] != v2[i])
			return FALSE;

		++i;
	}
}

int kmscon_symbol_table_new(struct kmscon_symbol_table **out)
{
	struct kmscon_symbol_table *st;
	int ret;
	static const uint32_t *val = NULL; /* we need an lvalue for glib */

	if (!out)
		return -EINVAL;

	st = malloc(sizeof(*st));
	if (!st)
		return -ENOMEM;

	memset(st, 0, sizeof(*st));
	st->ref = 1;
	st->next_id = KMSCON_UCS4_MAX + 2;

	st->index = g_array_new(FALSE, TRUE, sizeof(uint32_t*));
	if (!st->index) {
		ret = -ENOMEM;
		goto err_free;
	}
	g_array_append_val(st->index, val);

	st->symbols = g_hash_table_new_full(hash_ucs4, cmp_ucs4,
						(GDestroyNotify) free, NULL);
	if (!st->symbols) {
		ret = -ENOMEM;
		goto err_arr;
	}

	*out = st;
	return 0;

err_arr:
	g_array_unref(st->index);
err_free:
	free(st);
	return ret;
}

void kmscon_symbol_table_ref(struct kmscon_symbol_table *st)
{
	if (!st)
		return;

	++st->ref;
}

void kmscon_symbol_table_unref(struct kmscon_symbol_table *st)
{
	if (!st || !st->ref)
		return;

	if (--st->ref)
		return;

	g_hash_table_unref(st->symbols);
	g_array_unref(st->index);
	free(st);
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

kmscon_symbol_t kmscon_symbol_append(struct kmscon_symbol_table *st,
					kmscon_symbol_t sym, uint32_t ucs4)
{
	uint32_t buf[KMSCON_UCS4_MAXLEN + 1], nsym, *nval;
	const uint32_t *ptr;
	size_t s;

	if (!st)
		return sym;

	if (ucs4 > KMSCON_UCS4_MAX) {
		log_warn("invalid ucs4 character");
		return sym;
	}

	ptr = kmscon_symbol_get(st, &sym, &s);
	if (s >= KMSCON_UCS4_MAXLEN)
		return sym;

	memcpy(buf, ptr, s * sizeof(uint32_t));
	buf[s++] = ucs4;
	buf[s++] = KMSCON_UCS4_MAX + 1;

	nsym = GPOINTER_TO_UINT(g_hash_table_lookup(st->symbols, buf));
	if (nsym)
		return nsym;

	log_debug("adding new composed symbol");

	nval = malloc(sizeof(uint32_t) * s);
	if (!nval)
		return sym;

	memcpy(nval, buf, s * sizeof(uint32_t));
	nsym = st->next_id++;
	g_hash_table_insert(st->symbols, nval, GUINT_TO_POINTER(nsym));
	g_array_append_val(st->index, nval);

	return nsym;
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
const uint32_t *kmscon_symbol_get(const struct kmscon_symbol_table *st,
					kmscon_symbol_t *sym, size_t *size)
{
	uint32_t *ucs4;

	if (*sym <= KMSCON_UCS4_MAX) {
		if (size)
			*size = 1;
		return sym;
	}

	if (!st)
		goto def_value;

	ucs4 = g_array_index(st->index, uint32_t*,
						*sym - (KMSCON_UCS4_MAX + 1));
	if (!ucs4)
		goto def_value;

	if (size) {
		*size = 0;
		while (ucs4[*size] <= KMSCON_UCS4_MAX)
			++*size;
	}

	return ucs4;

def_value:
	if (size)
		*size = 1;
	return &kmscon_symbol_default;
}

const char *kmscon_symbol_get_u8(const struct kmscon_symbol_table *st,
					kmscon_symbol_t sym, size_t *size)
{
	const uint32_t *ucs4;
	gchar *val;
	glong len;

	if (!st)
		goto def_value;

	ucs4 = kmscon_symbol_get(st, &sym, size);
	val = g_ucs4_to_utf8(ucs4, *size, NULL, &len, NULL);
	if (!val || len < 0)
		goto def_value;

	*size = len;
	return val;

def_value:
	if (size)
		*size = 1;
	return default_u8;
}

void kmscon_symbol_free_u8(const char *s)
{
	if (s != default_u8)
		g_free((char*)s);
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
