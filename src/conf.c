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
#include <fcntl.h>
#include <getopt.h>
#include <paths.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "conf.h"
#include "log.h"

#define LOG_SUBSYSTEM "config"

struct conf_type;
struct conf_option;

struct conf_obj conf_global;
static char *def_argv[] = { NULL, "-i", NULL };

/* config option flags */
#define CONF_DONE		0x0001
#define CONF_LOCKED		0x0002

/* config type flags */
#define CONF_HAS_ARG		0x0001

struct conf_type {
	unsigned int flags;
	int (*parse) (struct conf_option *opt, bool on, const char *arg);
	void (*free) (struct conf_option *opt);
	void (*set_default) (struct conf_option *opt);
};

struct conf_option {
	unsigned int flags;
	char short_name;
	const char *long_name;
	const struct conf_type *type;
	void *mem;
	void *def;
};

static void print_help()
{
	/*
	 * Usage/Help information
	 * This should be scaled to a maximum of 80 characters per line:
	 *
	 * 80 char line:
	 *       |   10   |    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "12345678901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 * 80 char line starting with tab:
	 *       |10|    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "\t901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 */
	fprintf(stderr,
		"Usage:\n"
		"\t%1$s [options]\n"
		"\t%1$s -h [options]\n"
		"\t%1$s -l [options] -- /bin/sh [sh-arguments]\n"
		"\n"
		"You can prefix boolean options with \"no-\" to negate it. If an argument is\n"
		"given multiple times, only the last argument matters if not otherwise stated.\n"
		"\n"
		"General Options:\n"
		"\t-h, --help                  [off]   Print this help and exit\n"
		"\t-v, --verbose               [off]   Print verbose messages\n"
		"\t    --debug                 [off]   Enable debug mode\n"
		"\t    --silent                [off]   Suppress notices and warnings\n"
		"\t-s, --switchvt              [off]   Automatically switch to VT\n"
		"\t    --seat <seat-name>      [seat0] Select seat; default: seat0\n"
		"\n"
		"Terminal Options:\n"
		"\t-l, --login                 [/bin/sh]\n"
		"\t                              Start the given login process instead\n"
		"\t                              of the default process; all arguments\n"
		"\t                              following '--' will be be parsed as\n"
		"\t                              argv to this process. No more options\n"
		"\t                              after '--' will be parsed so use it at\n"
		"\t                              the end of the argument string\n"
		"\t-t, --term <TERM>           [vt220]\n"
		"\t                              Value of the TERM environment variable\n"
		"\t                              for the child process\n"
		"\n"
		"Video Options:\n"
		"\t    --fbdev                 [off]   Use fbdev instead of DRM\n"
		"\n"
		"Input Device Options:\n"
		"\t    --xkb-layout <layout>   [us]    Set XkbLayout for input devices\n"
		"\t    --xkb-variant <variant> [-]     Set XkbVariant for input devices\n"
		"\t    --xkb-options <options> [-]     Set XkbOptions for input devices\n"
		"\n"
		"Font Options:\n"
		"\t    --font-engine <engine>  [pango] Font engine\n",
		"kmscon");
	/*
	 * 80 char line:
	 *       |   10   |    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "12345678901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 * 80 char line starting with tab:
	 *       |10|    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "\t901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 */
}

static void free_value(struct conf_option *opt)
{
	if (*(void**)opt->mem != opt->def)
		free(*(void**)opt->mem);
}

static int parse_bool(struct conf_option *opt, bool on, const char *arg)
{
	*(bool*)opt->mem = on;
	return 0;
}

static void default_bool(struct conf_option *opt)
{
	*(bool*)opt->mem = (bool)opt->def;
}

static int parse_string(struct conf_option *opt, bool on, const char *arg)
{
	char *val = strdup(arg);
	if (!val)
		return -ENOMEM;

	opt->type->free(opt);
	*(void**)opt->mem = val;
	return 0;
}

static void default_string(struct conf_option *opt)
{
	*(void**)opt->mem = opt->def;
}

