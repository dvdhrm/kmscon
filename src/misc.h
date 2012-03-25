/*
 * kmscon - Miscellaneous Helpers
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
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
#define kMSCON_MISC_H

#include <stdbool.h>
#include <stdlib.h>

struct kmscon_ring;

int kmscon_ring_new(struct kmscon_ring **out);
void kmscon_ring_free(struct kmscon_ring *ring);
bool kmscon_ring_is_empty(struct kmscon_ring *ring);

int kmscon_ring_write(struct kmscon_ring *ring, const char *val, size_t len);
const char *kmscon_ring_peek(struct kmscon_ring *ring, size_t *len);
void kmscon_ring_drop(struct kmscon_ring *ring, size_t len);

struct kmscon_hook;
typedef void (*kmscon_hook_cb) (void *parent, void *arg, void *data);

int kmscon_hook_new(struct kmscon_hook **out);
void kmscon_hook_free(struct kmscon_hook *hook);
int kmscon_hook_add(struct kmscon_hook *hook, kmscon_hook_cb cb, void *data);
void kmscon_hook_rm(struct kmscon_hook *hook, kmscon_hook_cb cb, void *data);
void kmscon_hook_call(struct kmscon_hook *hook, void *parent, void *arg);
#define kmscon_hook_add_cast(hook, cb, data) \
	kmscon_hook_add((hook), (kmscon_hook_cb)(cb), (data))
#define kmscon_hook_rm_cast(hook, cb, data) \
	kmscon_hook_rm((hook), (kmscon_hook_cb)(cb), (data))

#endif /* KMSCON_MISC_H */
