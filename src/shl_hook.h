/*
 * shl - Hook Handling
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
 * Simply hook-implementation
 */

#ifndef SHL_HOOK_H
#define SHL_HOOK_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct shl_hook;
struct shl_hook_entry;
typedef void (*shl_hook_cb) (void *parent, void *arg, void *data);

#define shl_hook_add_cast(hook, cb, data) \
	shl_hook_add((hook), (shl_hook_cb)(cb), (data))
#define shl_hook_rm_cast(hook, cb, data) \
	shl_hook_rm((hook), (shl_hook_cb)(cb), (data))

struct shl_hook_entry {
	struct shl_hook_entry *next;
	shl_hook_cb cb;
	void *data;
};

struct shl_hook {
	unsigned int num;
	struct shl_hook_entry *entries;
	struct shl_hook_entry *cur_entry;
};

static inline int shl_hook_new(struct shl_hook **out)
{
	struct shl_hook *hook;

	if (!out)
		return -EINVAL;

	hook = malloc(sizeof(*hook));
	if (!hook)
		return -ENOMEM;
	memset(hook, 0, sizeof(*hook));

	*out = hook;
	return 0;
}

static inline void shl_hook_free(struct shl_hook *hook)
{
	struct shl_hook_entry *entry;

	if (!hook)
		return;

	while ((entry = hook->entries)) {
		hook->entries = entry->next;
		free(entry);
	}

	free(hook);
}

static inline unsigned int shl_hook_num(struct shl_hook *hook)
{
	if (!hook)
		return 0;

	return hook->num;
}

static inline int shl_hook_add(struct shl_hook *hook, shl_hook_cb cb,
				  void *data)
{
	struct shl_hook_entry *entry;

	if (!hook || !cb)
		return -EINVAL;

	entry = malloc(sizeof(*entry));
	if (!entry)
		return -ENOMEM;
	memset(entry, 0, sizeof(*entry));
	entry->cb = cb;
	entry->data = data;

	entry->next = hook->entries;
	hook->entries = entry;
	hook->num++;
	return 0;
}

static inline void shl_hook_rm(struct shl_hook *hook, shl_hook_cb cb,
				  void *data)
{
	struct shl_hook_entry *entry, *tmp;

	if (!hook || !cb || !hook->entries)
		return;

	tmp = NULL;
	if (hook->entries->cb == cb && hook->entries->data == data) {
		tmp = hook->entries;
		hook->entries = tmp->next;
	} else for (entry = hook->entries; entry->next; entry = entry->next) {
		if (entry->next->cb == cb && entry->next->data == data) {
			tmp = entry->next;
			entry->next = tmp->next;
			break;
		}
	}

	if (tmp) {
		/* if *_call() is currently running we must not disturb it */
		if (hook->cur_entry == tmp)
			hook->cur_entry = tmp->next;
		free(tmp);
		hook->num--;
	}
}

static inline void shl_hook_call(struct shl_hook *hook, void *parent,
				    void *arg)
{
	if (!hook)
		return;

	hook->cur_entry = hook->entries;
	while (hook->cur_entry) {
		hook->cur_entry->cb(parent, arg, hook->cur_entry->data);
		if (hook->cur_entry)
			hook->cur_entry = hook->cur_entry->next;
	}
}

#endif /* SHL_HOOK_H */
