/*
 * Log/Debug Interface
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Dedicated to the Public Domain
 */

/*
 * Log/Debug API Implementation
 * We provide thread-safety so we need a global lock. Function which
 * are prefixed with log__* need the lock to be held. All other functions must
 * be called without the lock held.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "shl_githead.h"
#include "shl_log.h"
#include "shl_misc.h"

/*
 * Locking
 * We need a global locking mechanism. Use pthread here.
 */

static pthread_mutex_t log__mutex = PTHREAD_MUTEX_INITIALIZER;

static inline void log_lock()
{
	pthread_mutex_lock(&log__mutex);
}

static inline void log_unlock()
{
	pthread_mutex_unlock(&log__mutex);
}

/*
 * Time Management
 * We print seconds and microseconds since application start for each
 * log-message.
 */

static struct timeval log__ftime;

static void log__time(long long *sec, long long *usec)
{
	struct timeval t;

	if (log__ftime.tv_sec == 0 && log__ftime.tv_usec == 0) {
		gettimeofday(&log__ftime, NULL);
		*sec = 0;
		*usec = 0;
	} else {
		gettimeofday(&t, NULL);
		*sec = t.tv_sec - log__ftime.tv_sec;
		*usec = (long long)t.tv_usec - (long long)log__ftime.tv_usec;
		if (*usec < 0) {
			*sec -= 1;
			*usec = 1000000 + *usec;
		}
	}
}

/*
 * Default Values
 * Several logging-parameters may be omitted by applications. To provide sane
 * default values we provide constants here.
 *
 * LOG_SUBSYSTEM: By default no subsystem is specified
 */

SHL_EXPORT
const struct log_config LOG_CONFIG = {
	.sev = {
		[LOG_DEBUG] = 2,
		[LOG_INFO] = 2,
		[LOG_NOTICE] = 2,
		[LOG_WARNING] = 2,
		[LOG_ERROR] = 2,
		[LOG_CRITICAL] = 2,
		[LOG_ALERT] = 2,
		[LOG_FATAL] = 2,
	}
};

const char *LOG_SUBSYSTEM = NULL;

/*
 * Filters
 * By default DEBUG and INFO messages are disabled. If LOG_ENABLE_DEBUG is not
 * defined, then all log_debug() statements compile to zero-code and they cannot
 * be enabled on runtime.
 * To enable DEBUG or INFO messages at runtime, you can either specify that they
 * should be enabled globally, per file or specify a custom filter. Other
 * messages than DEBUG and INFO cannot be configured. However, additional
 * configuration options may be added later.
 *
 * Use log_set_config() to enable debug/info messages globally. If you
 * enable a global message type, then all other filters are skipped. If you
 * disable a global message type then fine-grained filters can take effect.
 *
 * To enable DEBUG/INFO messages for a specific source-file, you can add
 * this line to the top of the source file:
 *   #define LOG_CONFIG LOG_STATIC_CONFIG(true, true)
 * So info and debug messages are enabled for this file on compile-time. First
 * parameter of LOG_STATIC_CONFIG is for debug, second one for info.
 *
 * Or you can add new configurations on runtime. Runtime configurations take a
 * filter parameter and a config parameter. The filter specifies what messages
 * are affected and the config parameter specifies what action is performed.
 */

static struct log_config log__gconfig = {
	.sev = {
		[LOG_DEBUG] = 0,
		[LOG_INFO] = 0,
		[LOG_NOTICE] = 1,
		[LOG_WARNING] = 1,
		[LOG_ERROR] = 1,
		[LOG_CRITICAL] = 1,
		[LOG_ALERT] = 1,
		[LOG_FATAL] = 1,
	}
};

struct log_dynconf {
	struct log_dynconf *next;
	int handle;
	struct log_filter filter;
	struct log_config config;
};

static struct log_dynconf *log__dconfig = NULL;

void log_set_config(const struct log_config *config)
{
	if (!config)
		return;

	log_lock();
	log__gconfig = *config;
	log_unlock();
}