static const struct conf_type conf_bool = {
	.flags = 0,
	.parse = parse_bool,
	.free = NULL,
	.set_default = default_bool,
};

static const struct conf_type conf_string = {
	.flags = CONF_HAS_ARG,
	.parse = parse_string,
	.free = free_value,
	.set_default = default_string,
};

#define CONF_OPTION(_flags, _short, _long, _type, _mem, _def) \
	{ _flags, _short, "no-" _long, _type, _mem, _def }
#define CONF_OPTION_BOOL(_short, _long, _mem, _def) \
	CONF_OPTION(0, \
		    _short, \
		    _long, \
		    &conf_bool, \
		    _mem, \
		    _def)
#define CONF_OPTION_STRING(_short, _long, _mem, _def) \
	CONF_OPTION(0, \
		    _short, \
		    _long, \
		    &conf_string, \
		    _mem, \
		    _def)

struct conf_option options[] = {
	CONF_OPTION_BOOL('h', "help", &conf_global.help, false),
	CONF_OPTION_BOOL('v', "verbose", &conf_global.verbose, false),
	CONF_OPTION_BOOL(0, "debug", &conf_global.debug, false),
	CONF_OPTION_BOOL(0, "silent", &conf_global.silent, false),
	CONF_OPTION_BOOL(0, "fbdev", &conf_global.use_fbdev, false),
	CONF_OPTION_BOOL('s', "switchvt", &conf_global.switchvt, false),
	CONF_OPTION_BOOL('l', "login", &conf_global.login, false),
	CONF_OPTION_STRING('t', "term", &conf_global.term, "vt220"),
	CONF_OPTION_STRING(0, "xkb-layout", &conf_global.xkb_layout, "us"),
	CONF_OPTION_STRING(0, "xkb-variant", &conf_global.xkb_variant, ""),
	CONF_OPTION_STRING(0, "xkb-options", &conf_global.xkb_options, ""),
	CONF_OPTION_STRING(0, "seat", &conf_global.seat, "seat0"),
	CONF_OPTION_STRING(0, "font-engine", &conf_global.font_engine, "pango"),
};

/* free all memory that we allocated and reset to initial state */
void conf_free(void)
{
	unsigned int i, num;

	num = sizeof(options) / sizeof(*options);
	for (i = 0; i < num; ++i) {
		if (options[i].type->free)
			options[i].type->free(&options[i]);
	}

	memset(&conf_global, 0, sizeof(conf_global));
}

/*
 * Parse command line arguments
 * This temporarily allocates the short_options and long_options arrays so we
 * can use the getopt_long() library call. It locks all arguments after they
 * have been set so command-line options will always overwrite config-options.
 */
