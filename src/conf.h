/*
 * Configuration Parsers
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
 * This provides generic command-line argument and configuration file parsers
 * which can be used by different applications that are part of this
 * distribution.
 */

#ifndef CONFIG_CONFIG_H
#define CONFIG_CONFIG_H

#include <stdbool.h>
#include <stdlib.h>

/* configuration parser */

struct conf_type;
struct conf_option;

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
	int (*aftercheck) (struct conf_option *opt, int argc,
			   char **argv, int idx);
	void *mem;
	void *def;
};

#define CONF_OPTION(_flags, _short, _long, _type, _aftercheck, _mem, _def) \
	{ _flags, _short, "no-" _long, _type, _aftercheck, _mem, _def }
#define CONF_OPTION_BOOL(_short, _long, _aftercheck, _mem, _def) \
	CONF_OPTION(0, \
		    _short, \
		    _long, \
		    &conf_bool, \
		    _aftercheck, \
		    _mem, \
		    _def)
#define CONF_OPTION_STRING(_short, _long, _aftercheck, _mem, _def) \
	CONF_OPTION(0, \
		    _short, \
		    _long, \
		    &conf_string, \
		    _aftercheck, \
		    _mem, \
		    _def)

void conf_free_value(struct conf_option *opt);
int conf_parse_bool(struct conf_option *opt, bool on, const char *arg);
void conf_default_bool(struct conf_option *opt);
int conf_parse_string(struct conf_option *opt, bool on, const char *arg);
void conf_default_string(struct conf_option *opt);

extern const struct conf_type conf_bool;
extern const struct conf_type conf_string;

void conf_free(struct conf_option *opts, size_t len);
int conf_parse_argv(struct conf_option *opts, size_t len,
		    int argc, char **argv);
int conf_parse_file(struct conf_option *opts, size_t len, const char *path);
int conf_parse_all_files(struct conf_option *opts, size_t len);

#endif /* CONFIG_CONFIG_H */
