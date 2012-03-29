/*
 * App Configuration
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
 * Configuration
 * This file provides a static global configuration. Several functions are
 * available which parse external data like command-line options or
 * configuration-files into the global configuration "conf_global".
 * All subsystems can add their parsers and values here so the single
 * configuration object will be sufficient to configure the whole application.
 *
 * The data is static and should be considered read-only. Only on startup the
 * configuration is written, all later functions should only read it. This is no
 * database so there is no reason to write the config again.
 */

#ifndef CONFIG_CONFIG_H
#define CONFIG_CONFIG_H

#include <stdlib.h>
#include <unistd.h>

struct conf_obj {
	/* exit application after parsing options */
	int exit;
	/* enable debug messages */
	int debug;
	/* enable verbose info messages */
	int verbose;
	/* disable notices and warnings */
	int silent;
	/* enter new VT directly */
	int switchvt;

	/* input KBD layout */
	const char *xkb_layout;
	const char *xkb_variant;
	const char *xkb_options;

	/* TERM value */
	const char *term;
	/* login process */
	char *login;
	/* argv for login process */
	char **argv;

	/* seat name */
	const char *seat;
};

extern struct conf_obj conf_global;

int conf_parse_argv(int argc, char **argv);

#endif /* CONFIG_CONFIG_H */