int conf_parse_argv(int argc, char **argv)
{
	char *short_options;
	struct option *long_options;
	struct option *opt;
	size_t len, i, pos;
	int c, ret;

	if (!argv || argc < 1)
		return -EINVAL;

	len = sizeof(options) / sizeof(*options);

	short_options = malloc(sizeof(char) * (len + 1) * 2);
	if (!short_options) {
		log_error("cannot allocate enough memory to parse command line arguments (%d): %m");
		return -ENOMEM;
	}

	long_options = malloc(sizeof(struct option) * len * 2);
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
			if (options[i].type->flags & CONF_HAS_ARG)
				short_options[pos++] = ':';
		}

		if (options[i].long_name) {
			/* skip the "no-" prefix */
			opt->name = &options[i].long_name[3];
			opt->has_arg = !!(options[i].type->flags & CONF_HAS_ARG);
			opt->flag = NULL;
			opt->val = 100000 + i;
			++opt;

			/* boolean args are also added with "no-" prefix */
			if (!(options[i].type->flags & CONF_HAS_ARG)) {
				opt->name = options[i].long_name;
				opt->has_arg = 0;
				opt->flag = NULL;
				opt->val = 200000 + i;
				++opt;
			}
		}
	}
	short_options[pos++] = 0;

	opterr = 0;
	while (1) {
		c = getopt_long(argc, argv, short_options,
				long_options, NULL);
		if (c <= 0) {
			break;
		} else if (c == ':') {
			fprintf(stderr, "Missing argument for: %s\n",
				argv[optind - 1]);
			return -EFAULT;
		} else if (c == '?') {
			if (optopt && optopt < 100000)
				fprintf(stderr, "Unknown argument: -%c\n",
					optopt);
			else if (!optopt)
				fprintf(stderr, "Unknown argument: %s\n",
					argv[optind - 1]);
			else
				fprintf(stderr, "Parameter takes no argument: %s\n",
					argv[optind - 1]);
			return -EFAULT;
		} else if (c < 100000) {
			for (i = 0; i < len; ++i) {
				if (options[i].short_name == c) {
					ret = options[i].type->parse(&options[i],
								     true,
								     optarg);
					if (ret)
						return ret;
					options[i].flags |= CONF_LOCKED;
					options[i].flags |= CONF_DONE;
					break;
				}
			}
		} else if (c < 200000) {
			i = c - 100000;
			ret = options[i].type->parse(&options[i], true, optarg);
			if (ret)
				return ret;
			options[i].flags |= CONF_LOCKED;
			options[i].flags |= CONF_DONE;
		} else {
			i = c - 200000;
			ret = options[i].type->parse(&options[i], false, NULL);
			if (ret)
				return ret;
			options[i].flags |= CONF_LOCKED;
			options[i].flags |= CONF_DONE;
		}
	}

	free(long_options);
	free(short_options);

	/* set default values if not configured */
	for (i = 0; i < len; ++i) {
		if (!(options[i].flags & CONF_DONE) &&
		    options[i].type->set_default) {
			options[i].type->set_default(&options[i]);
		}
	}

	/* parse "--login [...] -- args" arguments */
	if (conf_global.login) {
		conf_global.argv = &argv[optind];
	} else {
		def_argv[0] = getenv("SHELL") ? : _PATH_BSHELL;
		conf_global.argv = def_argv;
		if (optind != argc) {
			fprintf(stderr, "Unparsed remaining arguments\n");
			return -EFAULT;
		}
	}

	/* --debug implies --verbose */
	if (conf_global.debug)
		conf_global.verbose = 1;

	/* exit after printing --help information */
	if (conf_global.help) {
		print_help();
		conf_global.exit = 1;
	}

	return 0;
}

static int parse_kv_pair(const char *key, const char *value)
{
	unsigned int i, num;
	int ret;
	bool set;
	struct conf_option *opt;

	num = sizeof(options) / sizeof(*options);
	for (i = 0; i < num; ++i) {
		opt = &options[i];
		if (!opt->long_name)
			continue;

		if (!strcmp(key, opt->long_name))
			set = false;
		else if (!strcmp(key, &opt->long_name[3]))
			set = true;
		else
			continue;

		if (opt->type->flags & CONF_HAS_ARG && !value) {
			log_error("config option '%s' requires an argument", key);
			return -EFAULT;
		} else if (!(opt->type->flags & CONF_HAS_ARG) && value) {
			log_error("config option '%s' does not take arguments", key);
			return -EFAULT;
		}

		/* ignore if already set by command-line arguments */
		if (opt->flags & CONF_LOCKED)
			return 0;

		ret = opt->type->parse(opt, set, value);
		if (ret)
			return ret;

		opt->flags |= CONF_DONE;
		return 0;
	}

	log_error("unknown config option '%s'", key);
	return -EFAULT;
}

static void strip_spaces(char **buf)
{
	char *tail;

	while (**buf == ' ' || **buf == '\r' || **buf == '\t')
		++*buf;

	if (!**buf)
		return;

	tail = *buf;
	while (*tail)
		++tail;

	--tail;

	while (*tail == ' ' || *tail == '\r' || *tail == '\t')
		*tail-- = 0;
}

