/*
 * wlt - Main Header
 *
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
 * Wayland Terminal global information and state
 */

#ifndef WLT_MAIN_H
#define WLT_MAIN_H

#include <stdbool.h>
#include <stdlib.h>

struct wlt_conf_t {
	/* show help/usage information */
	bool help;
	/* exit application after parsing options */
	bool exit;
	/* enable debug messages */
	bool debug;
	/* enable verbose info messages */
	bool verbose;
	/* disable notices and warnings */
	bool silent;

	/* font engine */
	char *font_engine;
	/* font size */
	unsigned int font_size;
	/* font name */
	char *font_name;
	/* font ppi (overrides per monitor PPI) */
	unsigned int font_ppi;
};

extern struct wlt_conf_t wlt_conf;

#endif /* WLT_MAIN_H */
