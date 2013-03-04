/*
 * Log/Debug Interface
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Dedicated to the Public Domain
 */

/*
 * Log/Debug Interface
 * This interface provides basic logging to a single file or stderr. By default,
 * all log-messages are forwarded to stderr but you can change this to an
 * arbitrary file. However, no complex file-rotation/backup functions are
 * supported so you should use the default (stderr) and use a proper init-system
 * like systemd to do log-rotations. This can also forward stderr messages into
 * log-files.
 *
 * Besides simple log-functions this also provides run-time filters so special
 * debug messages can be enabled/disabled. This should be used for
 * debugging-only because it may slow-down your application if every message is
 * filtered.
 *
 * Define BUILD_ENABLE_DEBUG before including this header to enable
 * debug-messages for this file.
 */

#ifndef SHL_LOG_H_INCLUDED
#define SHL_LOG_H_INCLUDED

#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>

/*
 * Log Messages and Filters
 * A log message consists of:
 *  - file: the source file where the call was made
 *  - line: the corresponding line number
 *  - func: the function name
 *  - config: special configuration for this message only
 *  - subs: the subsystem
 *  - sev: the severity
 *  - format: format string
 *  - args: arguments depending on format string
 * Depending on this information the log system decides whether the message is
 * discarded or logged and what information is included. To allow fine-grained
 * configuration you can add log_filter and log_config objects. A log_filter
 * object specifies to what messages the log_config object shall apply. If a
 * log_filter does not match, then the corresponding log_config is ignored.
 * The log_config specifies whether a message is discarded, logged or whether
 * other filters shall be searched.
 *
 * The "config" field of every log-message does not have a corresponding
 * log_filter object. Instead, it is assumed that the config object only applies
 * to this single message. This allows to specify special behavior for every
 * single message but also refer to global filters.
 *
 * Config Object:
 * A log_config object contains a severity-array. Each severity is the index of
 * an integer in the array. If the integer is 0, then messages with the given
 * severity are discarded, if is is 1, then they are logged. If it is 2, then
 * the config object is ignored and global filters will be used.
 *
 * Filter Object:
 * A filter object specifies what messages are affected by the corresponding
 * config object. file, func and subs are strings. If they are empty (length=0)
 * then they are not used for matching. If line is smaller than 0 then it is
 * ignored. Otherwise all given information must match.
 *
 * log_set_config(config):
 * This sets the global config which is used if no filter applies.
 *
 * log_add_filter(filter, config):
 * This adds a new filter to the global filter-list. If the filter matches the
 * given config shall apply. This returns a negative error code on failure.
 * Otherwise it returns an ID which can be used to remove the filter again.
 * An ID is always >= 0.
 *
 * log_rm_filter(id):
 * This removes the filter with ID=id.
 *
 * log_clean_filters():
 * This removes all filters. This frees all allocated memory by the filters.
 *
 *
 * If you want to set a config option which shall apply to all log-messages in
 * a single source-file, then you can add the following line to the head of the
 * source-file:
 *   #define LOG_CONFIG LOG_CONFIG_ALL(options...)
 * Where LOG_CONFIG_ALL() can be replaced by LOG_CONFIG_DEBUG, LOG_CONFIG_INFO
 * and so on.
 * The LOG_CONFIG_*() macros create a log_config object on the stack and are
 * used for convenience. You can also provide your own log_config object here.
 * This LOG_CONFIG constant is picked up by all log-helpers which are provided
 * below. The raw log_submit and log_format() functions are not affected by
 * this, though.
 */

enum log_severity {
	LOG_FATAL = 0,
	LOG_ALERT = 1,
	LOG_CRITICAL = 2,
	LOG_ERROR = 3,
	LOG_WARNING = 4,
	LOG_NOTICE = 5,
	LOG_INFO = 6,
	LOG_DEBUG = 7,
	LOG_SEV_NUM,
};

#define LOG_STRMAX 128

struct log_filter {
	char file[LOG_STRMAX];
	int line;
	char func[LOG_STRMAX];
	char subs[LOG_STRMAX];
};

struct log_config {
	int sev[LOG_SEV_NUM];
};

#define LOG_CONFIG_ALL(debug, info, notice, warning, error, critical, alert, fatal) \
	(struct log_config){ .sev = { \
		[LOG_DEBUG] = (debug), \
		[LOG_INFO] = (info), \
		[LOG_NOTICE] = (notice), \
		[LOG_WARNING] = (warning), \
		[LOG_ERROR] = (error), \
		[LOG_CRITICAL] = (critical), \
		[LOG_ALERT] = (alert), \
		[LOG_FATAL] = (fatal), \
	} }

#define LOG_CONFIG_DEBUG(debug) \
	LOG_CONFIG_ALL((debug), 2, 2, 2, 2, 2, 2, 2)
#define LOG_CONFIG_INFO(debug, info) \
	LOG_CONFIG_ALL((debug), (info), 2, 2, 2, 2, 2, 2)
#define LOG_CONFIG_WARNING(debug, info, notice, warning) \
	LOG_CONFIG_ALL((debug), (info), (notice), (warning), 2, 2, 2, 2)

void log_set_config(const struct log_config *config);
int log_add_filter(const struct log_filter *filter,
			const struct log_config *config);
void log_rm_filter(int handle);
void log_clean_filter();

