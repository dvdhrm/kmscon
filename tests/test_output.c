/*
 * test_output - Test KMS/DRI output
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
 * Test KMS/DRI output subsystem
 * This is an example how to use the output subsystem. Invoked without
 * arguments it prints a list of all connected outputs and their modes.
 * If you pass numbers as arguments, it will enable these outputs and show an
 * image on the given monitors for 5 seconds.
 * The application terminates automatically after 5 seconds, however, you need
 * to switch VT to re-enable output on your screen. This application does not
 * reset the screen automatically, yet.
 *
 * This lists all outputs:
 * $ ./test_output
 *
 * This would show a test screen on output 0 and 4:
 * $ ./test_output 0 4
 * The test screen is a white background with two gray triangles in the top-left
 * and lower-right corner.
 */

#define GL_GLEXT_PROTOTYPES

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include "output.h"

static int set_outputs(struct kmscon_compositor *comp, int num, char **list)
{
	struct kmscon_output *iter;
	int i, j, val, ret;

	j = 0;
	iter = kmscon_compositor_get_outputs(comp);
	for ( ; iter; iter = kmscon_output_next(iter)) {
		for (i = 0; i < num; ++i) {
			val = atoi(list[i]);
			if (val == j)
				break;
		}

		if (i == num) {
			printf("Ignoring output %d\n", j);
		} else {
			printf("Activating output %d %p...\n", j, iter);
			ret = kmscon_output_activate(iter, NULL);
			if (ret)
				printf("Cannot activate output %d: %d\n", j,
									ret);
			else
				printf("Successfully activated output %d\n",
									j);
		}

		++j;
	}

	iter = kmscon_compositor_get_outputs(comp);
	for ( ; iter; iter = kmscon_output_next(iter)) {
		if (!kmscon_output_is_active(iter))
			continue;

		ret = kmscon_output_use(iter);
		if (ret) {
			printf("Cannot use output %p: %d\n", iter, ret);
			continue;
		}

		glClearColor(1.0, 1.0, 1.0, 1.0);
		glClear(GL_COLOR_BUFFER_BIT);

		glBegin(GL_TRIANGLES);
		glColor4f(0.5, 0.5, 0.5, 1.0);
		glVertex3f(1.0, 1.0, 0.0f);
		glVertex3f(0, 0, 0.0f);
		glVertex3f(1.0, 0, 0.0f);
		glVertex3f(-1.0, -1.0, 0.0f);
		glVertex3f(0, 0, 0.0f);
		glVertex3f(-1.0, 0, 0.0f);
		glEnd();

		ret = kmscon_output_swap(iter);
		if (ret) {
			printf("Cannot swap buffers of output %p: %d\n",
								iter, ret);
			continue;
		}

		printf("Successfully set screen on output %p\n", iter);
	}

	printf("Waiting 5 seconds...\n");
	sleep(5);
	printf("Exiting...\n");

	return 0;
}

static int list_outputs(struct kmscon_compositor *comp)
{
	struct kmscon_output *iter;
	struct kmscon_mode *cur, *mode;
	int i;

	printf("List of Outputs:\n");

	i = 0;
	iter = kmscon_compositor_get_outputs(comp);
	for ( ; iter; iter = kmscon_output_next(iter)) {
		cur = kmscon_output_get_current(iter);
	
		printf("Output %d:\n", i++);
		printf("  active: %d\n", kmscon_output_is_active(iter));
		printf("  has current: %s\n", cur ? "yes" : "no");

		mode = kmscon_output_get_modes(iter);
		for ( ; mode; mode = kmscon_mode_next(mode)) {
			printf("  Mode '%s':\n", kmscon_mode_get_name(mode));
			printf("    x: %u\n", kmscon_mode_get_width(mode));
			printf("    y: %u\n", kmscon_mode_get_height(mode));
		}
	}

	printf("End of Output list\n");

	return 0;
}

int main(int argc, char **argv)
{
	struct kmscon_compositor *comp;
	int ret;

	printf("Creating compositor...\n");
	ret = kmscon_compositor_new(&comp);
	if (ret) {
		printf("Cannot create compositor: %d\n", ret);
		return abs(ret);
	}

	printf("Wakeing up compositor...\n");
	ret = kmscon_compositor_wake_up(comp);
	if (ret < 0) {
		printf("Cannot wakeup compositor: %d\n", ret);
		goto err_unref;
	}

	kmscon_compositor_use(comp);

	if (argc < 2) {
		ret = list_outputs(comp);
		if (ret) {
			printf("Cannot list outputs: %d\n", ret);
			goto err_unref;
		}
	} else {
		ret = set_outputs(comp, argc - 1, &argv[1]);
		if (ret) {
			printf("Cannot set outputs: %d\n", ret);
			goto err_unref;
		}
	}

err_unref:
	kmscon_compositor_unref(comp);
	return abs(ret);
}
