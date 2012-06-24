/*
 * Library Log/Debug Interface
 * Copyright (c) 2012 David Herrmann <dh.herrmann@googlemail.com>
 * Dedicated to the Public Domain
 */

/*
 * Library Log/Debug Interface
 * Libraries should always avoid producing side-effects. This includes writing
 * log-messages of any kind. However, you often don't want to disable debugging
 * entirely, therefore, the core objects often contain a pointer to a function
 * which performs logging. If that pointer is NULL (default), logging is
 * disabled.
 *
 * This header should never be installed into the system! This is _no_ public
 * header. Instead, copy it into your application if you want and use it there.
 * Your public library API should include something like this:
 *
 *   typedef void (*MYPREFIX_log_t) (const char *file,
 *                                   int line,
 *                                   const char *func,
 *                                   const char *subs,
 *                                   unsigned int sev,
 *                                   const char *format,
 *                                   va_list args);
 *
 * And then the user can supply such a function when creating a new context
 * object of your library or simply supply NULL. Internally, you have a field of
 * type "MYPREFIX_log_t llog" in your main structure. If you pass this to the
 * convenience helpers like llog_dbg(), llog_warn() etc. it will automatically
 * use the "llog" field to print the message. If it is NULL, nothing is done.
 *
 * The arguments of the log-function are defined as:
 *   file: Zero terminated string of the file-name where the log-message
 *         occurred. Can be NULL.
 *   line: Line number of @file where the message occurred. Set to 0 or smaller
 *         if not available.
 *   func: Function name where the log-message occurred. Can be NULL.
 *   subs: Subsystem where the message occurred (zero terminated). Can be NULL.
 *   sev: Severity of log-message. An integer between 0 and 7 as defined below.
 *        These are identical to the linux-kernel severities so there is no need
 *        to include these in your public API. Every app can define them
 *        themself, if they need it.
 *   format: Format string. Must not be NULL.
 *   args: Argument array
 */

#ifndef LLOG_H_INCLUDED
#define LLOG_H_INCLUDED

#include <stdarg.h>
#include <stdlib.h>

enum llog_severity {
	LLOG_FATAL = 0,
	LLOG_ALERT = 1,
	LLOG_CRITICAL = 2,
	LLOG_ERROR = 3,
	LLOG_WARNING = 4,
	LLOG_NOTICE = 5,
	LLOG_INFO = 6,
	LLOG_DEBUG = 7,
	LLOG_SEV_NUM,
};

typedef void (*llog_submit_t) (const char *file,
			       int line,
			       const char *func,
			       const char *subs,
			       unsigned int sev,
			       const char *format,
			       va_list args);

static void llog_format(llog_submit_t llog,
			const char *file,
			int line,
			const char *func,
			const char *subs,
			unsigned int sev,
			const char *format,
			...)
{
	va_list list;

	if (llog) {
		va_start(list, format);
		llog(file, line, func, subs, sev, format, list);
		va_end(list);
	}
}

#ifndef LLOG_SUBSYSTEM
static const char *LLOG_SUBSYSTEM __attribute__((__unused__));
#endif

#define LLOG_DEFAULT __FILE__, __LINE__, __func__, LLOG_SUBSYSTEM

#define llog_printf(obj, sev, format, ...) \
	llog_format((obj)->llog, LLOG_DEFAULT, (sev), (format), ##__VA_ARGS__)

/*
 * Helpers
 * They pick-up all the default values and submit the message to the
 * llog-subsystem. The llog_debug() function produces zero-code if
 * LLOG_ENABLE_DEBUG is not defined. Therefore, it can be heavily used for
 * debugging and will not have any side-effects.
 */

#ifdef LLOG_ENABLE_DEBUG
	#define llog_debug(obj, format, ...) \
		llog_printf((obj), LLOG_DEBUG, (format), ##__VA_ARGS__)
#else
	#define llog_debug(obj, format, ...)
#endif

#define llog_info(obj, format, ...) \
	llog_printf((obj), LLOG_INFO, (format), ##__VA_ARGS__)
#define llog_notice(obj, format, ...) \
	llog_printf((obj), LLOG_NOTICE, (format), ##__VA_ARGS__)
#define llog_warning(obj, format, ...) \
	llog_printf((obj), LLOG_WARNING, (format), ##__VA_ARGS__)
#define llog_error(obj, format, ...) \
	llog_printf((obj), LLOG_ERROR, (format), ##__VA_ARGS__)
#define llog_critical(obj, format, ...) \
	llog_printf((obj), LLOG_CRITICAL, (format), ##__VA_ARGS__)
#define llog_alert(obj, format, ...) \
	llog_printf((obj), LLOG_ALERT, (format), ##__VA_ARGS__)
#define llog_fatal(obj, format, ...) \
	llog_printf((obj), LLOG_FATAL, (format), ##__VA_ARGS__)

#define llog_dbg llog_debug
#define llog_warn llog_warning
#define llog_err llog_error
#define llog_crit llog_critical

#endif /* LLOG_H_INCLUDED */
