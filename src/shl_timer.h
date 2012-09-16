/*
 * shl - Timers
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
 * Timers
 */

#ifndef SHL_TIMER_H
#define SHL_TIMER_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

struct shl_timer {
	struct timespec start;
	uint64_t elapsed;
};

static inline void shl_timer_reset(struct shl_timer *timer)
{
	if (!timer)
		return;

	clock_gettime(CLOCK_MONOTONIC, &timer->start);
	timer->elapsed = 0;
}

static inline int shl_timer_new(struct shl_timer **out)
{
	struct shl_timer *timer;

	if (!out)
		return -EINVAL;

	timer = malloc(sizeof(*timer));
	if (!timer)
		return -ENOMEM;
	memset(timer, 0, sizeof(*timer));
	shl_timer_reset(timer);

	*out = timer;
	return 0;
}

static inline void shl_timer_free(struct shl_timer *timer)
{
	if (!timer)
		return;

	free(timer);
}

static inline void shl_timer_start(struct shl_timer *timer)
{
	if (!timer)
		return;

	clock_gettime(CLOCK_MONOTONIC, &timer->start);
}

static inline uint64_t shl_timer_stop(struct shl_timer *timer)
{
	struct timespec spec;
	int64_t off, nsec;

	if (!timer)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &spec);
	off = spec.tv_sec - timer->start.tv_sec;
	nsec = spec.tv_nsec - timer->start.tv_nsec;
	if (nsec < 0) {
		--off;
		nsec += 1000000000ULL;
	}
	off *= 1000000;
	off += nsec / 1000;

	memcpy(&timer->start, &spec, sizeof(spec));
	timer->elapsed += off;
	return timer->elapsed;
}

static inline uint64_t shl_timer_elapsed(struct shl_timer *timer)
{
	struct timespec spec;
	int64_t off, nsec;

	if (!timer)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &spec);
	off = spec.tv_sec - timer->start.tv_sec;
	nsec = spec.tv_nsec - timer->start.tv_nsec;
	if (nsec < 0) {
		--off;
		nsec += 1000000000ULL;
	}
	off *= 1000000;
	off += nsec / 1000;

	return timer->elapsed + off;
}

#endif /* SHL_TIMER_H */
