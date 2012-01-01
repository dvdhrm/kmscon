/*
 * test_buffer - Test Buffer
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
 * Test Buffer
 * This runs some stress tests on the buffer implementation. It can also be used
 * to run some performance benchmarks and similar.
 * This does not access the display output or perform any other _useful_ action.
 *
 * Use this as playground for new buffer features.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "console.h"
#include "log.h"
#include "unicode.h"

/* print buffer with boundary */
static void print_buf(struct kmscon_buffer *buf)
{
	unsigned int width, height, x, y;
	kmscon_symbol_t ch;

	width = kmscon_buffer_get_width(buf);
	height = kmscon_buffer_get_height(buf);

	log_info("Buffer: %ux%u\n", width, height);

	printf("x");
	for (x = 0; x < width; ++x)
		printf("x");
	printf("x\n");

	for (y = 0; y < height; ++y) {
		printf("x");
		for (x = 0; x < width; ++x) {
			ch = kmscon_buffer_read(buf, x, y);
			printf("%c", ch ? ch : ' ');
		}
		printf("x\n");
	}

	printf("x");
	for (x = 0; x < width; ++x)
		printf("x");
	printf("x\n");
}

static void test1(struct kmscon_buffer *buf)
{
	kmscon_symbol_t ch;

	log_info("Test1:\n");

	ch = kmscon_symbol_make('?');

	kmscon_buffer_write(buf, 0, 0, ch);
	kmscon_buffer_write(buf, 9, 2, ch);
	kmscon_buffer_write(buf, 4, 4, ch);
	kmscon_buffer_rotate(buf);
	print_buf(buf);
	kmscon_buffer_resize(buf, 5, 3);
	print_buf(buf);
	kmscon_buffer_resize(buf, 20, 5);
	print_buf(buf);
	kmscon_buffer_write(buf, 15, 1, ch);
	print_buf(buf);
	kmscon_buffer_rotate(buf);
	print_buf(buf);
}

static void test2()
{
	struct kmscon_symbol_table *st;
	int ret;
	kmscon_symbol_t sym, sym2, sym3, sym4;
	const uint32_t *str;
	size_t len, i;

	log_info("Test2:\n");

	ret = kmscon_symbol_table_new(&st);
	if (ret)
		return;

	sym = kmscon_symbol_make('a');
	sym2 = kmscon_symbol_append(st, sym, '^');
	sym3 = kmscon_symbol_append(st, sym2, '^');
	sym4 = kmscon_symbol_append(st, sym, '^');

	log_info("equality: %i %i %i\n", sym == sym2, sym2 == sym4,
								sym3 == sym2);

	str = kmscon_symbol_get(st, &sym3, &len);

	printf("sym3: ");
	for (i = 0; i < len; ++i)
		printf("%c", str[i]);
	printf("\n");

	kmscon_symbol_table_unref(st);
}

int main(int argc, char **argv)
{
	struct kmscon_buffer *buf;
	int ret;

	ret = kmscon_buffer_new(&buf, 10, 5);
	if (ret) {
		log_err("Cannot create buffer object\n");
		goto err_out;
	}

	test1(buf);
	test2();

	kmscon_buffer_unref(buf);
err_out:
	return abs(ret);
}
