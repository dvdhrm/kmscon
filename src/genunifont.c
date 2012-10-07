/*
 * kmscon - Generate Unifont data files
 *
 * Copyright (c) 2012 Ted Kotz <ted@kotz.us>
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Unifont Generator
 * This converts the hex-encoded Unifont data into a C-array that is used by the
 * unifont-font-renderer.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DATA_SIZE 512

struct unifont_glyph {
	struct unifont_glyph *next;
	uint32_t codepoint;
	uint8_t len;
	char data[MAX_DATA_SIZE];
};

static uint8_t hex_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	else if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	else if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	fprintf(stderr, "genunifont: invalid hex-code %c\n", c);
	return 0;
}

static void print_data_row(FILE *out, char c)
{
	static const char *line_map[16] = {
		"0x00, 0x00, 0x00, 0x00,",
		"0x00, 0x00, 0x00, 0xff,",
		"0x00, 0x00, 0xff, 0x00,",
		"0x00, 0x00, 0xff, 0xff,",
		"0x00, 0xff, 0x00, 0x00,",
		"0x00, 0xff, 0x00, 0xff,",
		"0x00, 0xff, 0xff, 0x00,",
		"0x00, 0xff, 0xff, 0xff,",
		"0xff, 0x00, 0x00, 0x00,",
		"0xff, 0x00, 0x00, 0xff,",
		"0xff, 0x00, 0xff, 0x00,",
		"0xff, 0x00, 0xff, 0xff,",
		"0xff, 0xff, 0x00, 0x00,",
		"0xff, 0xff, 0x00, 0xff,",
		"0xff, 0xff, 0xff, 0x00,",
		"0xff, 0xff, 0xff, 0xff,",
	};
	uint8_t idx;

	idx = hex_val(c);
	if (idx < 16) {
		fputs(line_map[idx], out);
	} else {
		fprintf(stderr, "genunifont: invalid value %c\n", c);
		fputs(line_map[0], out);
	}
}

static void print_unifont_glyph(FILE *out, const struct unifont_glyph *g)
{
	int width;
	size_t i;

	switch (g->len) {
	case 64:
		width = 4;
		break;
	case 32:
		width = 2;
		break;
	default:
		fprintf(stderr, "genunifont: invalid data size");
		return;
	}

	fprintf(out, "\t{ /* %d 0x%x */\n"
		     "\t\t.buf = {\n"
		     "\t\t\t.width = %d,\n"
		     "\t\t\t.height = 16,\n"
		     "\t\t\t.stride = %d,\n"
		     "\t\t\t.format = UTERM_FORMAT_GREY,\n"
		     "\t\t\t.data = (uint8_t[]){\n",
		     g->codepoint,  g->codepoint,
		     width * 4, width * 4);

	for (i = 0; i < g->len; ++i) {
		fprintf(out, "\t\t\t\t");
		print_data_row(out, g->data[i]);
		fprintf(out, "\n");
	}

	fprintf(out, "\t\t\t},\n\t\t},\n\t},\n");
}

