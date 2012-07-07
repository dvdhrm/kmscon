/*
 * kmscon - Common test functions
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

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include "conf.h"
#include "eloop.h"
#include "log.h"

static void sig_generic(struct ev_eloop *p, struct signalfd_siginfo *info,
			void *data)
{
	struct ev_eloop *eloop = data;

	ev_eloop_exit(eloop);
	log_info("terminating due to caught signal %d", info->ssi_signo);
}

static int test_prepare(int argc, char **argv, struct ev_eloop **out)
{
	int ret;
	struct ev_eloop *eloop;

	ret = conf_parse_argv(argc, argv);
	if (ret)
		return -EINVAL;

	if (conf_global.exit)
		return -1;

	if (!conf_global.debug && !conf_global.verbose && conf_global.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(conf_global.debug,
						conf_global.verbose));

	log_print_init(argv[0]);

	ret = ev_eloop_new(&eloop, log_llog);
	if (ret)
		return ret;

	ret = ev_eloop_register_signal_cb(eloop, SIGTERM, sig_generic, eloop);
	if (ret) {
		ev_eloop_unref(eloop);
		return ret;
	}

	ret = ev_eloop_register_signal_cb(eloop, SIGINT, sig_generic, eloop);
	if (ret) {
		ev_eloop_unregister_signal_cb(eloop, SIGTERM,
						sig_generic, eloop);
		ev_eloop_unref(eloop);
		return ret;
	}

	*out = eloop;
	return 0;
}

static void test_fail(int ret)
{
	if (ret)
		log_err("init failed, errno %d: %s", ret, strerror(-ret));
}

static void test_exit(struct ev_eloop *eloop)
{
	ev_eloop_unregister_signal_cb(eloop, SIGINT, sig_generic, eloop);
	ev_eloop_unregister_signal_cb(eloop, SIGTERM, sig_generic, eloop);
	ev_eloop_unref(eloop);
}
