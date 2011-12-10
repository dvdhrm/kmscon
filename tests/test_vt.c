/*
 * test_console - Test VT Layer
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
 * Test VT Layer
 * This opens a new VT and prints some text on it. You can then change the VT
 * and change back. This is only to test the VT subsystem and event engine.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eloop.h"
#include "log.h"
#include "vt.h"

static bool terminate;

static void sig_term(struct kmscon_signal *sig, int signum, void *data)
{
	terminate = true;
}

int main(int argc, char **argv)
{
	int ret;
	struct kmscon_eloop *loop;
	struct kmscon_vt *vt;
	struct kmscon_signal *sig;

	ret = kmscon_eloop_new(&loop);
	if (ret) {
		log_err("Cannot create eloop\n");
		goto err_out;
	}

	ret = kmscon_eloop_new_signal(loop, &sig, SIGINT, sig_term, NULL);
	if (ret) {
		log_err("Cannot add signal\n");
		goto err_loop;
	}

	ret = kmscon_vt_new(&vt, NULL, NULL);
	if (ret) {
		log_err("Cannot create vt\n");
		goto err_sig;
	}

	ret = kmscon_vt_open(vt, KMSCON_VT_NEW);
	if (ret) {
		log_err("Cannot open VT\n");
		goto err_vt;
	}

	ret = kmscon_vt_connect_eloop(vt, loop);
	if (ret) {
		log_err("Cannot connect VT\n");
		goto err_vt;
	}

	ret = kmscon_vt_switch_enter(vt);
	if (ret)
		log_warning("Cannot switch to VT\n");

	while (!terminate) {
		ret = kmscon_eloop_dispatch(loop, -1);
		if (ret) {
			log_err("Dispatcher failed\n");
			break;
		}
	}

	ret = kmscon_vt_switch_leave(vt);
	if (!ret)
		kmscon_eloop_dispatch(loop, -1);
	else if (ret != -EALREADY)
		log_warning("Cannot switch back from VT\n");

err_vt:
	kmscon_vt_unref(vt);
err_sig:
	kmscon_eloop_rm_signal(sig);
err_loop:
	kmscon_eloop_unref(loop);
err_out:
	return abs(ret);
}