static int parse_line(char **buf, size_t *size)
{
	char *key;
	char *value = NULL;
	char *line;
	char c;
	size_t len, klen;
	int ret;

	line = *buf;
	len = *size;

	/* parse key */
	key = line;
	while (len) {
		c = *line;
		if (c == '\n' ||
		    c == '#' ||
		    c == '=')
			break;
		++line;
		--len;
	}

	if (!len) {
		*line = 0;
		goto done;
	} else if (*line == '\n') {
		*line = 0;
		goto done;
	} else if (*line == '#') {
		*line = 0;
		goto skip_comment;
	} else if (*line != '=') {
		return -EFAULT;
	}

	*line++ = 0;
	--len;

	/* parse value */
	value = line;
	while (len) {
		c = *line;
		if (c == '\n' ||
		    c == '#')
			break;
		++line;
		--len;
	}

	if (!len) {
		*line = 0;
		goto done;
	} else if (*line == '\n') {
		*line = 0;
		goto done;
	} else if (*line == '#') {
		*line = 0;
		goto skip_comment;
	} else {
		return -EFAULT;
	}

skip_comment:
	while (len) {
		c = *line;
		if (c == '\n')
			break;
		++line;
		--len;
	}

done:
	strip_spaces(&key);
	
	klen = strlen(key);
	if (klen > 0) {
		if (value)
			strip_spaces(&value);

		ret = parse_kv_pair(key, value);
		if (ret)
			return ret;
	}

	if (!len) {
		*buf = NULL;
		*size = 0;
	} else {
		*buf = ++line;
		*size = --len;
	}

	return 0;
}

static int parse_buffer(char *buf, size_t size)
{
	int ret = 0;

	while (!ret && size > 0)
		ret = parse_line(&buf, &size);

	return ret;
}

/* chunk size when reading config files */
#define CONF_BUFSIZE 4096

/* This reads the file at \path in memory and parses it as if it was given as
 * command line options. */
int conf_parse_file(const char *path)
{
	int fd, ret;
	size_t size, pos;
	char *buf, *tmp;

	if (!path)
		return -EINVAL;

	log_info("reading config file %s", path);
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) {
		log_error("cannot open %s (%d): %m", path, errno);
		return -EFAULT;
	}

	buf = NULL;
	size = 0;
	pos = 0;

	do {
		if (size - pos < CONF_BUFSIZE) {
			tmp = realloc(buf, size + CONF_BUFSIZE + 1);
			if (!tmp) {
				log_error("cannot allocate enough memory to parse config file %s (%d): %m",
					  path, errno);
				ret = -ENOMEM;
				goto out_free;
			}
			buf = tmp;
			size += CONF_BUFSIZE;
		}

		ret = read(fd, &buf[pos], CONF_BUFSIZE);
		if (ret < 0) {
			log_error("cannot read from config file %s (%d): %m",
				  path, errno);
			ret = -EFAULT;
			goto out_free;
		}
		pos += ret;
	} while (ret > 0);

	buf[pos] = 0;
	ret = parse_buffer(buf, pos);

out_free:
	free(buf);
	close(fd);
	return ret;
}

int conf_parse_all_files(void)
{
	int ret;
	const char *file, *home;
	char *path;

	ret = 0;

	file = "/etc/kmscon.conf";
	if (!access(file, F_OK)) {
		if (access(file, R_OK))
			log_warning("config file %s exists but read access was denied",
				    file);
		else
			ret = conf_parse_file(file);
	}

	if (ret)
		goto err_out;

	home = getenv("HOME");
	if (home) {
		ret = asprintf(&path, "%s/.kmscon.conf", home);
		if (ret < 0) {
			log_warning("cannot allocate enough resources to build a config-path");
			ret = -ENOMEM;
		} else {
			ret = 0;
			if (!access(path, F_OK)) {
				if (access(path, R_OK))
					log_warning("config file %s exists but read access was denied",
						    path);
				else
					ret = conf_parse_file(path);
			}
			free(path);
		}
	}

err_out:
	return ret;
}
