/*
 * Main App
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
 * This includes global data for the whole kmscon application. For instance,
 * global parameters can be accessed via this header.
 */

#ifndef KMSCON_MAIN_H
#define KMSCON_MAIN_H

#include <stdbool.h>
#include <stdlib.h>
#include "uterm.h"

struct kmscon_conf_t {
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
	/* VT number to run on on seat0 */
	int vt;
	/* enter new VT directly */
	bool switchvt;
	/* use framebuffers instead of DRM */
	bool use_fbdev;
	/* use dumb DRM devices */
	bool dumb;
	/* render engine */
	char *render_engine;

	/* input KBD layout */
	char *xkb_layout;
	char *xkb_variant;
	char *xkb_options;

	/* TERM value */
	char *term;
	/* custom login process */
	bool login;
	/* argv for login process */
	char **argv;
	/* terminal scroll-back buffer size */
	unsigned int sb_size;
	/* scroll-up grab */
	struct uterm_input_grab *grab_scroll_up;
	/* scroll-down grab */
	struct uterm_input_grab *grab_scroll_down;
	/* page-up grab */
	struct uterm_input_grab *grab_page_up;
	/* page-down grab */
	struct uterm_input_grab *grab_page_down;

	/* seats */
	char **seats;
	bool all_seats;

	/* font engine */
	char *font_engine;
	/* font size */
	unsigned int font_size;
	/* font name */
	char *font_name;
	/* font ppi (overrides per monitor PPI) */
	unsigned int font_ppi;

	/* color palette */
	char *palette;

	/* frame-rate limit */
	unsigned int fps;
	/* print rendering engine timing information */
	bool render_timing;
};

extern struct kmscon_conf_t kmscon_conf;

#endif /* KMSCON_MAIN_H */
