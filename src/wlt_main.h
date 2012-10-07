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
#include "conf.h"

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

	/* custom login process */
	bool login;
	/* argv for login process */
	char **argv;
	/* TERM value */
	char *term;
	/* color palette */
	char *palette;
	/* terminal scroll-back buffer size */
	unsigned int sb_size;

	/* scroll-up grab */
	struct conf_grab *grab_scroll_up;
	/* scroll-down grab */
	struct conf_grab *grab_scroll_down;
	/* page-up grab */
	struct conf_grab *grab_page_up;
	/* page-down grab */
	struct conf_grab *grab_page_down;
	/* fullscreen grab */
	struct conf_grab *grab_fullscreen;
	/* font-zoom-in grab */
	struct conf_grab *grab_zoom_in;
	/* font-zoom-out grab */
	struct conf_grab *grab_zoom_out;
	/* copy grab */
	struct conf_grab *grab_copy;
	/* paste grab */
	struct conf_grab *grab_paste;

	/* font engine */
	char *font_engine;
	/* font size */
	unsigned int font_size;
	/* font name */
	char *font_name;
	/* font ppi (overrides per monitor PPI) */
	unsigned int font_ppi;

	/* xkb key repeat delay */
	unsigned int xkb_repeat_delay;
	/* xkb key repeat rate */
	unsigned int xkb_repeat_rate;
};

extern struct wlt_conf_t wlt_conf;

#endif /* WLT_MAIN_H */