static int build_unifont_glyph(struct unifont_glyph *g, const char *buf)
{
	int val;

	val = 0;
	while (*buf && *buf != ':') {
		val <<= 4;
		val += hex_val(*buf++);
	}

	if (*buf++ != ':') {
		fprintf(stderr, "genunifont: invalid file format\n");
		return -EFAULT;
	}

	g->codepoint = val;
	g->len = 0;
	while (*buf && *buf != '\n' && g->len < MAX_DATA_SIZE) {
		g->data[g->len] = *buf++;
		++g->len;
	}

	return 0;
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

static int parse_single_file(FILE *out, FILE *in, const char *varname)
{
	struct unifont_glyph replacement = {
		.codepoint = 0,
		.len = 32,
		.data = "0000007E665A5A7A76767E76767E0000"
	};
	static const char c0[] = "const struct kmscon_glyph kmscon_";
	static const char c1[] = "_glyphs[] = {\n";
	static const char c2[] = "};\n\nconst size_t kmscon_";
	static const char c3[] = "_len =\n\tsizeof(kmscon_";
	static const char c4[] = "_glyphs) /\n\tsizeof(*kmscon_";
	static const char c5[] = "_glyphs);\n";
	char buf[MAX_DATA_SIZE];
	struct unifont_glyph *g, *iter, *list, *last;
	int ret, num;
	unsigned long status_max, status_cur;
	unsigned long perc_prev, perc_now;

	if (fseek(in, 0, SEEK_END) != 0) {
		fprintf(stderr, "genunifont: cannot seek: %m\n");
		return -EFAULT;
	}

	status_max = ftell(in);
	if (status_max < 0) {
		fprintf(stderr, "genunifont: cannot ftell: %m\n");
		return -EFAULT;
	}

	if (status_max < 1) {
		fprintf(stderr, "genunifont: empty file\n");
		return -EFAULT;
	}

	rewind(in);
	list = NULL;
	last = NULL;
	status_cur = 0;
	perc_prev = 0;
	perc_now = 0;

	fprintf(stderr, "Finished: %3ld%%", perc_now);

	while (fgets(buf, sizeof(buf) - 1, in) != NULL) {
		perc_now = status_cur * 100 / status_max;
		if (perc_now > perc_prev) {
			perc_prev = perc_now;
			fprintf(stderr, "\b\b\b\b%3ld%%", perc_now);
		}
		status_cur += strlen(buf);

		if (buf[0] == '#')
			continue;

		g = malloc(sizeof(*g));
		if (!g) {
			fprintf(stderr, "genunifont: out of memory\n");
			return -ENOMEM;
		}
		memset(g, 0, sizeof(*g));

		ret = build_unifont_glyph(g, buf);
		if (ret) {
			free(g);
			continue;
		}

		if (!list || list->codepoint > g->codepoint) {
			g->next = list;
			list = g;
			if (!last)
				last = g;
		} else if (list->codepoint == g->codepoint) {
			fprintf(stderr, "glyph %d used twice\n",
				g->codepoint);
			free(g);
		} else {
			if (last->codepoint < g->codepoint) {
				iter = last;
			} else {
				iter = list;
				while (iter->next) {
					if (iter->next->codepoint >= g->codepoint)
						break;
					iter = iter->next;
				}
			}

			if (iter->next) {
				if (iter->next->codepoint == g->codepoint) {
					fprintf(stderr, "glyph %d used twice\n",
						g->codepoint);
					free(g);
				} else {
					g->next = iter->next;
					iter->next = g;
				}
			} else {
				iter->next = g;
				last = g;
			}
		}
	}

	fprintf(stderr, "\n");

	fwrite(c0, sizeof(c0) - 1, 1, out);
	write_name(out, varname);
	fwrite(c1, sizeof(c1) - 1, 1, out);

	num = 0;
	while (list) {
		iter = list;
		list = iter->next;

		while (num++ < iter->codepoint)
			print_unifont_glyph(out, &replacement);

		print_unifont_glyph(out, iter);
		free(iter);
	}

	fwrite(c2, sizeof(c2) - 1, 1, out);
	write_name(out, varname);
	fwrite(c3, sizeof(c3) - 1, 1, out);
	write_name(out, varname);
	fwrite(c4, sizeof(c4) - 1, 1, out);
	write_name(out, varname);
	fwrite(c5, sizeof(c5) - 1, 1, out);

	return 0;
}

static const char *get_basename(const char *path)
{
	const char *res;

	res = strrchr(path, '/');
	if (!res || !*++res)
		return path;

	return res;
}

int main(int argc, char **argv)
{
	FILE *out, *in;
	size_t i;
	static const char c0[] = "/* This file was generated "
				 "by genunifont.c */\n\n"
				 "#include <stdint.h>\n"
				 "#include <stdlib.h>\n"
				 "#include \"text.h\"\n\n";
	int ret = EXIT_FAILURE;

	if (argc < 2) {
		fprintf(stderr, "genunifont: use ./genunifont <outputfile> [<inputfiles> ...]\n");
		goto err_out;
	}

	out = fopen(argv[1], "wb");
	if (!out) {
		fprintf(stderr, "genunifont: cannot open output %s: %m\n",
			argv[1]);
		goto err_out;
	}

	fwrite(c0, sizeof(c0) - 1, 1, out);
	for (i = 2; i < argc; ++i) {
		fprintf(stderr, "genunifont: parsing input %s\n", argv[i]);
		in = fopen(argv[i], "rb");
		if (!in) {
			fprintf(stderr, "genunifont: cannot open %s: %m\n",
				argv[i]);
			continue;
		}
		ret = parse_single_file(out, in, get_basename(argv[i]));
		if (ret)
			fprintf(stderr, "genunifont: parsing input %s failed",
				argv[i]);
		fclose(in);
	}

	ret = EXIT_SUCCESS;

	fclose(out);
err_out:
	return ret;
}
