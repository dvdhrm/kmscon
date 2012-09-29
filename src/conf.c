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
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "conf.h"
#include "log.h"

#define LOG_SUBSYSTEM "config"

void conf_free_value(struct conf_option *opt)
{
	if (*(void**)opt->mem != opt->def)
		free(*(void**)opt->mem);
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
	unsigned int i;
	unsigned int num, len, size, pos;
	char **list, *off;

	num = 0;
	size = 0;
	len = 0;
	for (i = 0; arg[i]; ++i) {
		if (arg[i] != ',') {
			++len;
			continue;
		}

		++num;
		size += len + 1;
		len = 0;
	}

	if (len > 0 || !i || (i > 0 && arg[i - 1] == ',')) {
		++num;
		size += len + 1;
	}

	list = malloc(sizeof(char*) * (num + 1) + size);
	if (!list)
		return -ENOMEM;

	off = (void*)(((char*)list) + (sizeof(char*) * (num + 1)));
	i = 0;
	for (pos = 0; pos < num; ++pos) {
		list[pos] = off;
		while (arg[i] && arg[i] != ',')
			*off++ = arg[i++];
		if (arg[i])
			++i;
		*off++ = 0;
	}
	list[pos] = NULL;

	opt->type->free(opt);
	*(void**)opt->mem = list;
	return 0;
}

void conf_default_string_list(struct conf_option *opt)
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
	ret = parse_buffer(opts, len, buf, pos);

out_free:
	free(buf);
	close(fd);
	return ret;
}

int conf_parse_all_files(struct conf_option *opts, size_t len)
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
			ret = conf_parse_file(opts, len, file);
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
					ret = conf_parse_file(opts, len, path);
			}
			free(path);
		}
	}

err_out:
	return ret;
}
