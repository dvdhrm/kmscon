/*
 * kmscon - Character-Device Session
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
 * Character-Device Session
 */

#ifndef KMSCON_CDEV_H
#define KMSCON_CDEV_H

#include <errno.h>
#include <stdlib.h>
#include "kmscon_seat.h"

#ifdef BUILD_ENABLE_SESSION_CDEV

int kmscon_cdev_register(struct kmscon_session **out,
			 struct kmscon_seat *seat);

#else /* !BUILD_ENABLE_SESSION_CDEV */

static inline int kmscon_cdev_register(struct kmscon_session **out,
				       struct kmscon_seat *seat)
{
	return -EOPNOTSUPP;
}

#endif /* BUILD_ENABLE_SESSION_CDEV */

#endif /* KMSCON_CDEV_H */
