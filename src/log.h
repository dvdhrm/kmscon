/*
 * kmscon - Log Control
 *
 * Copyright (c) 2011 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
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
 * Log Control
 * This is a fairly simply logging API. It forwards all messages to stderr.
 * They may be prefixed with a priority level like kernel messages.
 * To forward the messages to syslog simply connect stderr to the syslog daemon
 * via your init-manager.
 */

#ifndef KMSCON_LOG_H
#define KMSCON_LOG_H

#include <stdarg.h>
#include <stdlib.h>

#include "config.h"

/* LOG_EMERG and LOG_ALERT do not make sense for this application */
#define LOG_CRIT	"<2>" /* error that cannot be handled correctly */
#define LOG_ERR		"<3>" /* error detected */
#define LOG_WARNING	"<4>" /* warn about unexpected conditions */
#define LOG_NOTICE	"<5>" /* notify about unusual conditions */
#define LOG_INFO	"<6>" /* basic inforomational messages */
#define LOG_DEBUG	"<7>" /* debug messages */

/* dummy logger which allows gcc to check for printk-format */
static inline __attribute__ ((format (printf, 1, 2)))
void log_dummy(const char *format, ...)
{
}

__attribute__ ((format (printf, 1, 2)))
void log_printf(const char *format, ...);
__attribute__ ((format (printf, 1, 0)))
void log_vprintf(const char *format, va_list list);

#define log_crit(format, ...) log_printf(LOG_CRIT format, ##__VA_ARGS__)
#define log_err(format, ...) log_printf(LOG_ERR format, ##__VA_ARGS__)
#define log_warning(format, ...) log_printf(LOG_WARNING format, ##__VA_ARGS__)
#define log_notice(format, ...) log_printf(LOG_NOTICE format, ##__VA_ARGS__)
#define log_info(format, ...) log_printf(LOG_INFO format, ##__VA_ARGS__)

/* log_debug() should produce zero code if DEBUG is disabled */
#ifndef NDEBUG
#define log_debug(format, ...) log_printf(LOG_DEBUG format, ##__VA_ARGS__)
#else
#define log_debug(format, ...) log_dummy(LOG_DEBUG format, ##__VA_ARGS__)
#endif

#endif /* KMSCON_LOG_H */
