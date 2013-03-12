/*
 * test_console - Test VT Layer
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
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
 * Test VT Layer
 * This opens a new VT and prints some text on it. You can then change the VT
 * and change back. This is only to test the VT subsystem and event engine.
 * This automatically switches to the new VT. Currently, the display gets
 * frozen because we aren't painting to the framebuffer yet. Use
 * ctrl+alt+FX (or some equivalent) to switch back to X/VT.
 */

static void print_help();

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "eloop.h"
#include "shl_log.h"
#include "uterm_input.h"
#include "uterm_vt.h"
#include "test_include.h"

static void print_help()
{
	/*
	 * Usage/Help information
	 * This should be scaled to a maximum of 80 characters per line:
	 *
	 * 80 char line:
	 *       |   10   |    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "12345678901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 * 80 char line starting with tab:
	 *       |10|    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "\t901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 */
	fprintf(stderr,
		"Usage:\n"
		"\t%1$s [options]\n"
		"\t%1$s -h [options]\n"
		"\n"
		"You can prefix boolean options with \"no-\" to negate it. If an argument is\n"
		"given multiple times, only the last argument matters if not otherwise stated.\n"
		"\n"
		"General Options:\n"
		TEST_HELP
		"\n"
		"VT Options:\n"
		"\t    --vt <vt>               [-]     Path to VT to use\n"
		"\t-s, --switchvt              [off]   Switch automatically to the new VT\n",
		"test_vt");
	/*
	 * 80 char line:
	 *       |   10   |    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "12345678901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 * 80 char line starting with tab:
	 *       |10|    20   |    30   |    40   |    50   |    60   |    70   |    80   |
	 *      "\t901234567890123456789012345678901234567890123456789012345678901234567890\n"
	 */
}

static const char *vtpath = NULL;
static bool switchvt = false;

struct conf_option options[] = {
	TEST_OPTIONS,
	CONF_OPTION_STRING(0, "vt", &vtpath, NULL),
	CONF_OPTION_BOOL('s', "switchvt", &switchvt, false),
};

int main(int argc, char **argv)
{
	int ret;
	struct ev_eloop *eloop;
	struct uterm_vt_master *vtm;
	struct uterm_input *input;
	struct uterm_vt *vt;
	size_t onum;

	onum = sizeof(options) / sizeof(*options);
	ret = test_prepare(options, onum, argc, argv, &eloop);
	if (ret)
		goto err_fail;

	ret = uterm_vt_master_new(&vtm, eloop);
	if (ret)
		goto err_exit;

	ret = uterm_input_new(&input, eloop, "", "", "", "", "", 0, 0);
	if (ret)
		goto err_vtm;

	ret = uterm_vt_allocate(vtm, &vt, UTERM_VT_FAKE | UTERM_VT_REAL,
				"seat0", input, vtpath, NULL, NULL);
	if (ret)
		goto err_input;

	if (switchvt) {
		ret = uterm_vt_activate(vt);
		if (ret == -EINPROGRESS)
			log_debug("VT switch in progress");
		else if (ret)
			log_warn("cannot switch to VT: %d", ret);
	}

	ev_eloop_run(eloop, -1);

	log_debug("Terminating");

	/* switch back to previous VT but wait for eloop to process SIGUSR0 */
	if (switchvt) {
		ret = uterm_vt_deactivate(vt);
		if (ret == -EINPROGRESS)
			ev_eloop_run(eloop, 50);
	}

	uterm_vt_unref(vt);
err_input:
	uterm_input_unref(input);
err_vtm:
	uterm_vt_master_unref(vtm);
err_exit:
	test_exit(options, onum, eloop);
err_fail:
	if (ret != -ECANCELED)
		test_fail(ret);
	return abs(ret);
}
