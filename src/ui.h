/*
 * User Interface
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

/*
 * User Interface
 * This is the main UI object that handles all currently opened terminals and
 * draws the screen.
 */

#ifndef KMSCON_UI_H
#define KMSCON_UI_H

#include <stdlib.h>
#include <unistd.h>
#include "eloop.h"
#include "uterm.h"

struct kmscon_ui;

int kmscon_ui_new(struct kmscon_ui **out,
			struct ev_eloop *eloop,
			struct uterm_input *input);
void kmscon_ui_free(struct kmscon_ui *ui);
void kmscon_ui_add_video(struct kmscon_ui *ui, struct uterm_video *video);
void kmscon_ui_remove_video(struct kmscon_ui *ui, struct uterm_video *video);

#endif /* KMSCON_UI_H */
