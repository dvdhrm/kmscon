/*
 * kmscon - Generate Unifont data files
 *
 * Copyright (c) 2012 Ted Kotz <ted@kotz.us>
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
 * Unifont Generator
 * This converts the hex-encoded Unifont data into a C-array that is used by the
 * unifont-font-renderer.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DATA_SIZE 255

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

static void print_unifont_glyph(FILE *out, const struct unifont_glyph *g)
{
	size_t i;
	uint8_t val;

	switch (g->len) {
	case 32:
	case 64:
		break;
	default:
		fprintf(stderr, "genunifont: invalid data size %d for %x",
			g->len, g->codepoint);
		return;
	}

	fprintf(out, "%c", g->len / 2);
	for (i = 0; i < g->len; i += 2) {
		val = hex_val(g->data[i]) << 4;
		val |= hex_val(g->data[i + 1]);
		fprintf(out, "%c", val);
	}
	for ( ; i < 64; i += 2)
		fprintf(out, "%c", 0);
}

static int build_unifont_glyph(struct unifont_glyph *g, const char *buf)
{
	int val;
	const char *orig = buf;

	val = 0;
	while (*buf && *buf != ':') {
		val <<= 4;
		val += hex_val(*buf++);
	}

	if (*buf++ != ':') {
		fprintf(stderr, "genunifont: invalid file format: %s\n", orig);
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

static int parse_single_file(FILE *out, FILE *in)
{
	static const struct unifont_glyph replacement = {
		.codepoint = 0,
		.len = 32,
		.data = "0000007E665A5A7A76767E76767E0000"
	};
	char buf[MAX_DATA_SIZE];
	struct unifont_glyph *g, **iter, *list, *last;
	int ret, num;
	long status_max, status_cur;
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
		/* print status update in percent */
		perc_now = status_cur * 100 / status_max;
		if (perc_now > perc_prev) {
			perc_prev = perc_now;
			fprintf(stderr, "\b\b\b\b%3ld%%", perc_now);
			fflush(stderr);
		}
		status_cur += strlen(buf);

		/* ignore comments */
		if (buf[0] == '#')
			continue;

		/* allocate new glyph */
		g = malloc(sizeof(*g));
		if (!g) {
			fprintf(stderr, "genunifont: out of memory\n");
			return -ENOMEM;
		}
		memset(g, 0, sizeof(*g));

		/* read glyph data */
		ret = build_unifont_glyph(g, buf);
		if (ret) {
			free(g);
			return ret;
		}

		/* find glyph position */
		if (last && last->codepoint < g->codepoint) {
			iter = &last->next;
		} else {
			iter = &list;
			while (*iter && (*iter)->codepoint < g->codepoint)
				iter = &(*iter)->next;

			if (*iter && (*iter)->codepoint == g->codepoint) {
				fprintf(stderr, "glyph %d used twice\n",
					g->codepoint);
				free(g);
				return -EFAULT;
			}
		}

		/* insert glyph into single-linked list */
		g->next = *iter;
		if (!*iter)
			last = g;
		*iter = g;
	}

	fprintf(stderr, "\b\b\b\b%3d%%\n", 100);

	/* print all glyph-data to output file */
	num = 0;
	while (list) {
		g = list;
		list = g->next;

		/* print replacements if glyphs are missing */
		while (num++ < g->codepoint)
			print_unifont_glyph(out, &replacement);

		print_unifont_glyph(out, g);
		free(g);
	}

	return 0;
}

int main(int argc, char **argv)
{
	FILE *out, *in;
	int ret;

	if (argc < 3) {
		fprintf(stderr, "genunifont: use ./genunifont <outputfile> <inputfiles>\n");
		ret = EXIT_FAILURE;
		goto err_out;
	}

	out = fopen(argv[1], "wb");
	if (!out) {
		fprintf(stderr, "genunifont: cannot open output %s: %m\n",
			argv[1]);
		ret = EXIT_FAILURE;
		goto err_out;
	}

	fprintf(stderr, "genunifont: parsing input %s\n", argv[2]);
	in = fopen(argv[2], "rb");
	if (!in) {
		fprintf(stderr, "genunifont: cannot open %s: %m\n",
			argv[2]);
		ret = EXIT_FAILURE;
	} else {
		ret = parse_single_file(out, in);
		if (ret) {
			fprintf(stderr, "genunifont: parsing input %s failed",
				argv[2]);
			ret = EXIT_FAILURE;
		} else {
			ret = EXIT_SUCCESS;
		}
		fclose(in);
	}


	fclose(out);
err_out:
	return ret;
}
