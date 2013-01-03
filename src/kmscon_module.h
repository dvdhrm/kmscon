/*
 * kmscon - Module handling
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
 * Module Handling
 * Several subsystems of kmscon provide a generic interface that is implemented
 * by different backends. The user can choose a backend that is then used.
 * To make out-of-tree development easier and, more importantly, to reduce the
 * direct dependencies to external libraries, this subsystem implements a
 * dynamically-loadable module system.
 *
 * Modules can be loaded and unloaded during runtime. A module basically
 * provides memory-storage for code. As long as any code of a module is still
 * used (that is, registered as callback) we must not unload the module.
 * Therefore, we use reference-counting to allow other subsystems to acquire and
 * release code sections.
 *
 * A module needs to provide "module_init". Everything else is optional.
 * "module_init" is called after the module has been loaded and should
 * initialize the module. "module_exit" is called after the module has been
 * unloaded and the last reference to the module has been dropped. Therefore, it
 * is safe to release all allocated resources in "module_exit".
 *
 * "module_load" is called after "module_init". A module should register its
 * resources here. "module_unload" is called when the module is scheduled for
 * removal. A module should unregister its resources here. However, it must not
 * release the resources as there might still be users of it. Only when
 * "module_exit" is called, kmscon guarantees that there are no more users and
 * the module can release its resources.
 */

#ifndef KMSCON_MODULE_H
#define KMSCON_MODULE_H

#include <stdlib.h>

struct kmscon_module;

int kmscon_module_open(struct kmscon_module **out, const char *file);
void kmscon_module_ref(struct kmscon_module *module);
void kmscon_module_unref(struct kmscon_module *module);

int kmscon_module_load(struct kmscon_module *module);
void kmscon_module_unload(struct kmscon_module *module);

void kmscon_load_modules(void);
void kmscon_unload_modules(void);

#endif /* KMSCON_MODULE_H */
