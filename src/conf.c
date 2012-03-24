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
 * Implementation of the configuration parsers.
 */

#include <errno.h>
#include <getopt.h>
#include <paths.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "conf.h"
#include "log.h"

#define LOG_SUBSYSTEM "config"

struct conf_obj conf_global;

static void print_help()
{
	fprintf(stderr,
		"Usage:\n"
		"\t%1$s [options]\n"
		"\t%1$s [options] -h\n"
		"\t%1$s [options] -l /bin/sh [sh-arguments]\n"
		"\n"
		"General Options:\n"
		"\t-h, --help                    Print this help and exit\n"
		"\t-v, --verbose                 Print verbose messages\n"
		"\t    --debug                   Enable debug mode\n"
		"\t    --silent                  Suppress notices and warnings\n"
		"\t-s, --switchvt                Automatically switch to VT\n"
		"\n"
		"Login Process Options:\n"
		"\t-l, --login <login-process>   Start the given login process instead\n"
		"\t                              of the default process; all following\n"
		"\t                              arguments are passed as argv to this\n"
		"\t                              process\n"
		"\n"
		"Input Device Options:\n"
		"\t    --xkb-layout <layout>     Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant>   Set XkbVariant for input devices\n"
		"\t    --xkb-options <options>   Set XkbOptions for input devices\n",
		"kmscon");
}

int conf_parse_argv(int argc, char **argv)
{
	int show_help = 0;
	char short_options[] = ":hvsl:";
	struct option long_options[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "debug", no_argument, &conf_global.debug, 1 },
		{ "silent", no_argument, &conf_global.silent, 1 },
		{ "switchvt", no_argument, NULL, 's' },
		{ "xkb-layout", required_argument, NULL, -1 },
		{ "xkb-variant", required_argument, NULL, -2 },
		{ "xkb-options", required_argument, NULL, -3 },
		{ "login", required_argument, NULL, 'l' },
		{ NULL, 0, NULL, 0 },
	};
	int idx;
	int c;

	if (!argv || argc < 1)
		return -EINVAL;

	opterr = 0;
	while (1) {
		c = getopt_long(argc, argv, short_options,
				long_options, &idx);
		if (c < 0)
			break;
		switch (c) {
		case 0:
			break;
		case 'h':
			show_help = 1;
			break;
		case 'v':
			conf_global.verbose = 1;
			break;
		case 's':
			conf_global.switchvt = 1;
			break;
		case -1:
			conf_global.xkb_layout = optarg;
			break;
		case -2:
			conf_global.xkb_variant = optarg;
			break;
		case -3:
			conf_global.xkb_options = optarg;
			break;
		case 'l':
			conf_global.login = optarg;
			--optind;
			goto done;
		case ':':
			fprintf(stderr, "Missing argument for option -%c\n",
				optopt);
			break;
		case '?':
			if (optopt)
				fprintf(stderr, "Unknown argument -%c\n",
					optopt);
			else
				fprintf(stderr, "Unknown argument %s\n",
					argv[optind - 1]);
			break;
		}
	}

done:
	if (conf_global.login) {
		conf_global.argv = &argv[optind];
	} else {
		conf_global.login = getenv("SHELL") ? : _PATH_BSHELL;
		conf_global.argv = (char*[]){ conf_global.login, "-i", NULL };
		if (optind != argc)
			fprintf(stderr, "Unparsed remaining arguments\n");
	}

	if (conf_global.debug)
		conf_global.verbose = 1;

	if (!conf_global.xkb_layout)
		conf_global.xkb_layout = "us";
	if (!conf_global.xkb_variant)
		conf_global.xkb_variant = "";
	if (!conf_global.xkb_options)
		conf_global.xkb_options = "";

	if (show_help) {
		print_help();
		conf_global.exit = 1;
	}

	return 0;
}
