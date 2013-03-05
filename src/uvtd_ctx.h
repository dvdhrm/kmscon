/*
 * uvtd - User-space VT daemon
 *
 * Copyright (c) 2013 David Herrmann <dh.herrmann@gmail.com>
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
 * Contexts
 * A context manages a single UVT seat. It creates the seat object, allocates
 * the VTs and provides all the bookkeeping for the sessions. It's the main
 * entry point after the seat selectors in uvtd-main.
 */

#ifndef UVTD_CTX_H
#define UVTD_CTX_H

#include <stdlib.h>
#include "eloop.h"
#include "uvt.h"

struct uvtd_ctx;

int uvtd_ctx_new(struct uvtd_ctx **out, const char *seatname,
		 struct ev_eloop *eloop, struct uvt_ctx *uctx);
void uvtd_ctx_free(struct uvtd_ctx *ctx);

void uvtd_ctx_reconf(struct uvtd_ctx *ctx, unsigned int legacy_num);

#endif /* UVTD_CTX_H */
