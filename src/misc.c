/*
 * kmscon - Miscellaneous Helpers
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
 * Miscellaneous Helpers
 * Rings: Rings are used to buffer a byte-stream of data. It works like a FIFO
 * queue but in-memory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RING_SIZE 512

struct ring_entry {
	struct ring_entry *next;
	size_t len;
	char buf[];
};

struct kmscon_ring {
	struct ring_entry *first;
	struct ring_entry *last;
};

int kmscon_ring_new(struct kmscon_ring **out)
{
	struct kmscon_ring *ring;

	if (!out)
		return -EINVAL;

	ring = malloc(sizeof(*ring));
	if (!ring)
		return -ENOMEM;

	memset(ring, 0, sizeof(*ring));

	*out = ring;
	return 0;
}

void kmscon_ring_free(struct kmscon_ring *ring)
{
	struct ring_entry *tmp;

	if (!ring)
		return;

	while (ring->first) {
		tmp = ring->first;
		ring->first = tmp->next;
		free(tmp);
	}
	free(ring);
}

bool kmscon_ring_is_empty(struct kmscon_ring *ring)
{
	if (!ring)
		return true;

	return ring->first == NULL;
}

int kmscon_ring_write(struct kmscon_ring *ring, const char *val, size_t len)
{
	struct ring_entry *ent;
	size_t space, cp;

	if (!ring || !val || !len)
		return -EINVAL;

next:
	ent = ring->last;
	if (!ent || ent->len >= RING_SIZE) {
		ent = malloc(sizeof(*ent) + RING_SIZE);
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

	space = RING_SIZE - ent->len;
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

const char *kmscon_ring_peek(struct kmscon_ring *ring, size_t *len)
{
	if (!ring || !ring->first)
		return NULL;

	*len = ring->first->len;
	return ring->first->buf;
}

void kmscon_ring_drop(struct kmscon_ring *ring, size_t len)
{
	struct ring_entry *ent;

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
