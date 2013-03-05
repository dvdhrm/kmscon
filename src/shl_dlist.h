/*
 * shl - Double Linked List
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
 * A simple double linked list implementation
 */

#ifndef SHL_DLIST_H
#define SHL_DLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* miscellaneous */

#define shl_offsetof(pointer, type, member) ({ \
		const typeof(((type*)0)->member) *__ptr = (pointer); \
		(type*)(((char*)__ptr) - offsetof(type, member)); \
	})

/* double linked list */

struct shl_dlist {
	struct shl_dlist *next;
	struct shl_dlist *prev;
};

#define SHL_DLIST_INIT(head) { &(head), &(head) }

static inline void shl_dlist_init(struct shl_dlist *list)
{
	list->next = list;
	list->prev = list;
}

static inline void shl_dlist__link(struct shl_dlist *prev,
					struct shl_dlist *next,
					struct shl_dlist *n)
{
	next->prev = n;
	n->next = next;
	n->prev = prev;
	prev->next = n;
}

static inline void shl_dlist_link(struct shl_dlist *head,
					struct shl_dlist *n)
{
	return shl_dlist__link(head, head->next, n);
}

static inline void shl_dlist_link_tail(struct shl_dlist *head,
					struct shl_dlist *n)
{
	return shl_dlist__link(head->prev, head, n);
}

static inline void shl_dlist__unlink(struct shl_dlist *prev,
					struct shl_dlist *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void shl_dlist_unlink(struct shl_dlist *e)
{
	shl_dlist__unlink(e->prev, e->next);
	e->prev = NULL;
	e->next = NULL;
}

static inline bool shl_dlist_empty(struct shl_dlist *head)
{
	return head->next == head;
}

#define shl_dlist_entry(ptr, type, member) \
	shl_offsetof((ptr), type, member)

#define shl_dlist_first(head, type, member) \
	shl_dlist_entry((head)->next, type, member)

#define shl_dlist_last(head, type, member) \
	shl_dlist_entry((head)->prev, type, member)

#define shl_dlist_for_each(iter, head) \
	for (iter = (head)->next; iter != (head); iter = iter->next)

#define shl_dlist_for_each_but_one(iter, start, head) \
	for (iter = ((start)->next == (head)) ? \
				(start)->next->next : \
				(start)->next; \
	     iter != (start); \
	     iter = (iter->next == (head) && (start) != (head)) ? \
				iter->next->next : \
				iter->next)

#define shl_dlist_for_each_safe(iter, tmp, head) \
	for (iter = (head)->next, tmp = iter->next; iter != (head); \
		iter = tmp, tmp = iter->next)

#define shl_dlist_for_each_reverse(iter, head) \
	for (iter = (head)->prev; iter != (head); iter = iter->prev)

#define shl_dlist_for_each_reverse_but_one(iter, start, head) \
	for (iter = ((start)->prev == (head)) ? \
				(start)->prev->prev : \
				(start)->prev; \
	     iter != (start); \
	     iter = (iter->prev == (head) && (start) != (head)) ? \
				iter->prev->prev : \
				iter->prev)

#define shl_dlist_for_each_reverse_safe(iter, tmp, head) \
	for (iter = (head)->prev, tmp = iter->prev; iter != (head); \
		iter = tmp, tmp = iter->prev)

#endif /* SHL_DLIST_H */
