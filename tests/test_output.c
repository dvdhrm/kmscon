/*
 * test_output - Test KMS/DRI output
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
 * Test KMS/DRI output subsystem
 * This is an example how to use the output subsystem. Invoked without
 * arguments it prints a list of all connected outputs and their modes.
 * If you pass any argument it will enable all outputs for 5seconds.
 *
 * This lists all outputs:
 * $ ./test_output
 *
 * This would show a test screen:
 * $ ./test_output something
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "eloop.h"
#include "log.h"
#include "uterm.h"
#include "test_include.h"

/* eloop object */
static struct ev_eloop *eloop;

static int blit_outputs(struct uterm_video *video)
{
	struct uterm_display *iter;
	int j, ret;
	struct uterm_screen *screen;

	j = 0;
	iter = uterm_video_get_displays(video);
	for ( ; iter; iter = uterm_display_next(iter)) {
		log_notice("Activating display %d %p...", j, iter);
		ret = uterm_display_activate(iter, NULL);
		if (ret)
			log_err("Cannot activate display %d: %d", j, ret);
		else
			log_notice("Successfully activated display %d", j);

		ret = uterm_display_set_dpms(iter, UTERM_DPMS_ON);
		if (ret)
			log_err("Cannot set DPMS to ON: %d", ret);

		++j;
	}

	iter = uterm_video_get_displays(video);
	for ( ; iter; iter = uterm_display_next(iter)) {
		if (uterm_display_get_state(iter) != UTERM_DISPLAY_ACTIVE)
			continue;

		ret = uterm_screen_new_single(&screen, iter);
		if (ret) {
			log_err("Cannot create temp-screen object: %d", ret);
			continue;
		}

		ret = uterm_screen_fill(screen, 0xff, 0xff, 0xff, 0, 0,
					uterm_screen_width(screen),
					uterm_screen_height(screen));
		if (ret) {
			log_err("cannot fill framebuffer");
			uterm_screen_unref(screen);
			continue;
		}

		ret = uterm_screen_swap(screen);
		if (ret) {
			log_err("Cannot swap screen: %d", ret);
			uterm_screen_unref(screen);
			continue;
		}

		log_notice("Successfully set screen on display %p", iter);
		uterm_screen_unref(screen);
	}

	log_notice("Waiting 5 seconds...");
	ev_eloop_run(eloop, 5000);
	log_notice("Exiting...");

	return 0;
}

static int list_outputs(struct uterm_video *video)
{
	struct uterm_display *iter;
	struct uterm_mode *cur, *mode;
	int i;

	log_notice("List of Outputs:");

	i = 0;
	iter = uterm_video_get_displays(video);
	for ( ; iter; iter = uterm_display_next(iter)) {
		cur = uterm_display_get_current(iter);

		log_notice("Output %d:", i++);
		log_notice("  active: %d", uterm_display_get_state(iter));
		log_notice("  has current: %s", cur ? "yes" : "no");

		mode = uterm_display_get_modes(iter);
		for ( ; mode; mode = uterm_mode_next(mode)) {
			log_notice("  Mode '%s':", uterm_mode_get_name(mode));
			log_notice("    x: %u", uterm_mode_get_width(mode));
			log_notice("    y: %u", uterm_mode_get_height(mode));
		}
	}

	log_notice("End of Output list");

	return 0;
}

int main(int argc, char **argv)
{
	struct uterm_video *video;
	int ret;
	unsigned int mode;
	const char *node;

	ret = test_prepare(argc, argv, &eloop);
	if (ret)
		goto err_fail;

	if (conf_global.use_fbdev) {
		mode = UTERM_VIDEO_FBDEV;
		node = "/dev/fb0";
	} else {
		mode = UTERM_VIDEO_DRM;
		node = "/dev/dri/card0";
	}

	log_notice("Creating video object using %s...", node);

	ret = uterm_video_new(&video, eloop, mode, node);
	if (ret) {
		if (mode == UTERM_VIDEO_DRM) {
			log_notice("cannot create drm device; trying dumb drm mode");
			ret = uterm_video_new(&video, eloop,
					      UTERM_VIDEO_DUMB, node);
			if (ret)
				goto err_exit;
		} else {
			goto err_exit;
		}
	}

	log_notice("Wakeing up video object...");
	ret = uterm_video_wake_up(video);
	if (ret < 0)
		goto err_unref;

	if (argc < 2) {
		ret = list_outputs(video);
		if (ret) {
			log_err("Cannot list outputs: %d", ret);
			goto err_unref;
		}
	} else {
		ret = blit_outputs(video);
		if (ret) {
			log_err("Cannot set outputs: %d", ret);
			goto err_unref;
		}
	}

err_unref:
	uterm_video_unref(video);
err_exit:
	test_exit(eloop);
err_fail:
	test_fail(ret);
	return abs(ret);
}
