/*
 * App Configuration
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
 * Implementation of the configuration parsers.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include "conf.h"
#include "shl_log.h"
#include "shl_misc.h"

#define LOG_SUBSYSTEM "conf"

/*
 * Config Contexts
 */

struct conf_ctx {
	struct conf_option *opts;
	size_t onum;
	void *mem;
};

int conf_ctx_new(struct conf_ctx **out, const struct conf_option *opts,
		  size_t onum, void *mem)
{
	struct conf_ctx *ctx;
	size_t size;

	if (!out || !opts || !onum)
		return -EINVAL;

	size = sizeof(*ctx) + onum * sizeof(*opts);
	ctx = malloc(size);
	if (!ctx) {
		log_error("cannot allocate memory for config context");
		return -ENOMEM;
	}
	memset(ctx, 0, size);
	ctx->opts = (void*)((char*)ctx + sizeof(*ctx));
	ctx->onum = onum;
	ctx->mem = mem;
	memcpy(ctx->opts, opts, onum * sizeof(*opts));

	conf_ctx_reset(ctx);

	*out = ctx;
	return 0;
}

void conf_ctx_free(struct conf_ctx *ctx)
{
	if (!ctx)
		return;

	conf_ctx_reset(ctx);
	free(ctx);
}

void conf_ctx_reset(struct conf_ctx *ctx)
{
	unsigned int i;

	if (!ctx)
		return;

	for (i = 0; i < ctx->onum; ++i) {
		if (ctx->opts[i].type->free)
			ctx->opts[i].type->free(&ctx->opts[i]);
		ctx->opts[i].flags = 0;
		if (ctx->opts[i].type->set_default)
			ctx->opts[i].type->set_default(&ctx->opts[i]);
	}
}

void *conf_ctx_get_mem(struct conf_ctx *ctx)
{
	if (!ctx)
		return NULL;

	return ctx->mem;
}

/*
 * Copy all entries from \src into \ctx
 * This calls the "copy" callback for each option inside of \ctx with the
 * corresponding option inside of \src if both have the same type. If the types
 * do not match, nothing is done.
 */
