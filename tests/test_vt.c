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

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eloop.h"
#include "log.h"
#include "uterm.h"
#include "test_include.h"

int main(int argc, char **argv)
{
	int ret;
	struct ev_eloop *eloop;
	struct uterm_vt_master *vtm;
	struct uterm_vt *vt;

	ret = test_prepare(argc, argv, &eloop);
	if (ret)
		goto err_fail;

	ret = uterm_vt_master_new(&vtm, eloop);
	if (ret)
		goto err_exit;

	ret = uterm_vt_allocate(vtm, &vt, NULL, NULL, NULL);
	if (ret)
		goto err_vtm;

	ret = uterm_vt_activate(vt);
	if (ret)
		log_warn("Cannot switch to VT");

	ev_eloop_run(eloop, -1);

	log_debug("Terminating\n");

	/* switch back to previous VT but wait for eloop to process SIGUSR0 */
	ret = uterm_vt_deactivate(vt);
	if (ret == -EINPROGRESS)
		ev_eloop_run(eloop, 50);

	uterm_vt_unref(vt);
err_vtm:
	uterm_vt_master_unref(vtm);
err_exit:
	test_exit(eloop);
err_fail:
	test_fail(ret);
	return abs(ret);
}
