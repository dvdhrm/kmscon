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
 * A circular memory ring implementation
 */

#ifndef SHL_RING_H
#define SHL_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define SHL_RING_SIZE 512

struct shl_ring_entry {
	struct shl_ring_entry *next;
	size_t len;
	char buf[];
};

struct shl_ring {
	struct shl_ring_entry *first;
	struct shl_ring_entry *last;
};

static inline int shl_ring_new(struct shl_ring **out)
{
	struct shl_ring *ring;

	if (!out)
		return -EINVAL;

	ring = malloc(sizeof(*ring));
	if (!ring)
		return -ENOMEM;

	memset(ring, 0, sizeof(*ring));

	*out = ring;
	return 0;
}

static inline void shl_ring_free(struct shl_ring *ring)
{
	struct shl_ring_entry *tmp;

	if (!ring)
		return;

	while (ring->first) {
		tmp = ring->first;
		ring->first = tmp->next;
		free(tmp);
	}
	free(ring);
}

static inline bool shl_ring_is_empty(struct shl_ring *ring)
{
	if (!ring)
		return true;

	return ring->first == NULL;
}

static inline int shl_ring_write(struct shl_ring *ring, const char *val,
				 size_t len)
{
	struct shl_ring_entry *ent;
	size_t space, cp;

	if (!ring || !val || !len)
		return -EINVAL;

next:
	ent = ring->last;
	if (!ent || ent->len >= SHL_RING_SIZE) {
		ent = malloc(sizeof(*ent) + SHL_RING_SIZE);
		if (!ent)
			return -ENOMEM;

		ent->len = 0;
		ent->next = NULL;
		if (ring->last)
			ring->last->next = ent;
		else
			ring->first = ent;
		ring->last = ent;
	}

	space = SHL_RING_SIZE - ent->len;
	if (len >= space)
		cp = space;
	else
		cp = len;

	memcpy(&ent->buf[ent->len], val, cp);
	ent->len += cp;

	val = &val[cp];
	len -= cp;
	if (len > 0)
		goto next;

	return 0;
}

static inline const char *shl_ring_peek(struct shl_ring *ring, size_t *len,
					size_t offset)
{
	struct shl_ring_entry *iter;

	if (!ring || !ring->first || !len) {
		if (len)
			*len = 0;
		return NULL;
	}

	iter = ring->first;
	while (iter->len <= offset) {
		if (!iter->next) {
			*len = 0;
			return NULL;
		}

		offset -= iter->len;
		iter = iter->next;
	}

	*len = ring->first->len - offset;
	return &ring->first->buf[offset];
}

static inline void shl_ring_drop(struct shl_ring *ring, size_t len)
{
	struct shl_ring_entry *ent;

	if (!ring || !len)
		return;

next:
	ent = ring->first;
	if (!ent)
		return;

	if (len >= ent->len) {
		len -= ent->len;
		ring->first = ent->next;
		free(ent);
		if (!ring->first)
			ring->last = NULL;

		if (len)
			goto next;
	} else {
		memmove(ent->buf, &ent->buf[len], ent->len - len);
		ent->len -= len;
	}
}

static inline void shl_ring_flush(struct shl_ring *ring)
{
	struct shl_ring_entry *tmp;

	if (!ring)
		return;

	while (ring->first) {
		tmp = ring->first;
		ring->first = tmp->next;
		free(tmp);
	}
	ring->last = NULL;
}

#endif /* SHL_RING_H */
