/*
 * kmscon - Module Interface
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * Public Module Interface
 */

#ifndef KMSCON_MODULE_INTERFACE_H
#define KMSCON_MODULE_INTERFACE_H

#include <stdbool.h>
#include <stdlib.h>
#include "githead.h"
#include "kmscon_module.h"
#include "shl_dlist.h"
#include "shl_misc.h"

struct kmscon_module_info {
	const char *githead;
	const char *date;
	const char *time;
	int (*init) (void);
	int (*load) (void);
	void (*unload) (void);
	void (*exit) (void);
};

struct kmscon_module {
	struct kmscon_module_info info;
	struct shl_dlist list;
	unsigned long ref;
	bool loaded;
	void *handle;
	char *file;
};

#define KMSCON_MODULE(_init, _load, _unload, _exit) \
	struct kmscon_module module = { \
		.info = { \
			.githead = BUILD_GIT_HEAD, \
			.date = __DATE__, \
			.time = __TIME__, \
			.init = _init, \
			.load = _load, \
			.unload = _unload, \
			.exit = _exit, \
		}, \
	};

SHL_EXPORT
extern struct kmscon_module module;
#define KMSCON_THIS_MODULE (&module)

#endif /* KMSCON_MODULE_INTERFACE_H */
