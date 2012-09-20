/*
 * test_key - Test client key-input
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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>

int main()
{
	int res;
	unsigned char buf;
	struct termios omode, nmode;
	bool reset = false;

	fprintf(stderr, "Quit with 'q' (maybe followed by 'enter'/'return')\r\n");
	fprintf(stderr, "Maybe your terminal may be unusable after this, use 'reset' to fix it\r\n");

	if (tcgetattr(0, &omode) < 0) {
		fprintf(stderr, "cannot retrieve terminal attributes (%d): %m\r\n",
			errno);
	} else {
		memcpy(&nmode, &omode, sizeof(nmode));
		cfmakeraw(&nmode);
		if (tcsetattr(0, TCSANOW, &nmode) < 0)
			fprintf(stderr, "cannot set terminal attributes (%d): %m\r\n",
				errno);
		else
			reset = true;
	}

	while (1) {
		res = fread(&buf, 1, 1, stdin);
		if (res != 1) {
			fprintf(stderr, "error on stdin: %d %d: %m\r\n",
				res, errno);
			break;
		}

		if (buf == '\n')
			fprintf(stderr, "key: <newline>\r\n");
		else
			fprintf(stderr, "key: %x %u %o '%c'\r\n",
				(int)buf, buf, buf, buf);

		if (buf == 'q')
			break;
	}

	if (reset && tcsetattr(0, TCSANOW, &omode) < 0)
		fprintf(stderr, "cannot reset terminal attributes (%d): %m\r\n",
			errno);

	return 0;
}
