/*
 * kmscon - Generate Shader Files
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
 * Shader Generator
 * This takes as arguments shaders and creates a C-source file which
 * contains these shaders as constants.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_file(const char *path, size_t *size)
{
	FILE *ffile;
	ssize_t len;
	char *buf;

	ffile = fopen(path, "rb");
	if (!ffile) {
		fprintf(stderr, "genshader: cannot open %s: %m\n", path);
		abort();
	}

	if (fseek(ffile, 0, SEEK_END) != 0) {
		fprintf(stderr, "genshader: cannot seek %s: %m\n", path);
		abort();
	}

	len = ftell(ffile);
	if (len < 0) {
		fprintf(stderr, "genshader: cannot tell %s: %m\n", path);
		abort();
	}

	if (len < 1) {
		fprintf(stderr, "genshader: empty file %s\n", path);
		abort();
	}

	rewind(ffile);

	buf = malloc(len + 1);
	if (!buf) {
		fprintf(stderr, "genshader: memory allocation failed\n");
		abort();
	}

	if (len != fread(buf, 1, len, ffile)) {
		fprintf(stderr, "genshader: cannot read %s: %m\n", path);
		abort();
	}

	buf[len] = 0;
	*size = len;
	fclose(ffile);

	return buf;
}

static const char *get_basename(const char *path)
{
	const char *res;

	res = strrchr(path, '/');
	if (!res || !*++res)
		return path;

	return res;
}

static void write_seq(FILE *out, const char *src, size_t len)
{
	size_t i;

	for (i = 0; i < len; ++i) {
		if (src[i] == '\n') {
			fwrite("\\n\"\n\"", 5, 1, out);
		} else if (src[i] == '"') {
			fwrite("\\\"", 2, 1, out);
		} else {
			fwrite(&src[i], 1, 1, out);
		}
	}
}

static void write_name(FILE *out, const char *name)
{
	size_t i, len;

	len = strlen(name);
	for (i = 0; i < len; ++i) {
		if ((name[i] >= 'A' && name[i] <= 'Z') ||
		    (name[i] >= 'a' && name[i] <= 'z') ||
		    (name[i] >= '0' && name[i] <= '9'))
			fwrite(&name[i], 1, 1, out);
		else
			fwrite("_", 1, 1, out);
	}
}

static void write_single_file(FILE *out, const char *path)
{
	static const char c1[] = "const char *gl_";
	static const char c2[] = " = \"";
	static const char c3[] = "\";\n";
	const char *name;
	char *content;
	size_t len;

	name = get_basename(path);
	content = read_file(path, &len);

	fwrite(c1, sizeof(c1) - 1, 1, out);
	write_name(out, name);
	fwrite(c2, sizeof(c2) - 1, 1, out);
	write_seq(out, content, len);
	fwrite(c3, sizeof(c3) - 1, 1, out);

	free(content);
}

int main(int argc, char *argv[])
{
	FILE *out;
	size_t i;
	static const char c0[] = "/* This file was generated "
				 "by genshader.c */\n";

	if (argc < 2) {
		fprintf(stderr, "genshader: use ./genshader <outputfile> [<shader-files> ...]\n");
		abort();
	}

	out = fopen(argv[1], "wb");
	if (!out) {
		fprintf(stderr, "genshader: cannot open %s: %m\n", argv[1]);
		abort();
	}

	fwrite(c0, sizeof(c0) - 1, 1, out);
	for (i = 2; i < argc; ++i) {
		write_single_file(out, argv[i]);
	}

	fclose(out);

	return EXIT_SUCCESS;
}