int conf_ctx_parse_ctx(struct conf_ctx *ctx, const struct conf_ctx *src)
{
	unsigned int i;
	struct conf_option *d, *s, *o;
	int ret;

	if (!ctx || !src)
		return -EINVAL;

	for (i = 0; i < ctx->onum && i < src->onum; ++i) {
		d = &ctx->opts[i];
		s = &src->opts[i];

		if (d->type != s->type)
			continue;
		if (d->flags & CONF_LOCKED)
			continue;

		if (s->flags & CONF_LOCKED)
			d->flags |= CONF_LOCKED;

		if (d->type->copy) {
			ret = d->type->copy(d, s);
			if (ret)
				return ret;
		}

		if (d->copy) {
			ret = d->copy(d, s);
			if (ret)
				return ret;
		}
	}

	for (i = 0; i < ctx->onum; ++i) {
		o = &ctx->opts[i];
		if (o->aftercheck) {
			ret = o->aftercheck(o, 0, NULL, 0);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

/*
 * Parse command line arguments
 * This temporarily allocates the short_options and long_options arrays so we
 * can use the getopt_long() library call. It locks all arguments after they
 * have been set so command-line options will always overwrite config-options.
 */
int conf_ctx_parse_argv(struct conf_ctx *ctx, int argc, char **argv)
{
	char *short_options;
	struct option *long_options;
	struct option *opt;
	struct conf_option *o;
	size_t i, pos;
	int c, ret;

	if (!ctx || !argv)
		return -EINVAL;

	short_options = malloc(sizeof(char) * (ctx->onum + 1) * 2);
	if (!short_options) {
		log_error("out of memory to parse cmd-line arguments (%d): %m",
			  errno);
		return -ENOMEM;
	}

	long_options = malloc(sizeof(struct option) * ctx->onum * 2);
	if (!long_options) {
		log_error("out of memory to parse cmd-line arguments (%d): %m",
			  errno);
		free(short_options);
		return -ENOMEM;
	}

	pos = 0;
	short_options[pos++] = ':';
	opt = long_options;
	for (i = 0; i < ctx->onum; ++i) {
		if (ctx->opts[i].short_name) {
			short_options[pos++] = ctx->opts[i].short_name;
			if (ctx->opts[i].type->flags & CONF_HAS_ARG)
				short_options[pos++] = ':';
		}

		if (ctx->opts[i].long_name) {
			/* skip the "no-" prefix */
			opt->name = &ctx->opts[i].long_name[3];
			opt->has_arg = !!(ctx->opts[i].type->flags &
								CONF_HAS_ARG);
			opt->flag = NULL;
			opt->val = 100000 + i;
			++opt;

			/* boolean args are also added with "no-" prefix */
			if (!(ctx->opts[i].type->flags & CONF_HAS_ARG)) {
				opt->name = ctx->opts[i].long_name;
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
				fprintf(stderr, "Option takes no arg: %s\n",
					argv[optind - 1]);
			return -EFAULT;
		} else if (c < 100000) {
			for (i = 0; i < ctx->onum; ++i) {
				o = &ctx->opts[i];

				if (o->short_name != c)
					continue;

				if (o->type->parse) {
					ret = o->type->parse(o, true, optarg);
					if (ret)
						return ret;
				}

				o->flags |= CONF_LOCKED;
				break;
			}
		} else if (c < 200000) {
			i = c - 100000;
			o = &ctx->opts[i];

			if (o->type->parse) {
				ret = o->type->parse(o, true, optarg);
				if (ret)
					return ret;
			}

			o->flags |= CONF_LOCKED;
		} else {
			i = c - 200000;
			o = &ctx->opts[i];

			if (o->type->parse) {
				ret = o->type->parse(o, false, NULL);
				if (ret)
					return ret;
			}

			o->flags |= CONF_LOCKED;
		}
	}

	free(long_options);
	free(short_options);

	/* Perform aftercheck:
	 * All arguments that provide an aftercheck will be passed the remaining
	 * arguments in order. If they return a negative error code, it is
	 * interpreted as fatal error and returned to the caller. A positive
	 * error code is interpreted as the amount of remaining arguments that
	 * have been consumed by this aftercheck. 0 means nothing has been
	 * consumed.
	 * The next argument's aftercheck will only get the now remaning
	 * arguments passed in. If not all arguments are consumed, then this
	 * function will report an error to the caller. */
	for (i = 0; i < ctx->onum; ++i) {
		o = &ctx->opts[i];
		if (o->aftercheck) {
			ret = o->aftercheck(o, argc, argv, optind);
			if (ret < 0)
				return ret;
			optind += ret;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Unparsed remaining args starting with: %s\n",
			argv[optind]);
		return -EFAULT;
	}

	return 0;
}

static int parse_kv_pair(struct conf_option *opts, size_t len,
			 const char *key, const char *value)
{
	unsigned int i;
	int ret;
	bool set;
	struct conf_option *opt;

	for (i = 0; i < len; ++i) {
		opt = &opts[i];
		if (!opt->long_name)
			continue;

		if (!strcmp(key, opt->long_name))
			set = false;
		else if (!strcmp(key, &opt->long_name[3]))
			set = true;
		else
			continue;

		/* ignore if already set by command-line arguments */
		if (opt->flags & CONF_LOCKED)
			return 0;

		if (opt->file) {
			ret = opt->file(opt, set, value);
			if (ret)
				return ret;
			return 0;
		}

		if (opt->type->flags & CONF_HAS_ARG && !value) {
			log_error("config option '%s' requires an argument",
				  key);
			return -EFAULT;
		} else if (!(opt->type->flags & CONF_HAS_ARG) && value) {
			log_error("config option '%s' does not take arguments",
				  key);
			return -EFAULT;
		}

		if (opt->type->parse) {
			ret = opt->type->parse(opt, set, value);
			if (ret)
				return ret;
		}

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

static int parse_line(struct conf_option *opts, size_t olen,
		      char **buf, size_t *size)
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

		ret = parse_kv_pair(opts, olen, key, value);
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

static int parse_buffer(struct conf_option *opts, size_t len,
			char *buf, size_t size)
{
	int ret = 0;
	struct conf_option *o;
	unsigned int i;

	while (!ret && size > 0)
		ret = parse_line(opts, len, &buf, &size);

	if (ret)
		return ret;

	for (i = 0; i < len; ++i) {
		o = &opts[i];
		if (o->aftercheck) {
			ret = o->aftercheck(o, 0, NULL, 0);
			if (ret < 0)
				return ret;
		}
	}

	return 0;
}

/* chunk size when reading config files */
#define CONF_BUFSIZE 4096

/* This reads the file at \path in memory and parses it as if it was given as
 * command line options. */
static int conf_parse_file(struct conf_option *opts, size_t len,
			   const char *path)
{
	int fd, ret;
	size_t size, pos;
	char *buf, *tmp;

	if (!opts || !len || !path)
		return -EINVAL;

	if (access(path, F_OK))
		return 0;

	if (access(path, R_OK)) {
		log_error("read access to config file %s denied", path);
		return -EACCES;
	}

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
	ret = parse_buffer(opts, len, buf, pos);

out_free:
	free(buf);
	close(fd);
	return ret;
}

int conf_ctx_parse_file(struct conf_ctx *ctx, const char *format, ...)
{
	va_list list;
	char *path;
	int ret;

	if (!ctx || !format)
		return -EINVAL;

	va_start(list, format);
	ret = vasprintf(&path, format, list);
	va_end(list);

	if (ret < 0) {
		log_error("cannot allocate memory for config-file path");
		return -ENOMEM;
	}

	ret = conf_parse_file(ctx->opts, ctx->onum, path);
	free(path);
	return ret;
}

/*
 * Config Types
 * Each option that can be parsed must be a specific config-type. A config-type
 * is used to parse, free and reset the value. It must implement the following
 * callbacks:
 *   set_default: This should link opt->mem to opt->def and must not fail. It is
 *                called during initialization and reset.
 *   free: This should free any allocated memory and reset the option to the
 *         initial state. It must not fail.
 *   parse: This should parse a command-line option. Return 0 on success.
 *   copy: Copy data from source into destination. Return 0 on success.
 *
 * The backing memory is zeroed on reset so a config-type must be able to handle
 * this as "not set". Also, the "free" callback should reset it to zero (which
 * is the initial state).
 */

/* Miscellaneous helper */

static void conf_free_value(struct conf_option *opt)
{
	if (*(void**)opt->mem) {
		if (*(void**)opt->mem != opt->def)
			free(*(void**)opt->mem);
		*(void**)opt->mem = NULL;
	}
}

/* Boolean Option */

static void conf_default_bool(struct conf_option *opt)
{
	*(bool*)opt->mem = (bool)opt->def;
}

static void conf_free_bool(struct conf_option *opt)
{
	*(bool*)opt->mem = false;
}

static int conf_parse_bool(struct conf_option *opt, bool on, const char *arg)
{
	*(bool*)opt->mem = on;
	return 0;
}

static int conf_copy_bool(struct conf_option *opt,
			  const struct conf_option *src)
{
	*(bool*)opt->mem = *(bool*)src->mem;
	return 0;
}

const struct conf_type conf_bool = {
	.flags = 0,
	.set_default = conf_default_bool,
	.free = conf_free_bool,
	.parse = conf_parse_bool,
	.copy = conf_copy_bool,
};

/* Int Option */

static int conf_parse_int(struct conf_option *opt, bool on, const char *arg)
{
	*(int*)opt->mem = atoi(arg);
	return 0;
}

static void conf_free_int(struct conf_option *opt)
{
	*(int*)opt->mem = 0;
}

static void conf_default_int(struct conf_option *opt)
{
	*(int*)opt->mem = (int)(unsigned long)opt->def;
}

static int conf_copy_int(struct conf_option *opt,
			 const struct conf_option *src)
{
	*(int*)opt->mem = *(int*)src->mem;
	return 0;
}

const struct conf_type conf_int = {
	.flags = CONF_HAS_ARG,
	.set_default = conf_default_int,
	.free = conf_free_int,
	.parse = conf_parse_int,
	.copy = conf_copy_int,
};

/* Unsigned Int Option */

static void conf_default_uint(struct conf_option *opt)
{
	*(unsigned int*)opt->mem = (unsigned int)(unsigned long)opt->def;
}

static void conf_free_uint(struct conf_option *opt)
{
	*(unsigned int*)opt->mem = 0;
}

static int conf_parse_uint(struct conf_option *opt, bool on, const char *arg)
{
	*(unsigned int*)opt->mem = atoi(arg);
	return 0;
}

static int conf_copy_uint(struct conf_option *opt,
			  const struct conf_option *src)
{
	*(unsigned int*)opt->mem = *(unsigned int*)src->mem;
	return 0;
}

const struct conf_type conf_uint = {
	.flags = CONF_HAS_ARG,
	.set_default = conf_default_uint,
	.free = conf_free_uint,
	.parse = conf_parse_uint,
	.copy = conf_copy_uint,
};

/* String Option */

static void conf_default_string(struct conf_option *opt)
{
	opt->type->free(opt);
	*(void**)opt->mem = opt->def;
}

static int conf_parse_string(struct conf_option *opt, bool on, const char *arg)
{
	char *val = strdup(arg);
	if (!val)
		return -ENOMEM;

	opt->type->free(opt);
	*(void**)opt->mem = val;
	return 0;
}

static int conf_copy_string(struct conf_option *opt,
			    const struct conf_option *src)
{
	char *val;

	if (!*(void**)src->mem) {
		val = NULL;
	} else {
		val = strdup(*(void**)src->mem);
		if (!val)
			return -ENOMEM;
	}

	opt->type->free(opt);
	*(void**)opt->mem = val;
	return 0;
}

const struct conf_type conf_string = {
	.flags = CONF_HAS_ARG,
	.set_default = conf_default_string,
	.free = conf_free_value,
	.parse = conf_parse_string,
	.copy = conf_copy_string,
};

/* Stringlist Option */

static void conf_default_string_list(struct conf_option *opt)
{
	opt->type->free(opt);
	*(void**)opt->mem = opt->def;
}

static int conf_parse_string_list(struct conf_option *opt, bool on,
				  const char *arg)
{
	int ret;
	char **list;

	ret = shl_split_string(arg, &list, NULL, ',', true);
	if (ret)
		return ret;

	opt->type->free(opt);
	*(char***)opt->mem = list;
	return 0;
}

static int conf_copy_string_list(struct conf_option *opt,
				 const struct conf_option *src)
{
	int ret;
	char **t;

	if (!(void***)src->mem) {
		t = NULL;
	} else {
		ret = shl_dup_array(&t, *(char***)src->mem);
		if (ret)
			return ret;
	}

	opt->type->free(opt);
	*(char***)opt->mem = t;
	return 0;
}

const struct conf_type conf_string_list = {
	.flags = CONF_HAS_ARG,
	.set_default = conf_default_string_list,
	.free = conf_free_value,
	.parse = conf_parse_string_list,
	.copy = conf_copy_string_list,
};

/* Grab Option */

static void conf_default_grab(struct conf_option *opt)
{
	opt->type->free(opt);
	*(void**)opt->mem = opt->def;
}

static void conf_free_grab(struct conf_option *opt)
{
	struct conf_grab *grab;
	unsigned int i;

	grab = *(void**)opt->mem;
	*(void**)opt->mem = NULL;

	if (!grab || grab == opt->def)
		return;

	for (i = 0; i < grab->num; ++i)
		free(grab->keysyms[i]);

	free(grab->keysyms);
	free(grab->num_syms);
	free(grab->mods);
	free(grab);
}

static int parse_single_grab(char *arg, unsigned int *mods,
			     uint32_t *keysym, bool allow_mods)
{
	char *tmp, *start, *end;
	char buf[128];

	tmp = arg;
	do {
		while (*tmp == ' ')
			++tmp;
		if (!allow_mods)
			break;
		if (*tmp != '<')
			break;

		start = tmp;
		while (*tmp && *tmp != '>')
			++tmp;

		if (*tmp != '>') {
			log_error("missing '>' near '%s'", start);
			return -EFAULT;
		}

		*tmp++ = 0;
		++start;
		if (!strcasecmp(start, "shift")) {
			*mods |= SHL_SHIFT_MASK;
		} else if (!strcasecmp(start, "lock")) {
			*mods |= SHL_LOCK_MASK;
		} else if (!strcasecmp(start, "control") ||
			   !strcasecmp(start, "ctrl")) {
			*mods |= SHL_CONTROL_MASK;
		} else if (!strcasecmp(start, "alt")) {
			*mods |= SHL_ALT_MASK;
		} else if (!strcasecmp(start, "logo")) {
			*mods |= SHL_LOGO_MASK;
		} else {
			log_error("invalid modifier '%s'", start);
			return -EFAULT;
		}
	} while (1);

	while (*tmp == ' ')
		++tmp;

	start = tmp;
	end = start;
	do {
		while (*tmp && *tmp != ' ')
			++tmp;
		end = tmp;
		if (!*tmp)
			break;
		while (*tmp == ' ')
			++tmp;
	} while (1);

	if (start == end)
		return 0;
	if (*end)
		*end = 0;

	*keysym = xkb_keysym_from_name(start, 0);
	if (!*keysym) {
		*keysym = xkb_keysym_from_name(start,
					       XKB_KEYSYM_CASE_INSENSITIVE);
		if (!*keysym) {
			log_error("invalid key '%s'", start);
			return -EFAULT;
		}

		xkb_keysym_get_name(*keysym, buf, sizeof(buf));
		log_warning("invalid keysym '%s', did you mean '%s'? (keysyms are case-sensitive)",
			    start, buf);
		return -EFAULT;
	}

	return 1;
}

static int conf_parse_grab(struct conf_option *opt, bool on, const char *arg)
{
	char **list, **keys;
	unsigned int list_num, key_num, i, j, k, l;
	int ret;
	struct conf_grab *grab;

	ret = shl_split_string(arg, &list, &list_num, ',', false);
	if (ret)
		return ret;

	grab = malloc(sizeof(*grab));
	if (!grab) {
		ret = -ENOMEM;
		goto err_list;
	}
	memset(grab, 0, sizeof(*grab));

	if (list_num) {
		grab->mods = malloc(sizeof(*grab->mods) * list_num);
		if (!grab->mods) {
			ret = -ENOMEM;
			goto err_grab;
		}
		memset(grab->mods, 0, sizeof(*grab->mods) * list_num);

		grab->num_syms = malloc(sizeof(*grab->num_syms) * list_num);
		if (!grab->num_syms) {
			ret = -ENOMEM;
			goto err_grab;
		}
		memset(grab->num_syms, 0, sizeof(*grab->num_syms) * list_num);

		grab->keysyms = malloc(sizeof(*grab->keysyms) * list_num);
		if (!grab->keysyms) {
			ret = -ENOMEM;
			goto err_grab;
		}
		memset(grab->keysyms, 0, sizeof(*grab->keysyms) * list_num);
	}

	l = 0;
	for (i = 0; i < list_num; ++i) {
		ret = shl_split_string(list[i], &keys, &key_num, '+', false);
		if (ret)
			goto err_all;
		if (!key_num) {
			free(keys);
			continue;
		}

		grab->keysyms[l] = malloc(sizeof(*grab->keysyms[l]) * key_num);
		if (!grab->keysyms[l]) {
			ret = -ENOMEM;
			free(keys);
			goto err_all;
		}

		k = 0;
		for (j = 0; j < key_num; ++j) {
			ret = parse_single_grab(keys[j], &grab->mods[l],
						&grab->keysyms[l][k],
						j == 0);
			if (ret < 0) {
				log_error("cannot parse grab '%s' in '%s'",
					  list[i], arg);
				free(keys);
				goto err_all;
			}
			k += ret;
		}

		free(keys);
		if (!k)
			continue;
		grab->num_syms[l] = k;
		++l;
		++grab->num;
	}

	free(list);
	opt->type->free(opt);
	*(void**)opt->mem = grab;
	return 0;

err_all:
	for (i = 0; i < list_num; ++i)
		free(grab->keysyms[i]);
err_grab:
	free(grab->keysyms);
	free(grab->num_syms);
	free(grab->mods);
	free(grab);
err_list:
	free(list);
	return ret;
}

static int conf_copy_grab(struct conf_option *opt,
			  const struct conf_option *src)
{
	struct conf_grab *grab, *s;
	int ret;
	unsigned int i;

	s = *(void**)src->mem;

	if (!s) {
		opt->type->free(opt);
		*(void**)opt->mem = NULL;
		return 0;
	}

	grab = malloc(sizeof(*grab));
	if (!grab)
		return -ENOMEM;
	memset(grab, 0, sizeof(*grab));
	grab->num = s->num;

	if (grab->num) {
		grab->mods = malloc(sizeof(*grab->mods) * grab->num);
		if (!grab->mods) {
			ret = -ENOMEM;
			goto err_grab;
		}
		memcpy(grab->mods, s->mods, sizeof(*grab->mods) * grab->num);

		grab->num_syms = malloc(sizeof(*grab->num_syms) * grab->num);
		if (!grab->num_syms) {
			ret = -ENOMEM;
			goto err_grab;
		}
		memcpy(grab->num_syms, s->num_syms,
		       sizeof(*grab->num_syms) * grab->num);

		grab->keysyms = malloc(sizeof(*grab->keysyms) * grab->num);
		if (!grab->keysyms) {
			ret = -ENOMEM;
			goto err_grab;
		}
		memset(grab->keysyms, 0, sizeof(*grab->keysyms) * grab->num);
	}

	for (i = 0; i < grab->num; ++i) {
		grab->keysyms[i] = malloc(sizeof(*s->keysyms[i]) *
					  s->num_syms[i]);
		if (!grab->keysyms[i]) {
			ret = -ENOMEM;
			goto err_all;
		}
		memcpy(grab->keysyms[i], s->keysyms[i],
		       sizeof(*s->keysyms[i]) * s->num_syms[i]);
	}

	opt->type->free(opt);
	*(void**)opt->mem = grab;
	return 0;

err_all:
	for (i = 0; i < grab->num; ++i)
		free(grab->keysyms[i]);
err_grab:
	free(grab->keysyms);
	free(grab->num_syms);
	free(grab->mods);
	free(grab);
	return ret;
}

const struct conf_type conf_grab = {
	.flags = CONF_HAS_ARG,
	.set_default = conf_default_grab,
	.free = conf_free_grab,
	.parse = conf_parse_grab,
	.copy = conf_copy_grab,
};
