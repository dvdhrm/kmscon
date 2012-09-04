/*
 * kmscon - Double Linked List
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

#ifndef KMSCON_STATIC_DLIST_H
#define KMSCON_STATIC_DLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* miscellaneous */

#define kmscon_offsetof(pointer, type, member) ({ \
		const typeof(((type*)0)->member) *__ptr = (pointer); \
		(type*)(((char*)__ptr) - offsetof(type, member)); \
	})

/* double linked list */

struct kmscon_dlist {
	struct kmscon_dlist *next;
	struct kmscon_dlist *prev;
};

#define KMSCON_DLIST_INIT(head) { &(head), &(head) }

static inline void kmscon_dlist_init(struct kmscon_dlist *list)
{
	list->next = list;
	list->prev = list;
}

static inline void kmscon_dlist__link(struct kmscon_dlist *prev,
					struct kmscon_dlist *next,
					struct kmscon_dlist *n)
{
	next->prev = n;
	n->next = next;
	n->prev = prev;
	prev->next = n;
}

static inline void kmscon_dlist_link(struct kmscon_dlist *head,
					struct kmscon_dlist *n)
{
	return kmscon_dlist__link(head, head->next, n);
}

static inline void kmscon_dlist_link_tail(struct kmscon_dlist *head,
					struct kmscon_dlist *n)
{
	return kmscon_dlist__link(head->prev, head, n);
}

static inline void kmscon_dlist__unlink(struct kmscon_dlist *prev,
					struct kmscon_dlist *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void kmscon_dlist_unlink(struct kmscon_dlist *e)
{
	kmscon_dlist__unlink(e->prev, e->next);
	e->prev = NULL;
	e->next = NULL;
}

static inline bool kmscon_dlist_empty(struct kmscon_dlist *head)
{
	return head->next == head;
}

#define kmscon_dlist_entry(ptr, type, member) \
	kmscon_offsetof((ptr), type, member)

#define kmscon_dlist_for_each(iter, head) \
	for (iter = (head)->next; iter != (head); iter = iter->next)

#define kmscon_dlist_for_each_safe(iter, tmp, head) \
	for (iter = (head)->next, tmp = iter->next; iter != (head); \
		iter = tmp, tmp = iter->next)

#endif /* KMSCON_STATIC_DLIST_H */
