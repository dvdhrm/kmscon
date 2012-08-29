/*
 * kmscon - Miscellaneous Helpers
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
 * Miscellaneous Helpers
 * This provides several helper objects like memory-rings or similar.
 */

#ifndef KMSCON_MISC_H
#define KMSCON_MISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* miscellaneous */

#define kmscon_offsetof(pointer, type, member) ({ \
		const typeof(((type*)0)->member) *__ptr = (pointer); \
		(type*)(((char*)__ptr) - offsetof(type, member)); \
	})

/* ring buffer for arbitrary byte-streams */

struct kmscon_ring;

int kmscon_ring_new(struct kmscon_ring **out);
void kmscon_ring_free(struct kmscon_ring *ring);
bool kmscon_ring_is_empty(struct kmscon_ring *ring);

int kmscon_ring_write(struct kmscon_ring *ring, const char *val, size_t len);
const char *kmscon_ring_peek(struct kmscon_ring *ring, size_t *len);
void kmscon_ring_drop(struct kmscon_ring *ring, size_t len);

/* callback hooks */

struct kmscon_hook;
typedef void (*kmscon_hook_cb) (void *parent, void *arg, void *data);

int kmscon_hook_new(struct kmscon_hook **out);
void kmscon_hook_free(struct kmscon_hook *hook);
unsigned int kmscon_hook_num(struct kmscon_hook *hook);
int kmscon_hook_add(struct kmscon_hook *hook, kmscon_hook_cb cb, void *data);
void kmscon_hook_rm(struct kmscon_hook *hook, kmscon_hook_cb cb, void *data);
void kmscon_hook_call(struct kmscon_hook *hook, void *parent, void *arg);
#define kmscon_hook_add_cast(hook, cb, data) \
	kmscon_hook_add((hook), (kmscon_hook_cb)(cb), (data))
#define kmscon_hook_rm_cast(hook, cb, data) \
	kmscon_hook_rm((hook), (kmscon_hook_cb)(cb), (data))

/* hash-tables */

struct kmscon_hashtable;

typedef unsigned int (*kmscon_hash_cb) (const void *data);
typedef bool (*kmscon_equal_cb) (const void *data1, const void *data2);
typedef void (*kmscon_free_cb) (void *data);

unsigned int kmscon_direct_hash(const void *data);
bool kmscon_direct_equal(const void *data1, const void *data2);

int kmscon_hashtable_new(struct kmscon_hashtable **out,
				kmscon_hash_cb hash_cb,
				kmscon_equal_cb equal_cb,
				kmscon_free_cb free_key,
				kmscon_free_cb free_value);
void kmscon_hashtable_free(struct kmscon_hashtable *tbl);
int kmscon_hashtable_insert(struct kmscon_hashtable *tbl, void *key,
				void *data);
bool kmscon_hashtable_find(struct kmscon_hashtable *tbl, void **out, void *key);

/* dynamic arrays */

struct kmscon_array;

int kmscon_array_new(struct kmscon_array **out, size_t element_size,
		     size_t initial_size);
void kmscon_array_free(struct kmscon_array *arr);

int kmscon_array_push(struct kmscon_array *arr, void *data);
void kmscon_array_pop(struct kmscon_array *arr);
void *kmscon_array_get_array(struct kmscon_array *arr);
size_t kmscon_array_get_length(struct kmscon_array *arr);
size_t kmscon_array_get_bsize(struct kmscon_array *arr);
size_t kmscon_array_get_element_size(struct kmscon_array *arr);

#define KMSCON_ARRAY_AT(_arr, _type, _pos) \
	(&((_type*)kmscon_array_get_array(_arr))[(_pos)])

/* time measurement */

struct kmscon_timer;

int kmscon_timer_new(struct kmscon_timer **out);
void kmscon_timer_free(struct kmscon_timer *timer);

void kmscon_timer_reset(struct kmscon_timer *timer);
void kmscon_timer_start(struct kmscon_timer *timer);
uint64_t kmscon_timer_stop(struct kmscon_timer *timer);
uint64_t kmscon_timer_elapsed(struct kmscon_timer *timer);

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

#endif /* KMSCON_MISC_H */
