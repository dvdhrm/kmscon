/*
 * shl - Dynamic Array
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
 * A dynamic hash table implementation
 */

#ifndef SHL_HASHTABLE_H
#define SHL_HASHTABLE_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "external/htable.h"

struct shl_hashtable;

typedef unsigned int (*shl_hash_cb) (const void *data);
typedef bool (*shl_equal_cb) (const void *data1, const void *data2);
typedef void (*shl_free_cb) (void *data);

struct shl_hashentry {
	void *key;
	void *value;
};

struct shl_hashtable {
	struct htable tbl;
	shl_hash_cb hash_cb;
	shl_equal_cb equal_cb;
	shl_free_cb free_key;
	shl_free_cb free_value;
};

static inline unsigned int shl_direct_hash(const void *data)
{
	return (unsigned int)(unsigned long)data;
}

static inline bool shl_direct_equal(const void *data1, const void *data2)
{
	return data1 == data2;
}

static size_t shl_rehash(const void *ele, void *priv)
{
	struct shl_hashtable *tbl = priv;
	const struct shl_hashentry *ent = ele;

	return tbl->hash_cb(ent->key);
}

static inline int shl_hashtable_new(struct shl_hashtable **out,
				    shl_hash_cb hash_cb,
				    shl_equal_cb equal_cb,
				    shl_free_cb free_key,
				    shl_free_cb free_value)
{
	struct shl_hashtable *tbl;

	if (!out || !hash_cb || !equal_cb)
		return -EINVAL;

	tbl = malloc(sizeof(*tbl));
	if (!tbl)
		return -ENOMEM;
	memset(tbl, 0, sizeof(*tbl));
	tbl->hash_cb = hash_cb;
	tbl->equal_cb = equal_cb;
	tbl->free_key = free_key;
	tbl->free_value = free_value;

	htable_init(&tbl->tbl, shl_rehash, tbl);

	*out = tbl;
	return 0;
}

static inline void shl_hashtable_free(struct shl_hashtable *tbl)
{
	struct htable_iter i;
	struct shl_hashentry *entry;

	if (!tbl)
		return;

	for (entry = htable_first(&tbl->tbl, &i);
	     entry;
	     entry = htable_next(&tbl->tbl, &i)) {
		htable_delval(&tbl->tbl, &i);
		if (tbl->free_key)
			tbl->free_key(entry->key);
		if (tbl->free_value)
			tbl->free_value(entry->value);
		free(entry);
	}

	htable_clear(&tbl->tbl);
	free(tbl);
}

static inline int shl_hashtable_insert(struct shl_hashtable *tbl, void *key,
				       void *value)
{
	struct shl_hashentry *entry;
	size_t hash;

	if (!tbl)
		return -EINVAL;

	entry = malloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	entry->key = key;
	entry->value = value;

	hash = tbl->hash_cb(key);

	if (!htable_add(&tbl->tbl, hash, entry)) {
		free(entry);
		return -ENOMEM;
	}

	return 0;
}

static inline void shl_hashtable_remove(struct shl_hashtable *tbl, void *key)
{
	struct htable_iter i;
	struct shl_hashentry *entry;
	size_t hash;

	if (!tbl)
		return;

	hash = tbl->hash_cb(key);

	for (entry = htable_firstval(&tbl->tbl, &i, hash);
	     entry;
	     entry = htable_nextval(&tbl->tbl, &i, hash)) {
		if (tbl->equal_cb(key, entry->key)) {
			htable_delval(&tbl->tbl, &i);
			return;
		}
	}
}

static inline bool shl_hashtable_find(struct shl_hashtable *tbl, void **out,
				      void *key)
{
	struct htable_iter i;
	struct shl_hashentry *entry;
	size_t hash;

	if (!tbl)
		return false;

	hash = tbl->hash_cb(key);

	for (entry = htable_firstval(&tbl->tbl, &i, hash);
	     entry;
	     entry = htable_nextval(&tbl->tbl, &i, hash)) {
		if (tbl->equal_cb(key, entry->key)) {
			if (out)
				*out = entry->value;
			return true;
		}
	}

	return false;
}

#endif /* SHL_HASHTABLE_H */
