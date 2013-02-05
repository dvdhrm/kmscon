/*
 * Configuration Parsers
 *
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
 * Configuration
 * This provides generic command-line argument and configuration file parsers
 * which can be used by different applications that are part of this
 * distribution. It provides most basic types but can be extended on the fly
 * with more advanced types.
 */

#ifndef CONF_CONF_H
#define CONF_CONF_H

#include <stdbool.h>
#include <stdlib.h>
#include "shl_misc.h"

struct conf_type;
struct conf_option;
struct conf_ctx;

/* Conf Types */

#define CONF_HAS_ARG		0x0001

struct conf_type {
	unsigned int flags;
	void (*set_default) (struct conf_option *opt);
	void (*free) (struct conf_option *opt);
	int (*parse) (struct conf_option *opt, bool on, const char *arg);
	int (*copy) (struct conf_option *opt, const struct conf_option *src);
};

/*
 * Bool: expects "mem" to point to a "bool"
 * Initial state is "false".
 */

extern const struct conf_type conf_bool;

/*
 * Int: expects "mem" to point to an "int"
 * Initial state is "0".
 */

extern const struct conf_type conf_int;

/*
 * Uint: expects "mem" to point to an "uint"
 * Initial state is "0"
 */

extern const struct conf_type conf_uint;

/*
 * String: expects "mem" to point to an "char*"
 * Initial state is NULL. Memory is allocated by the parser and a string is
 * always zero-terminated.
 */

extern const struct conf_type conf_string;

/*
 * Stringlist: expects "mem" to point to an "char**"
 * Initial state is NULL. The list is NULL-terminated and each entry is a
 * zero-terminated string. Memory is allocated by the parser.
 */

extern const struct conf_type conf_string_list;

/*
 * Grabs: expects "mem" to point to an "struct conf_grab*"
 * Initial state is NULL. See below for the type definition. The memory for the
 * type is allocated by the parser.
 * Two small helpers are available to ease the use.
 */

extern const struct conf_type conf_grab;

struct conf_grab {
	unsigned int num;
	unsigned int *mods;
	unsigned int *num_syms;
	uint32_t **keysyms;
};

static inline bool conf_grab_matches(const struct conf_grab *grab,
				     unsigned int ev_mods,
				     unsigned int ev_num_syms,
				     const uint32_t *ev_syms)
{
	return shl_grab_has_match(ev_mods, ev_num_syms, ev_syms,
				  grab->num, grab->mods, grab->num_syms,
				  grab->keysyms);
}

#define CONF_SINGLE_GRAB(_mods, _sym) { \
		.num = 1, \
		.mods = (unsigned int[]) { (_mods) }, \
		.num_syms = (unsigned int[]) { 1 }, \
		.keysyms = (uint32_t*[]) { (uint32_t[]) { (_sym) } }, \
	}

/*
 * Configuration Context
 * A configuration context is initialized with an array of config-options and
 * then can be used to parse different sources. The backing memory is managed by
 * the user, not by this context.
 * All options are set to their default values on startup and reset.
 */

struct conf_ctx;

int conf_ctx_new(struct conf_ctx **out, const struct conf_option *opts,
		 size_t onum, void *mem);
void conf_ctx_free(struct conf_ctx *ctx);
void conf_ctx_reset(struct conf_ctx *ctx);
void *conf_ctx_get_mem(struct conf_ctx *ctx);

int conf_ctx_parse_ctx(struct conf_ctx *ctx, const struct conf_ctx *src);
int conf_ctx_parse_argv(struct conf_ctx *ctx, int argc, char **argv);
int conf_ctx_parse_file(struct conf_ctx *ctx, const char *format, ...);

/*
 * Configuration Options
 * A configuration option specifies the name of the option, the type, the
 * backing memory, the default value and more. Each option is represented by
 * this structure.
 */

#define CONF_LOCKED		0x0001

struct conf_option {
	unsigned int flags;
	char short_name;
	const char *long_name;
	const struct conf_type *type;
	int (*aftercheck) (struct conf_option *opt, int argc,
			   char **argv, int idx);
	int (*copy) (struct conf_option *opt, const struct conf_option *src);
	int (*file) (struct conf_option *opt, bool on, const char *arg);
	void *mem;
	void *def;
};

#define CONF_OPTION(_flags, _short, _long, _type, _aftercheck, _copy, _file, _mem, _def) \
	{ _flags, _short, "no-" _long, _type, _aftercheck, _copy, _file, _mem, _def }

#define CONF_OPTION_BOOL_FULL(_short, _long, _aftercheck, _copy, _file, _mem, _def) \
	CONF_OPTION(0, _short, _long, &conf_bool, _aftercheck, _copy, _file, _mem, (void*)(long)_def)
#define CONF_OPTION_BOOL(_short, _long, _mem, _def) \
	CONF_OPTION_BOOL_FULL(_short, _long, NULL, NULL, NULL, _mem, _def)

#define CONF_OPTION_INT_FULL(_short, _long, _aftercheck, _copy, _file, _mem, _def) \
	CONF_OPTION(0, _short, _long, &conf_int, _aftercheck, _copy, _file, _mem, (void*)(long)_def)
#define CONF_OPTION_INT(_short, _long, _mem, _def) \
	CONF_OPTION_INT_FULL(_short, _long, NULL, NULL, NULL, _mem, _def)

#define CONF_OPTION_UINT_FULL(_short, _long, _aftercheck, _copy, _file, _mem, _def) \
	CONF_OPTION(0, _short, _long, &conf_uint, _aftercheck, _copy, _file, _mem, (void*)(unsigned long)_def)
#define CONF_OPTION_UINT(_short, _long, _mem, _def) \
	CONF_OPTION_UINT_FULL(_short, _long, NULL, NULL, NULL, _mem, _def)

#define CONF_OPTION_STRING_FULL(_short, _long, _aftercheck, _copy, _file, _mem, _def) \
	CONF_OPTION(0, _short, _long, &conf_string, _aftercheck, _copy, _file, _mem, _def)
#define CONF_OPTION_STRING(_short, _long, _mem, _def) \
	CONF_OPTION_STRING_FULL(_short, _long, NULL, NULL, NULL, _mem, _def)

#define CONF_OPTION_STRING_LIST_FULL(_short, _long, _aftercheck, _copy, _file, _mem, _def) \
	CONF_OPTION(0, _short, _long, &conf_string_list, _aftercheck, _copy, _file, _mem, _def)
#define CONF_OPTION_STRING_LIST(_short, _long, _mem, _def) \
	CONF_OPTION_STRING_LIST_FULL(_short, _long, NULL, NULL, NULL, _mem, _def)

#define CONF_OPTION_GRAB_FULL(_short, _long, _aftercheck, _copy, _file, _mem, _def) \
	CONF_OPTION(0, _short, _long, &conf_grab, _aftercheck, _copy, _file, _mem, _def)
#define CONF_OPTION_GRAB(_short, _long, _mem, _def) \
	CONF_OPTION_GRAB_FULL(_short, _long, NULL, NULL, NULL, _mem, _def)

#endif /* CONF_CONF_H */
