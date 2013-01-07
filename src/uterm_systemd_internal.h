/*
 * uterm - Linux User-Space Terminal
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
 * Systemd integration
 * Systemd provides multi-seat support and other helpers that we can use in
 * uterm.
 */

#ifndef UTERM_SYSTEMD_H
#define UTERM_SYSTEMD_H

#include <stdlib.h>
#include "uterm_monitor.h"

struct uterm_sd;

#ifdef BUILD_ENABLE_MULTI_SEAT

int uterm_sd_new(struct uterm_sd **out);
void uterm_sd_free(struct uterm_sd *sd);
int uterm_sd_get_fd(struct uterm_sd *sd);
void uterm_sd_flush(struct uterm_sd *sd);
int uterm_sd_get_seats(struct uterm_sd *sd, char ***seats);

#else

static inline int uterm_sd_new(struct uterm_sd **out)
{
	return -EOPNOTSUPP;
}

static inline void uterm_sd_free(struct uterm_sd *sd)
{
}

static inline int uterm_sd_get_fd(struct uterm_sd *sd)
{
	return -1;
}

static inline void uterm_sd_flush(struct uterm_sd *sd)
{
}

static inline int uterm_sd_get_seats(struct uterm_sd *sd, char ***seats)
{
	return -EINVAL;
}

#endif

#endif /* UTERM_SYSTEMD_H */
