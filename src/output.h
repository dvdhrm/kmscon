/*
 * kmscon - KMS/DRM output handling
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
 * KMS/DRM Output Handling
 * This module provides a compositor object which manages the different outputs.
 * Each output object belongs to a connected monitor.
 * After creating a compositor object it will create a list of all available
 * outputs. All outputs are disconnected by default. If you connect an
 * output, a framebuffer with two renderbuffers is registered and you can start
 * drawing to it using double-buffering.
 * You can connect as many outputs as you want.
 *
 * To allow other applications to access the DRM you can put a compositor asleep
 * and wake it up. When the compositor is asleep, the OpenGL context and
 * framebuffers are still available, however, you cannot add or remove outputs
 * unless the compositor is awake. You also cannot modify output modes or other
 * output settings. It is recommended to avoid accessing the output objects at
 * all as most of the functions simply return -EINVAL while being asleep.
 *
 * When waking up the compositor, it rereads all connected outputs. If a
 * previously connected output has gone, it disconnects the output, removes
 * the associated framebuffer and context and unbinds the output object from the
 * compositor. If you own a reference to the output object, you should unref it
 * now.
 * You should also reread the output list for newly connected outputs.
 * You can also force the compositor to reread all outputs if you noticed any
 * monitor hotplugging (for instance via udev).
 *
 * An output may be used in different modes. Each output chooses one mode by
 * default, however, you can always switch to another mode if you want another
 * pixel-resolution, color-mode, etc. When switching modes, the current
 * framebuffer is destroyed and a new one is created.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>

struct kmscon_mode;
struct kmscon_output;
struct kmscon_compositor;

/* output modes */

int kmscon_mode_new(struct kmscon_mode **out);
void kmscon_mode_ref(struct kmscon_mode *mode);
void kmscon_mode_unref(struct kmscon_mode *mode);

struct kmscon_mode *kmscon_mode_next(struct kmscon_mode *mode);
const char *kmscon_mode_get_name(const struct kmscon_mode *mode);
uint32_t kmscon_mode_get_width(const struct kmscon_mode *mode);
uint32_t kmscon_mode_get_height(const struct kmscon_mode *mode);

/* compositor outputs */

int kmscon_output_new(struct kmscon_output **out);
void kmscon_output_ref(struct kmscon_output *output);
void kmscon_output_unref(struct kmscon_output *output);

bool kmscon_output_is_awake(struct kmscon_output *output);
struct kmscon_output *kmscon_output_next(struct kmscon_output *output);
struct kmscon_mode *kmscon_output_get_modes(struct kmscon_output *output);
struct kmscon_mode *kmscon_output_get_current(struct kmscon_output *output);
struct kmscon_mode *kmscon_output_get_default(struct kmscon_output *output);

int kmscon_output_activate(struct kmscon_output *output,
						struct kmscon_mode *mode);
void kmscon_output_deactivate(struct kmscon_output *output);
bool kmscon_output_is_active(struct kmscon_output *output);

int kmscon_output_use(struct kmscon_output *output);
int kmscon_output_swap(struct kmscon_output *output);

/* compositors */

int kmscon_compositor_new(struct kmscon_compositor **out);
void kmscon_compositor_ref(struct kmscon_compositor *comp);
void kmscon_compositor_unref(struct kmscon_compositor *comp);

void kmscon_compositor_sleep(struct kmscon_compositor *comp);
int kmscon_compositor_wake_up(struct kmscon_compositor *comp);
bool kmscon_compositor_is_asleep(struct kmscon_compositor *comp);
int kmscon_compositor_use(struct kmscon_compositor *comp);

int kmscon_compositor_get_fd(struct kmscon_compositor *comp);
struct kmscon_output *kmscon_compositor_get_outputs(
					struct kmscon_compositor *comp);
int kmscon_compositor_refresh(struct kmscon_compositor *comp);