int log_add_filter(const struct log_filter *filter,
			const struct log_config *config)
{
	struct log_dynconf *dconf;
	int ret;

	if (!filter || !config)
		return -EINVAL;

	dconf = malloc(sizeof(*dconf));
	if (!dconf)
		return -ENOMEM;

	memset(dconf, 0, sizeof(*dconf));
	memcpy(&dconf->filter, filter, sizeof(*filter));
	memcpy(&dconf->config, config, sizeof(*config));

	log_lock();
	if (log__dconfig)
		dconf->handle = log__dconfig->handle + 1;
	dconf->next = log__dconfig;
	log__dconfig = dconf;
	ret = dconf->handle;
	log_unlock();

	return ret;
}

void log_rm_filter(int handle)
{
	struct log_dynconf *dconf, *i;

	dconf = NULL;

	log_lock();
	if (log__dconfig) {
		if (log__dconfig->handle == handle) {
			dconf = log__dconfig;
			log__dconfig = dconf->next;
		} else for (i = log__dconfig; i->next; i = i->next) {
			dconf = i->next;
			if (dconf->handle == handle) {
				i->next = dconf->next;
				break;
			}
		}
	}
	log_unlock();

	free(dconf);
}

void log_clean_filters()
{
	struct log_dynconf *dconf;

	log_lock();
	while ((dconf = log__dconfig)) {
		log__dconfig = dconf->next;
		free(dconf);
	}
	log_unlock();
}

static bool log__matches(const struct log_filter *filter,
				const char *file,
				int line,
				const char *func,
				const char *subs)
{
	if (*filter->file) {
		if (!file || strncmp(filter->file, file, LOG_STRMAX))
			return false;
	}
	if (filter->line >= 0 && filter->line != line)
		return false;
	if (*filter->func) {
		if (!func || strncmp(filter->func, func, LOG_STRMAX))
			return false;
	}
	if (*filter->subs) {
		if (!subs || strncmp(filter->subs, subs, LOG_STRMAX))
			return false;
	}
	return true;
}

static bool log__omit(const char *file,
			int line,
			const char *func,
			const struct log_config *config,
			const char *subs,
			enum log_severity sev)
{
	int val;
	struct log_dynconf *dconf;

	if (sev >= LOG_SEV_NUM)
		return false;

	if (config) {
		val = config->sev[sev];
		if (val == 0)
			return true;
		if (val == 1)
			return false;
	}

	for (dconf = log__dconfig; dconf; dconf = dconf->next) {
		if (log__matches(&dconf->filter, file, line, func, subs)) {
			val = dconf->config.sev[sev];
			if (val == 0)
				return true;
			if (val == 1)
				return false;
		}
	}

	val = log__gconfig.sev[sev];
	if (val == 0)
		return true;
	if (val == 1)
		return false;

	return false;
}

/*
 * Forward declaration so we can use the locked-versions in other functions
 * here. Be careful to avoid deadlocks, though.
 * Also set default log-subsystem to "log" for all logging inside this API.
 */

static void log__submit(const char *file,
			int line,
			const char *func,
			const struct log_config *config,
			const char *subs,
			unsigned int sev,
			const char *format,
			va_list args);

static void log__format(const char *file,
			int line,
			const char *func,
			const struct log_config *config,
			const char *subs,
			unsigned int sev,
			const char *format,
			...);

#define LOG_SUBSYSTEM "log"

/*
 * Log-File
 * By default logging is done to stderr. However, you can set a file which is
 * used instead of stderr for logging. We do not provide complex log-rotation or
 * management functions, you can add them yourself or use a proper init-system
 * like systemd which does this for you.
 * We cannot set this to "stderr" as stderr might not be a compile-time
 * constant. Therefore, NULL means stderr.
 */

static FILE *log__file = NULL;

int log_set_file(const char *file)
{
	FILE *f, *old;

	if (file) {
		f = fopen(file, "a");
		if (!f) {
			log_err("cannot change log-file to %s (%d): %s",
				file, errno, strerror(errno));
			return -EFAULT;
		}
	} else {
		f = NULL;
		file = "<default>";
	}

	old = NULL;

	log_lock();
	if (log__file != f) {
		log__format(LOG_DEFAULT, LOG_NOTICE,
				"set log-file to %s", file);
		old = log__file;
		log__file = f;
		f = NULL;
	}
	log_unlock();

	if (f)
		fclose(f);
	if (old)
		fclose(old);

	return 0;
}

