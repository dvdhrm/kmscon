/*
 * kmscon - OpenGL Helpers
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
 * OpenGL Helpers
 * This file provides several helper functions that are commonly used when
 * working with OpenGL.
 */

#ifndef GL_H_INCLUDED
#define GL_H_INCLUDED

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include "shl_llog.h"

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
 * Shader Helpers
 * These helpers load, compile and link shaders and allow easy attribute/uniform
 * access.
 */

struct gl_shader;

int gl_shader_new(struct gl_shader **out, const char *vert, const char *frag,
		  char **attr, size_t attr_count, llog_submit_t llog,
		  void *llog_data);
void gl_shader_ref(struct gl_shader *shader);
void gl_shader_unref(struct gl_shader *shader);
GLuint gl_shader_get_uniform(struct gl_shader *shader, const char *name);
void gl_shader_use(struct gl_shader *shader);

void gl_tex_new(GLuint *tex, size_t num);
void gl_tex_free(GLuint *tex, size_t num);
void gl_tex_load(GLuint tex, unsigned int width, unsigned int stride,
		 unsigned int height, uint8_t *buf);

void gl_clear_error();
bool gl_has_error(struct gl_shader *shader);
const char *gl_err_to_str(GLenum err);

#endif /* GL_H_INCLUDED */