/*
 * Log-Functions
 * These functions pass a log-message to the log-subsystem. Handy helpers are
 * provided below. You almost never use these directly.
 *
 * log_submit:
 * Submit the message to the log-subsystem. This is the backend of all other
 * loggers.
 *
 * log_format:
 * Same as log_submit but first converts the arguments into a va_list object.
 *
 * log_llog:
 * Same as log_submit but used as connection to llog. It uses the default config
 * for every message.
 *
 * log_set_file(file):
 * This opens the file specified by \file and redirects all new messages to this
 * file. If \file is NULL, then the default is used which is stderr.
 * Messages are appended to the file and no file-locks are used so you cannot
 * use a single file for multiple processes.
 * No log-file-rotations or other backup/rotation functions are supported. Use a
 * proper init system like systemd to do this.
 *
 * log_print_init(appname):
 * This prints a message with build-time/date and appname to the log. You should
 * invoke this very early in your program. It is not required, though. However,
 * every message is prepended with a time-offset since application-start. This
 * offset is measured since the first log-message is sent so you should send
 * some log-message at application start. This is a handy-helper to do this.
 */

__attribute__((format(printf, 7, 0)))
void log_submit(const char *file,
		int line,
		const char *func,
		const struct log_config *config,
		const char *subs,
		unsigned int sev,
		const char *format,
		va_list args);

__attribute__((format(printf, 7, 8)))
void log_format(const char *file,
		int line,
		const char *func,
		const struct log_config *config,
		const char *subs,
		unsigned int sev,
		const char *format,
		...);

__attribute__((format(printf, 7, 0)))
void log_llog(void *data,
	      const char *file,
	      int line,
	      const char *func,
	      const char *subs,
	      unsigned int sev,
	      const char *format,
	      va_list args);

int log_set_file(const char *file);
void log_print_init(const char *appname);

static inline __attribute__((format(printf, 2, 3)))
void log_dummyf(unsigned int sev, const char *format, ...)
{
}

/*
 * Default values
 * All helpers automatically pick-up the file, line, func, config and subsystem
 * parameters for a log-message. file, line and func are generated with
 * __FILE__, __LINE__ and __func__ and should almost never be replaced. The
 * config argument is by default NULL so global filters apply. You can use the
 *   #define LOG_CONFIG ...
 * method to overwrite this. It is described above in detail.
 * The subsystem is by default an empty string. To overwrite this, add this
 * line to the top of your source file:
 *   #define LOG_SUBSYSTEM "mysubsystem"
 * Then all following log-messages will use this string as subsystem.
 *
 * If you want to change one of these, you need to directly use log_submit and
 * log_format. If you want the defaults for file, line and func you can use:
 *   log_format(LOG_DEFAULT_BASE, config, subsys, sev, format, ...);
 * If you want the default config, use:
 *   log_format(LOG_DEFAULT_CONF, subsys, sev, format, ...);
 * If you want all default values, use:
 *   log_format(LOG_DEFAULT, sev, format, ...);
 *
 * If you want to change a single value, this is the default line that is used
 * internally. Adjust it to your needs:
 *   log_format(__FILE__, __LINE__, __func__, &LOG_CONFIG, LOG_SUBSYSTEM,
 *              LOG_ERROR, "your format string: %s %d", "some args", 5, ...);
 *
 * log_printf is the same as log_format(LOG_DEFAULT, sev, format, ...) and is
 * the most basic wrapper that you can use.
 */

#ifndef LOG_CONFIG
extern const struct log_config LOG_CONFIG;
#endif

#ifndef LOG_SUBSYSTEM
extern const char *LOG_SUBSYSTEM;
#endif

#define LOG_DEFAULT_BASE __FILE__, __LINE__, __func__
#define LOG_DEFAULT_CONF LOG_DEFAULT_BASE, &LOG_CONFIG
#define LOG_DEFAULT LOG_DEFAULT_CONF, LOG_SUBSYSTEM

#define log_printf(sev, format, ...) \
	log_format(LOG_DEFAULT, (sev), (format), ##__VA_ARGS__)

/*
 * Helpers
 * The pick-up all the default values and submit the message to the
 * log-subsystem. The log_debug() function produces zero-code if
 * BUILD_ENABLE_DEBUG is not defined. Therefore, it can be heavily used for
 * debugging and will not have any side-effects.
 */

#ifdef BUILD_ENABLE_DEBUG
	#define log_debug(format, ...) \
		log_printf(LOG_DEBUG, (format), ##__VA_ARGS__)
#else
	#define log_debug(format, ...) \
		log_dummyf(LOG_DEBUG, (format), ##__VA_ARGS__)
#endif

#define log_info(format, ...) \
	log_printf(LOG_INFO, (format), ##__VA_ARGS__)
#define log_notice(format, ...) \
	log_printf(LOG_NOTICE, (format), ##__VA_ARGS__)
#define log_warning(format, ...) \
	log_printf(LOG_WARNING, (format), ##__VA_ARGS__)
#define log_error(format, ...) \
	log_printf(LOG_ERROR, (format), ##__VA_ARGS__)
#define log_critical(format, ...) \
	log_printf(LOG_CRITICAL, (format), ##__VA_ARGS__)
#define log_alert(format, ...) \
	log_printf(LOG_ALERT, (format), ##__VA_ARGS__)
#define log_fatal(format, ...) \
	log_printf(LOG_FATAL, (format), ##__VA_ARGS__)

#define log_dbg log_debug
#define log_warn log_warning
#define log_err log_error
#define log_crit log_critical

#endif /* SHL_LOG_H_INCLUDED */
