/*
 * GL - Graphics Layer
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Graphics Layer
 * This provides lots of helpers to work with OpenGL APIs. This includes
 * math-helpers, basic shaders and a texture-API. If working with this API,
 * there must always be a valid OpenGL-context!
 */

#ifndef GL_GL_H
#define GL_GL_H

#include <stdbool.h>
#include <stdlib.h>
#include "uterm.h"

/* miscellaneous */
void gl_clear_error();
bool gl_has_error();
void gl_viewport(struct uterm_screen *screen);

/*
 * Math Helpers
 * The gl_m4 type is a 4x4 matrix of floats. The gl_m4_stack is a stack of m4
 * matrices where you can only access the top-most member.
 */

struct gl_m4_stack;

void gl_m4_identity(float *m);
void gl_m4_copy(float *dest, const float *src);
void gl_m4_mult_dest(float *dest, const float *n, const float *m);
void gl_m4_mult(float *n, const float *m);
void gl_m4_translate(float *m, float x, float y, float z);
void gl_m4_scale(float *m, float x, float y, float z);
void gl_m4_transpose_dest(float *dest, const float *src);
void gl_m4_transpose(float *m);

int gl_m4_stack_new(struct gl_m4_stack **out);
void gl_m4_stack_free(struct gl_m4_stack *stack);
float *gl_m4_stack_push(struct gl_m4_stack *stack);
float *gl_m4_stack_pop(struct gl_m4_stack *stack);
float *gl_m4_stack_tip(struct gl_m4_stack *stack);

/*
 * Texture API
 * This allows to create new textures which can then be used to draw images with
 * the shader API.
 */

unsigned int gl_tex_new();
void gl_tex_free(unsigned int tex);
void gl_tex_load(unsigned int tex, unsigned int width, unsigned int stride,
			unsigned int height, void *buf);

/*
 * Shader API
 */

struct gl_shader;

int gl_shader_new(struct gl_shader **out);
void gl_shader_ref(struct gl_shader *shader);
void gl_shader_unref(struct gl_shader *shader);

void gl_shader_draw_def(struct gl_shader *shader, float *vertices,
			float *colors, size_t num);
void gl_shader_draw_tex(struct gl_shader *shader, const float *vertices,
			const float *texcoords, size_t num,
			unsigned int tex, const float *m);

#endif /* GL_GL_H */
