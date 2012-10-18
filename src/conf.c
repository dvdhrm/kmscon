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
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include "conf.h"
#include "log.h"
#include "shl_misc.h"

#define LOG_SUBSYSTEM "config"

void conf_free_value(struct conf_option *opt)
{
	if (*(void**)opt->mem && *(void**)opt->mem != opt->def) {
		free(*(void**)opt->mem);
		*(void**)opt->mem = NULL;
	}
}

int conf_parse_bool(struct conf_option *opt, bool on, const char *arg)
{
	*(bool*)opt->mem = on;
	return 0;
}

void conf_default_bool(struct conf_option *opt)
{
	*(bool*)opt->mem = (bool)opt->def;
}

int conf_parse_int(struct conf_option *opt, bool on, const char *arg)
{
	*(int*)opt->mem = atoi(arg);
	return 0;
}

void conf_default_int(struct conf_option *opt)
{
	*(int*)opt->mem = (int)(unsigned long)opt->def;
}

int conf_parse_uint(struct conf_option *opt, bool on, const char *arg)
{
	*(unsigned int*)opt->mem = atoi(arg);
	return 0;
}

void conf_default_uint(struct conf_option *opt)
{
	*(unsigned int*)opt->mem = (unsigned int)(unsigned long)opt->def;
}

int conf_parse_string(struct conf_option *opt, bool on, const char *arg)
{
	char *val = strdup(arg);
	if (!val)
		return -ENOMEM;

	opt->type->free(opt);
	*(void**)opt->mem = val;
	return 0;
}

void conf_default_string(struct conf_option *opt)
{
	*(void**)opt->mem = opt->def;
}

int conf_parse_string_list(struct conf_option *opt, bool on, const char *arg)
{
	int ret;
	char **list;

	ret = shl_split_string(arg, &list, NULL, ',', true);
	if (ret)
		return ret;

	opt->type->free(opt);
	*(void**)opt->mem = list;
	return 0;
}

void conf_default_string_list(struct conf_option *opt)
{
	*(void**)opt->mem = opt->def;
}

static int parse_single_grab(char *arg, unsigned int *mods,
			     uint32_t *keysym, bool allow_mods)
{
	char *tmp, *start, *end;

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

	*keysym = xkb_keysym_from_name(start);
	if (!*keysym) {
		log_error("invalid key '%s'", start);
		return -EFAULT;
	}

	return 1;
}

int conf_parse_grab(struct conf_option *opt, bool on, const char *arg)
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

		grab->keysyms[l] = malloc(sizeof(*grab->keysyms[l] * key_num));
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

void conf_free_grab(struct conf_option *opt)
{
	struct conf_grab *grab;
	unsigned int i;

	if (!*(void**)opt->mem || *(void**)opt->mem == opt->def)
		return;

	grab = *(void**)opt->mem;
	*(void**)opt->mem = NULL;

	for (i = 0; i < grab->num; ++i)
		free(grab->keysyms[i]);

	free(grab->keysyms);
	free(grab->num_syms);
	free(grab->mods);
	free(grab);
}

void conf_default_grab(struct conf_option *opt)
{
	*(void**)opt->mem = opt->def;
}

const struct conf_type conf_bool = {
	.flags = 0,
	.parse = conf_parse_bool,
	.free = NULL,
	.set_default = conf_default_bool,
};

const struct conf_type conf_int = {
	.flags = CONF_HAS_ARG,
	.parse = conf_parse_int,
	.free = NULL,
	.set_default = conf_default_int,
};

const struct conf_type conf_uint = {
	.flags = CONF_HAS_ARG,
	.parse = conf_parse_uint,
	.free = NULL,
	.set_default = conf_default_uint,
};

const struct conf_type conf_string = {
	.flags = CONF_HAS_ARG,
	.parse = conf_parse_string,
	.free = conf_free_value,
	.set_default = conf_default_string,
};

const struct conf_type conf_string_list = {
	.flags = CONF_HAS_ARG,
	.parse = conf_parse_string_list,
	.free = conf_free_value,
	.set_default = conf_default_string_list,
};

const struct conf_type conf_grab = {
	.flags = CONF_HAS_ARG,
	.parse = conf_parse_grab,
	.free = conf_free_grab,
	.set_default = conf_default_grab,
};

/* free all memory that we allocated and reset to initial state */
void conf_free(struct conf_option *opts, size_t len)
{
	unsigned int i;

	for (i = 0; i < len; ++i) {
		if (opts[i].type->free)
			opts[i].type->free(&opts[i]);
	}
}

