/*
 * shl - Flagset
 *
 * Copyright (c) 2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * A dynamic flagset implementation
 */

#ifndef SHL_FLAGSET_H
#define SHL_FLAGSET_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "shl_array.h"
#include "shl_misc.h"

static inline int shl_flagset_new(struct shl_array **out)
{
	return shl_array_new(out, sizeof(unsigned long), 1);
}

static inline void shl_flagset_free(struct shl_array *arr)
{
	return shl_array_free(arr);
}

static inline int shl_flagset_alloc(struct shl_array *arr, unsigned int *out)
{
	static const unsigned long one = 1;
	unsigned int i, j;
	unsigned long *data;
	int ret;

	if (!arr)
		return -EINVAL;

	for (i = 0; i < arr->length; ++i) {
		data = SHL_ARRAY_AT(arr, unsigned long, i);
		for (j = 0; j < SHL_ULONG_BITS; ++j) {
			if (!(*data & (1UL << j))) {
				*data |= 1UL << j;
				*out = i * SHL_ULONG_BITS + j;
				return 0;
			}
		}
	}

	ret = shl_array_push(arr, &one);
	if (ret)
		return ret;

	*out = (arr->length - 1) * SHL_ULONG_BITS;
	return 0;
}

static inline int shl_flagset_reserve(struct shl_array *arr, unsigned int num)
{
	int ret;
	unsigned int idx, off;
	unsigned long *data;

	if (!arr)
		return -EINVAL;

	idx = num / SHL_ULONG_BITS;
	off = num % SHL_ULONG_BITS;

	if (idx >= arr->length) {
		ret = shl_array_zresize(arr, idx + 1);
		if (ret)
			return ret;
	}

	data = SHL_ARRAY_AT(arr, unsigned long, idx);
	if (*data & (1UL << off))
		return -EEXIST;

	*data |= 1UL << off;
	return 0;
}

static inline int shl_flagset_set(struct shl_array *arr, unsigned int num)
{
	int ret;

	ret = shl_flagset_reserve(arr, num);
	if (ret == -EEXIST)
		return 0;

	return ret;
}

static inline void shl_flagset_unset(struct shl_array *arr, unsigned int num)
{
	unsigned int idx, off;
	unsigned long *data;

	if (!arr)
		return;

	idx = num / SHL_ULONG_BITS;
	off = num % SHL_ULONG_BITS;

	if (idx >= arr->length)
		return;

	data = SHL_ARRAY_AT(arr, unsigned long, idx);
	*data &= ~(1UL << off);
}

#endif /* SHL_FLAGSET_H */
