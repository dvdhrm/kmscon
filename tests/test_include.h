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
#include "shl_log.h"

#define TEST_HELP \
	"\t-h, --help                  [off]   Print this help and exit\n" \
	"\t-v, --verbose               [off]   Print verbose messages\n" \
	"\t    --debug                 [off]   Enable debug mode\n" \
	"\t    --silent                [off]   Suppress notices and warnings\n"

static struct {
	bool help;
	bool exit;
	bool verbose;
	bool debug;
	bool silent;
} test_conf;

static struct conf_ctx *test_ctx;

static int aftercheck_debug(struct conf_option *opt, int argc, char **argv,
			    int idx)
{
	/* --debug implies --verbose */
	if (test_conf.debug)
		test_conf.verbose = 1;

	return 0;
}

static int aftercheck_help(struct conf_option *opt, int argc, char **argv,
			   int idx)
{
	/* exit after printing --help information */
	if (test_conf.help) {
		print_help();
		test_conf.exit = true;
	}

	return 0;
}

#define TEST_OPTIONS \
	CONF_OPTION_BOOL_FULL('h', "help", aftercheck_help, NULL, NULL, &test_conf.help, false), \
	CONF_OPTION_BOOL('v', "verbose", &test_conf.verbose, false), \
	CONF_OPTION_BOOL_FULL(0, "debug", aftercheck_debug, NULL, NULL, &test_conf.debug, false), \
	CONF_OPTION_BOOL(0, "silent", &test_conf.silent, false)

static void sig_generic(struct ev_eloop *p, struct signalfd_siginfo *info,
			void *data)
{
	struct ev_eloop *eloop = data;

	ev_eloop_exit(eloop);
	log_info("terminating due to caught signal %d", info->ssi_signo);
}

static int test_prepare(struct conf_option *opts, size_t len,
			int argc, char **argv, struct ev_eloop **out)
{
	int ret;
	struct ev_eloop *eloop;

	ret = conf_ctx_new(&test_ctx, opts, len, &test_conf);
	if (ret)
		return ret;

	ret = conf_ctx_parse_argv(test_ctx, argc, argv);
	if (ret)
		goto err_out;

	if (test_conf.exit) {
		ret = -ECANCELED;
		goto err_out;
	}

	if (!test_conf.debug && !test_conf.verbose && test_conf.silent)
		log_set_config(&LOG_CONFIG_WARNING(0, 0, 0, 0));
	else
		log_set_config(&LOG_CONFIG_INFO(test_conf.debug,
						test_conf.verbose));

	log_print_init(argv[0]);

	ret = ev_eloop_new(&eloop, log_llog, NULL);
	if (ret)
		goto err_out;

	ret = ev_eloop_register_signal_cb(eloop, SIGTERM, sig_generic, eloop);
	if (ret)
		goto err_unref;

	ret = ev_eloop_register_signal_cb(eloop, SIGINT, sig_generic, eloop);
	if (ret) {
		ev_eloop_unregister_signal_cb(eloop, SIGTERM,
						sig_generic, eloop);
		goto err_unref;
	}

	*out = eloop;
	return 0;

err_unref:
	ev_eloop_unref(eloop);
err_out:
	conf_ctx_free(test_ctx);
	return ret;
}

static void test_fail(int ret)
{
	if (ret)
		log_err("init failed, errno %d: %s", ret, strerror(-ret));
}

static void test_exit(struct conf_option *opts, size_t len,
		      struct ev_eloop *eloop)
{
	ev_eloop_unregister_signal_cb(eloop, SIGINT, sig_generic, eloop);
	ev_eloop_unregister_signal_cb(eloop, SIGTERM, sig_generic, eloop);
	ev_eloop_unref(eloop);
	conf_ctx_free(test_ctx);
}