/*
 * Parse command line arguments
 * This temporarily allocates the short_options and long_options arrays so we
 * can use the getopt_long() library call. It locks all arguments after they
 * have been set so command-line options will always overwrite config-options.
 */
int conf_parse_argv(struct conf_option *opts, size_t len,
		    int argc, char **argv)
{
	char *short_options;
	struct option *long_options;
	struct option *opt;
	size_t i, pos;
	int c, ret;

	if (!argv || argc < 1)
		return -EINVAL;

	short_options = malloc(sizeof(char) * (len + 1) * 2);
	if (!short_options) {
		log_error("cannot allocate enough memory to parse command line arguments (%d): %m", errno);
		return -ENOMEM;
	}

	long_options = malloc(sizeof(struct option) * len * 2);
	if (!long_options) {
		log_error("cannot allocate enough memory to parse command line arguments (%d): %m", errno);
		free(short_options);
		return -ENOMEM;
	}

	pos = 0;
	short_options[pos++] = ':';
	opt = long_options;
	for (i = 0; i < len; ++i) {
		if (opts[i].short_name) {
			short_options[pos++] = opts[i].short_name;
			if (opts[i].type->flags & CONF_HAS_ARG)
				short_options[pos++] = ':';
		}

		if (opts[i].long_name) {
			/* skip the "no-" prefix */
			opt->name = &opts[i].long_name[3];
			opt->has_arg = !!(opts[i].type->flags & CONF_HAS_ARG);
			opt->flag = NULL;
			opt->val = 100000 + i;
			++opt;

			/* boolean args are also added with "no-" prefix */
			if (!(opts[i].type->flags & CONF_HAS_ARG)) {
				opt->name = opts[i].long_name;
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
				if (opts[i].short_name == c) {
					ret = opts[i].type->parse(&opts[i],
								  true,
								  optarg);
					if (ret)
						return ret;
					opts[i].flags |= CONF_LOCKED;
					opts[i].flags |= CONF_DONE;
					break;
				}
			}
		} else if (c < 200000) {
			i = c - 100000;
			ret = opts[i].type->parse(&opts[i], true, optarg);
			if (ret)
				return ret;
			opts[i].flags |= CONF_LOCKED;
			opts[i].flags |= CONF_DONE;
		} else {
			i = c - 200000;
			ret = opts[i].type->parse(&opts[i], false, NULL);
			if (ret)
				return ret;
			opts[i].flags |= CONF_LOCKED;
			opts[i].flags |= CONF_DONE;
		}
	}

	free(long_options);
	free(short_options);

	/* set default values if not configured */
	for (i = 0; i < len; ++i) {
		if (!(opts[i].flags & CONF_DONE) &&
		    opts[i].type->set_default) {
			opts[i].type->set_default(&opts[i]);
		}
	}

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
	for (i = 0; i < len; ++i) {
		if (opts[i].aftercheck) {
			ret = opts[i].aftercheck(&opts[i], argc, argv, optind);
			if (ret < 0)
				return ret;
			optind += ret;
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Unparsed remaining arguments starting with: %s\n",
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

	while (!ret && size > 0)
		ret = parse_line(opts, len, &buf, &size);

	return ret;
}

/* chunk size when reading config files */
#define CONF_BUFSIZE 4096

/* This reads the file at \path in memory and parses it as if it was given as
 * command line options. */
int conf_parse_file(struct conf_option *opts, size_t len, const char *path)
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

int conf_parse_file_f(struct conf_option *opts, size_t len,
		      const char *format, ...)
{
	va_list list;
	char *path;
	int ret;

	if (!opts || !len || !format)
		return -EINVAL;

	va_start(list, format);
	ret = vasprintf(&path, format, list);
	va_end(list);

	if (ret < 0) {
		log_error("cannot allocate memory for config-file path");
		return -ENOMEM;
	}

	ret = conf_parse_file(opts, len, path);
	free(path);
	return ret;
}

int conf_parse_standard_files(struct conf_option *opts, size_t len,
			      const char *fname)
{
	int ret;
	const char *home;

	ret = conf_parse_file_f(opts, len, "/etc/%s.conf", fname);
	if (ret)
		return ret;

	home = getenv("HOME");
	if (home) {
		ret = conf_parse_file_f(opts, len, "%s/.%s.conf", home, fname);
		if (ret)
			return ret;
	}

	return 0;
}
