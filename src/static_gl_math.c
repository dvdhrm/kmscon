/*
 * kmscon - GL Math Helpers
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
 * Math Helper
 * Basic linear algebra and a matrix stack helper.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "static_gl.h"

struct gl_m4_entry {
	struct gl_m4_entry *next;
	float matrix[16];
};

struct gl_m4_stack {
	struct gl_m4_entry stack;
	struct gl_m4_entry *cache;
};

void gl_m4_identity(float *m)
{
	if (!m)
		return;

	m[0] = 1;
	m[1] = 0;
	m[2] = 0;
	m[3] = 0;

	m[4] = 0;
	m[5] = 1;
	m[6] = 0;
	m[7] = 0;

	m[8] = 0;
	m[9] = 0;
	m[10] = 1;
	m[11] = 0;

	m[12] = 0;
	m[13] = 0;
	m[14] = 0;
	m[15] = 1;
}

void gl_m4_copy(float *dest, const float *src)
{
	if (!dest || !src)
		return;

	dest[0] = src[0];
	dest[1] = src[1];
	dest[2] = src[2];
	dest[3] = src[3];

	dest[4] = src[4];
	dest[5] = src[5];
	dest[6] = src[6];
	dest[7] = src[7];

	dest[8] = src[8];
	dest[9] = src[9];
	dest[10] = src[10];
	dest[11] = src[11];

	dest[12] = src[12];
	dest[13] = src[13];
	dest[14] = src[14];
	dest[15] = src[15];
}

/* Matrix Multiplication
 * This is the simplest algorithm to multiply two matrices. It computes:
 *    dest = n * m
 * That is, n is left side and m is right side.
 *
 * Matrix-multiplication is heavy, avoid it if possible.
 */
void gl_m4_mult_dest(float *dest, const float *n, const float *m)
{
	unsigned int row, col, j;

	if (!dest || !n || !m)
		return;

	for (row = 0; row < 4; ++row) {
		for (col = 0; col < 4; ++col) {
			dest[row * 4 + col] = 0;
			for (j = 0; j < 4; ++j)
				dest[row * 4 + col] +=
					n[row * 4 + j] * m[j * 4 + col];
		}
	}
}

/* this computes n = n * m */
void gl_m4_mult(float *n, const float *m)
{
	float tmp[16];

	if (!n || !m)
		return;

	gl_m4_mult_dest(tmp, n, m);
	gl_m4_copy(n, tmp);
}

void gl_m4_translate(float *m, float x, float y, float z)
{
	float trans[16] = { 1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z, 0, 0, 0, 1 };

	if (!m)
		return;

	gl_m4_mult(m, trans);
}

void gl_m4_scale(float *m, float x, float y, float z)
{
	float scale[16] = { x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1 };

	if (!m)
		return;

	gl_m4_mult(m, scale);
}

void gl_m4_transpose_dest(float *dest, const float *src)
{
	if (!dest || !src)
		return;

	dest[0] = src[0];
	dest[5] = src[5];
	dest[10] = src[10];
	dest[15] = src[15];

	dest[1] = src[4];
	dest[4] = src[1];

	dest[8] = src[2];
	dest[2] = src[8];

	dest[3] = src[12];
	dest[12] = src[3];

	dest[7] = src[13];
	dest[13] = src[7];

	dest[11] = src[14];
	dest[14] = src[11];

	dest[6] = src[9];
	dest[9] = src[6];
}

void gl_m4_transpose(float *m)
{
	float tmp;

	if (!m)
		return;

	tmp = m[1];
	m[1] = m[4];
	m[4] = tmp;

	tmp = m[8];
	m[8] = m[2];
	m[2] = tmp;

	tmp = m[3];
	m[3] = m[12];
	m[12] = tmp;

	tmp = m[7];
	m[7] = m[13];
	m[13] = tmp;

	tmp = m[11];
	m[11] = m[14];
	m[14] = tmp;

	tmp = m[6];
	m[6] = m[9];
	m[9] = tmp;
}

int gl_m4_stack_new(struct gl_m4_stack **out)
{
	struct gl_m4_stack *stack;

	if (!out)
		return -EINVAL;

	stack = malloc(sizeof(*stack));
	if (!stack)
		return -ENOMEM;

	memset(stack, 0, sizeof(*stack));
	gl_m4_identity(stack->stack.matrix);

	*out = stack;
	return 0;
}

void gl_m4_stack_free(struct gl_m4_stack *stack)
{
	struct gl_m4_entry *tmp;

	if (!stack)
		return;

	while (stack->stack.next) {
		tmp = stack->stack.next;
		stack->stack.next = tmp->next;
		free(tmp);
	}

	while (stack->cache) {
		tmp = stack->cache;
		stack->cache = tmp->next;
		free(tmp);
	}

	free(stack);
}

float *gl_m4_stack_push(struct gl_m4_stack *stack)
{
	struct gl_m4_entry *entry;

	if (stack->cache) {
		entry = stack->cache;
		stack->cache = entry->next;
	} else {
		entry = malloc(sizeof(*entry));
		if (!entry)
			return NULL;
	}

	gl_m4_copy(entry->matrix, stack->stack.matrix);
	entry->next = stack->stack.next;
	stack->stack.next = entry;

	return stack->stack.matrix;
}

float *gl_m4_stack_pop(struct gl_m4_stack *stack)
{
	struct gl_m4_entry *entry;

	if (!stack)
		return NULL;

	entry = stack->stack.next;
	if (!entry) {
		gl_m4_identity(stack->stack.matrix);
		return stack->stack.matrix;
	}

	stack->stack.next = entry->next;
	entry->next = stack->cache;
	stack->cache = entry;

	gl_m4_copy(stack->stack.matrix, entry->matrix);

	return stack->stack.matrix;
}

float *gl_m4_stack_tip(struct gl_m4_stack *stack)
{
	if (!stack)
		return NULL;

	return stack->stack.matrix;
}
