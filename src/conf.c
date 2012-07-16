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
static char *def_argv[] = { NULL, "-i", NULL };

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
		"\t    --seat <seat-name>        Select seat; default: seat0\n"
		"\n"
		"Terminal Options:\n"
		"\t-l, --login <login-process>   Start the given login process instead\n"
		"\t                              of the default process; all following\n"
		"\t                              arguments are passed as argv to this\n"
		"\t                              process\n"
		"\t-t, --term <TERM>             Value of the TERM environment variable\n"
		"\t                              for the child process\n"
		"\n"
		"Video Options:\n"
		"\t    --fbdev                   Use fbdev instead of DRM\n"
		"\n"
		"Input Device Options:\n"
		"\t    --xkb-layout <layout>     Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant>   Set XkbVariant for input devices\n"
		"\t    --xkb-options <options>   Set XkbOptions for input devices\n",
		"kmscon");
}

static bool parse_help(const char *arg)
{
	conf_global.help = 1;
	return false;
}

static bool parse_verbose(const char *arg)
{
	conf_global.verbose = 1;
	return false;
}

static bool parse_debug(const char *arg)
{
	conf_global.debug = 1;
	return false;
}

static bool parse_silent(const char *arg)
{
	conf_global.silent = 1;
	return false;
}

static bool parse_fbdev(const char *arg)
{
	conf_global.use_fbdev = 1;
	return false;
}

static bool parse_switchvt(const char *arg)
{
	conf_global.switchvt = 1;
	return false;
}

static bool parse_login(const char *arg)
{
	conf_global.login = (char*)arg;
	return true;
}

static bool parse_term(const char *arg)
{
	conf_global.term = arg;
	return false;
}

static bool parse_xkb_layout(const char *arg)
{
	conf_global.xkb_layout = arg;
	return false;
}

static bool parse_xkb_variant(const char *arg)
{
	conf_global.xkb_variant = arg;
	return false;
}

static bool parse_xkb_options(const char *arg)
{
	conf_global.xkb_options = arg;
	return false;
}

static bool parse_seat(const char *arg)
{
	conf_global.seat = arg;
	return false;
}

struct conf_option {
	char short_name;
	const char *long_name;
	int needs_arg; // no_argument, required_argument or optional_argument
	bool (*parse) (const char *arg);
} options[] = {
	{ 'h', "help", no_argument, parse_help },
	{ 'v', "verbose", no_argument, parse_verbose },
	{ 0, "debug", no_argument, parse_debug },
	{ 0, "silent", no_argument, parse_silent },
	{ 0, "fbdev", no_argument, parse_fbdev },
	{ 's', "switchvt", no_argument, parse_switchvt },
	{ 'l', "login", required_argument, parse_login },
	{ 't', "term", required_argument, parse_term },
	{ 0, "xkb-layout", required_argument, parse_xkb_layout },
	{ 0, "xkb-variant", required_argument, parse_xkb_variant },
	{ 0, "xkb-options", required_argument, parse_xkb_options },
	{ 0, "seat", required_argument, parse_seat },
};

int conf_parse_argv(int argc, char **argv)
{
	char *short_options;
	struct option *long_options;
	struct option *opt;
	size_t len, i, pos;
	int c;

	if (!argv || argc < 1)
		return -EINVAL;

	len = sizeof(options) / sizeof(struct conf_option);

	short_options = malloc(sizeof(char) * (len + 1) * 2);
	if (!short_options) {
		log_error("cannot allocate enough memory to parse command line arguments (%d): %m");
		return -ENOMEM;
	}

	long_options = malloc(sizeof(struct option) * len);
	if (!long_options) {
		log_error("cannot allocate enough memory to parse command line arguments (%d): %m");
		free(short_options);
		return -ENOMEM;
	}

	pos = 0;
	short_options[pos++] = ':';
	opt = long_options;
	for (i = 0; i < len; ++i) {
		if (options[i].short_name) {
			short_options[pos++] = options[i].short_name;
			if (options[i].needs_arg)
				short_options[pos++] = ':';
		}

		if (options[i].long_name) {
			opt->name = options[i].long_name;
			opt->has_arg = options[i].needs_arg;
			opt->flag = NULL;
			opt->val = 1000 + i;
			++opt;
		}
	}
	short_options[pos++] = 0;

	opterr = 0;
	while (1) {
		c = getopt_long(argc, argv, short_options,
				long_options, NULL);
		if (c <= 0)
			break;

		if (c == ':') {
			fprintf(stderr, "Missing argument for option -%c\n",
				optopt);
		} else if (c == '?') {
			if (optopt)
				fprintf(stderr, "Unknown argument -%c\n",
					optopt);
			else
				fprintf(stderr, "Unknown argument %s\n",
					argv[optind - 1]);
		} else if (c < 1000) {
			for (i = 0; i < len; ++i) {
				if (options[i].short_name == c) {
					if (options[i].parse(optarg))
						goto done;
					break;
				}
			}
		} else {
			if (options[c - 1000].parse(optarg))
				goto done;
		}
	}

done:
	free(long_options);
	free(short_options);

	if (conf_global.login) {
		conf_global.argv = &argv[optind];
	} else {
		conf_global.login = getenv("SHELL") ? : _PATH_BSHELL;
		def_argv[0] = conf_global.login;
		conf_global.argv = def_argv;
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

	if (!conf_global.term)
		conf_global.term = "vt220";

	if (!conf_global.seat)
		conf_global.seat = "seat0";

	if (conf_global.help) {
		print_help();
		conf_global.exit = 1;
	}

	return 0;
}