/*
 * Basic logger
 * The log__submit function writes the message into the current log-target. It
 * must be called with log__mutex locked.
 * log__format does the same but first converts the argument list into a
 * va_list.
 * By default the current time elapsed since the first message was logged is
 * prepended to the message. file, line and func information are appended to the
 * message if sev == LOG_DEBUG.
 * The subsystem, if not NULL, is prepended as "SUBS: " to the message and a
 * newline is always appended by default. Multiline-messages are not allowed and
 * do not make sense here.
 */

static const char *log__sev2str[] = {
	[LOG_DEBUG] = "DEBUG",
	[LOG_INFO] = "INFO",
	[LOG_NOTICE] = "NOTICE",
	[LOG_WARNING] = "WARNING",
	[LOG_ERROR] = "ERROR",
	[LOG_CRITICAL] = "CRITICAL",
	[LOG_ALERT] = "ALERT",
	[LOG_FATAL] = "FATAL",
};

static void log__submit(const char *file,
			int line,
			const char *func,
			const struct log_config *config,
			const char *subs,
			unsigned int sev,
			const char *format,
			va_list args)
{
	const char *prefix = NULL;
	FILE *out;
	long long sec, usec;

	if (log__omit(file, line, func, config, subs, sev))
		return;

	if (log__file)
		out = log__file;
	else
		out = stderr;

	log__time(&sec, &usec);

	if (sev < LOG_SEV_NUM)
		prefix = log__sev2str[sev];

	if (prefix) {
		if (subs)
			fprintf(out, "[%.4lld.%.6lld] %s: %s: ",
				sec, usec, prefix, subs);
		else
			fprintf(out, "[%.4lld.%.6lld] %s: ",
				sec, usec, prefix);
	} else {
		if (subs)
			fprintf(out, "[%.4lld.%.6lld] %s: ", sec, usec, subs);
		else
			fprintf(out, "[%.4lld.%.6lld] ", sec, usec);
	}

	vfprintf(out, format, args);

	if (sev == LOG_DEBUG) {
		if (!func)
			func = "<unknown>";
		if (!file)
			file = "<unknown>";
		if (line < 0)
			line = 0;
		fprintf(out, " (%s() in %s:%d)\n", func, file, line);
	} else {
		fprintf(out, "\n");
	}
}

static void log__format(const char *file,
			int line,
			const char *func,
			const struct log_config *config,
			const char *subs,
			unsigned int sev,
			const char *format,
			...)
{
	va_list list;

	va_start(list, format);
	log__submit(file, line, func, config, subs, sev, format, list);
	va_end(list);
}

SHL_EXPORT
void log_submit(const char *file,
		int line,
		const char *func,
		const struct log_config *config,
		const char *subs,
		unsigned int sev,
		const char *format,
		va_list args)
{
	int saved_errno = errno;

	log_lock();
	log__submit(file, line, func, config, subs, sev, format, args);
	log_unlock();

	errno = saved_errno;
}

SHL_EXPORT
void log_format(const char *file,
		int line,
		const char *func,
		const struct log_config *config,
		const char *subs,
		unsigned int sev,
		const char *format,
		...)
{
	va_list list;
	int saved_errno = errno;

	va_start(list, format);
	log_lock();
	log__submit(file, line, func, config, subs, sev, format, list);
	log_unlock();
	va_end(list);

	errno = saved_errno;
}

SHL_EXPORT
void log_llog(void *data,
	      const char *file,
	      int line,
	      const char *func,
	      const char *subs,
	      unsigned int sev,
	      const char *format,
	      va_list args)
{
	log_submit(file, line, func, NULL, subs, sev, format, args);
}

void log_print_init(const char *appname)
{
	if (!appname)
		appname = "<unknown>";
	log_format(LOG_DEFAULT_CONF, NULL, LOG_NOTICE,
		   "%s Revision %s %s %s", appname,
		   shl_git_head, __DATE__, __TIME__);
}
