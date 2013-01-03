/*
 * shl - Named-Objects Registers
 *
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Named-Objects Registers
 * We often have an interface and several different backends that implement the
 * interface. To allow dynamic loading/unloading of backends, we give each
 * backend a name and vtable/data-ptr.
 * Once registered, the backend can be searched for and then used. The
 * shl_register object manages the backend loading/unloading in a thread-safe
 * way so we can share the backends between threads. It also manages access to
 * still used backends while they have been unregistered. Only after the last
 * record of a backend has been dropped, the backend is fully unused.
 */

#ifndef SHL_REGISTER_H
#define SHL_REGISTER_H

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "shl_dlist.h"

typedef void (*shl_register_destroy_cb) (void *data);

struct shl_register_record {
	struct shl_dlist list;

	pthread_mutex_t mutex;
	unsigned long ref;
	char *name;
	void *data;
	shl_register_destroy_cb destroy;
};

struct shl_register {
	pthread_mutex_t mutex;
	struct shl_dlist records;
};

#define SHL_REGISTER_INIT(name) { \
		.mutex = PTHREAD_MUTEX_INITIALIZER, \
		.records = SHL_DLIST_INIT((name).records), \
	}

static inline void shl_register_record_ref(struct shl_register_record *record)
{
	int ret;

	if (!record)
		return;

	ret = pthread_mutex_lock(&record->mutex);
	if (record->ref)
		++record->ref;
	if (!ret)
		pthread_mutex_unlock(&record->mutex);
}

static inline void shl_register_record_unref(struct shl_register_record *record)
{
	unsigned long ref = 1;
	int ret;

	if (!record)
		return;

	ret = pthread_mutex_lock(&record->mutex);
	if (record->ref)
		ref = --record->ref;
	if (!ret)
		pthread_mutex_unlock(&record->mutex);

	if (ref)
		return;

	if (record->destroy)
		record->destroy(record->data);
	pthread_mutex_destroy(&record->mutex);
	free(record->name);
	free(record);
}

static inline int shl_register_new(struct shl_register **out)
{
	struct shl_register *reg;
	int ret;

	if (!out)
		return -EINVAL;

	reg = malloc(sizeof(*reg));
	if (!reg)
		return -ENOMEM;
	memset(reg, 0, sizeof(*reg));
	shl_dlist_init(&reg->records);

	ret = pthread_mutex_init(&reg->mutex, NULL);
	if (ret) {
		ret = -EFAULT;
		goto err_free;
	}

	*out = reg;
	return 0;

err_free:
	free(reg);
	return ret;
}

static inline void shl_register_free(struct shl_register *reg)
{
	struct shl_dlist *iter;
	struct shl_register_record *record;

	if (!reg)
		return;

	shl_dlist_for_each(iter, &reg->records) {
		record = shl_dlist_entry(iter, struct shl_register_record,
					 list);
		shl_dlist_unlink(&record->list);
		shl_register_record_unref(record);
	}

	pthread_mutex_destroy(&reg->mutex);
	free(reg);
}

static inline int shl_register_add_cb(struct shl_register *reg,
				      const char *name, void *data,
				      shl_register_destroy_cb destroy)
{
	struct shl_dlist *iter;
	struct shl_register_record *record;
	int ret;

	if (!reg || !name)
		return -EINVAL;

	ret = pthread_mutex_lock(&reg->mutex);
	if (ret)
		return -EFAULT;

	shl_dlist_for_each(iter, &reg->records) {
		record = shl_dlist_entry(iter, struct shl_register_record,
					 list);
		if (!strcmp(record->name, name)) {
			ret = -EALREADY;
			goto out_unlock;
		}
	}

	record = malloc(sizeof(*record));
	if (!record) {
		ret = -ENOMEM;
		goto out_unlock;
	}
	memset(record, 0, sizeof(*record));
	record->ref = 1;
	record->data = data;
	record->destroy = destroy;

	ret = pthread_mutex_init(&record->mutex, NULL);
	if (ret) {
		ret = -EFAULT;
		goto err_free;
	}

	record->name = strdup(name);
	if (!record->name) {
		ret = -ENOMEM;
		goto err_mutex;
	}

	shl_dlist_link_tail(&reg->records, &record->list);
	ret = 0;
	goto out_unlock;

err_mutex:
	pthread_mutex_destroy(&record->mutex);
err_free:
	free(record);
out_unlock:
	pthread_mutex_unlock(&reg->mutex);
	return ret;
}

static inline int shl_register_add(struct shl_register *reg, const char *name,
				   void *data)
{
	return shl_register_add_cb(reg, name, data, NULL);
}

static inline void shl_register_remove(struct shl_register *reg,
				       const char *name)
{
	struct shl_dlist *iter;
	struct shl_register_record *record;
	int ret;

	if (!reg || !name)
		return;

	ret = pthread_mutex_lock(&reg->mutex);
	if (ret)
		return;

	shl_dlist_for_each(iter, &reg->records) {
		record = shl_dlist_entry(iter, struct shl_register_record,
					 list);
		if (strcmp(record->name, name))
			continue;

		shl_dlist_unlink(&record->list);
		shl_register_record_unref(record);
		break;
	}

	pthread_mutex_unlock(&reg->mutex);
}

static inline struct shl_register_record *shl_register_find(
						struct shl_register *reg,
						const char *name)
{
	struct shl_dlist *iter;
	struct shl_register_record *record, *res;
	int ret;

	if (!reg || !name)
		return NULL;

	ret = pthread_mutex_lock(&reg->mutex);
	if (ret)
		return NULL;

	res = NULL;
	shl_dlist_for_each(iter, &reg->records) {
		record = shl_dlist_entry(iter, struct shl_register_record,
					 list);
		if (!strcmp(record->name, name)) {
			res = record;
			shl_register_record_ref(res);
			break;
		}
	}

	pthread_mutex_unlock(&reg->mutex);
	return res;
}

static inline struct shl_register_record *shl_register_first(
						struct shl_register *reg)
{
	int ret;
	void *res;

	if (!reg)
		return NULL;

	ret = pthread_mutex_lock(&reg->mutex);
	if (ret)
		return NULL;

	if (shl_dlist_empty(&reg->records)) {
		res = NULL;
	} else {
		res = shl_dlist_entry(reg->records.next,
				      struct shl_register_record, list);
		shl_register_record_ref(res);
	}

	pthread_mutex_unlock(&reg->mutex);
	return res;
}

static inline struct shl_register_record *shl_register_last(
						struct shl_register *reg)
{
	int ret;
	void *res;

	if (!reg)
		return NULL;

	ret = pthread_mutex_lock(&reg->mutex);
	if (ret)
		return NULL;

	if (shl_dlist_empty(&reg->records)) {
		res = NULL;
	} else {
		res = shl_dlist_entry(reg->records.prev,
				      struct shl_register_record, list);
		shl_register_record_ref(res);
	}

	pthread_mutex_unlock(&reg->mutex);
	return res;
}

#endif /* SHL_REGISTER_H */
