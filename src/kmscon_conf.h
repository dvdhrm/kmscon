/*
 * Main App
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
 * This includes global data for the whole kmscon application. For instance,
 * global parameters can be accessed via this header.
 */

#ifndef KMSCON_MAIN_H
#define KMSCON_MAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include "conf.h"
#include "shl_dlist.h"

enum kmscon_conf_gpu_selection {
	KMSCON_GPU_ALL,
	KMSCON_GPU_AUX,
	KMSCON_GPU_PRIMARY,
};

struct kmscon_conf_t {
	/* header information */
	bool seat_config;

	/* General Options */
	/* show help/usage information */
	bool help;
	/* exit application after parsing options */
	bool exit;
	/* enable verbose info messages */
	bool verbose;
	/* enable debug messages */
	bool debug;
	/* disable notices and warnings */
	bool silent;
	/* config directory name */
	char *configdir;
	/* listen mode */
	bool listen;

	/* Seat Options */
	/* VT number to run on */
	char *vt;
	/* enter new VT directly */
	bool switchvt;
	/* seats */
	char **seats;

	/* Session Options */
	/* sessions */
	unsigned int session_max;
	/* allow keyboard session control */
	bool session_control;
	/* run terminal session */
	bool terminal_session;
	/* cdev session */
	bool cdev_session;

	/* Terminal Options */
	/* custom login process */
	bool login;
	/* argv for login process */
	char **argv;
	/* TERM value */
	char *term;
	/* reset environment */
	bool reset_env;
	/* color palette */
	char *palette;
	/* terminal scroll-back buffer size */
	unsigned int sb_size;

	/* Input Options */
	/* input KBD model */
	char *xkb_model;
	/* input KBD layout */
	char *xkb_layout;
	/* input KBD variant */
	char *xkb_variant;
	/* input KBD options */
	char *xkb_options;
	/* input predefined KBD keymap */
	char *xkb_keymap;
	/* keyboard key-repeat delay */
	unsigned int xkb_repeat_delay;
	/* keyboard key-repeat rate */
	unsigned int xkb_repeat_rate;

	/* Grabs / Keyboard-Shortcuts */
	/* scroll-up grab */
	struct conf_grab *grab_scroll_up;
	/* scroll-down grab */
	struct conf_grab *grab_scroll_down;
	/* page-up grab */
	struct conf_grab *grab_page_up;
	/* page-down grab */
	struct conf_grab *grab_page_down;
	/* zoom-in grab */
	struct conf_grab *grab_zoom_in;
	/* zoom-out grab */
	struct conf_grab *grab_zoom_out;
	/* session-next grab */
	struct conf_grab *grab_session_next;
	/* session-prev grab */
	struct conf_grab *grab_session_prev;
	/* session-dummy grab */
	struct conf_grab *grab_session_dummy;
	/* session-close grab */
	struct conf_grab *grab_session_close;
	/* terminal-new grab */
	struct conf_grab *grab_terminal_new;

	/* Video Options */
	/* use DRM if available */
	bool drm;
	/* use 3D hardware-acceleration if available */
	bool hwaccel;
	/* gpu selection mode */
	unsigned int gpus;
	/* render engine */
	char *render_engine;
	/* print render-engine timing information */
	bool render_timing;

	/* Font Options */
	/* font engine */
	char *font_engine;
	/* font size */
	unsigned int font_size;
	/* font name */
	char *font_name;
	/* font ppi (overrides per monitor PPI) */
	unsigned int font_ppi;
};

int kmscon_conf_new(struct conf_ctx **out);
void kmscon_conf_free(struct conf_ctx *ctx);
int kmscon_conf_load_main(struct conf_ctx *ctx, int argc, char **argv);
int kmscon_conf_load_seat(struct conf_ctx *ctx, const struct conf_ctx *main,
			  const char *seat);

static inline bool kmscon_conf_is_current_seat(struct kmscon_conf_t *conf)
{
	return conf && shl_string_list_is(conf->seats, "current");
}

static inline bool kmscon_conf_is_all_seats(struct kmscon_conf_t *conf)
{
	return conf && shl_string_list_is(conf->seats, "all");
}

static inline bool kmscon_conf_is_single_seat(struct kmscon_conf_t *conf)
{
	return conf && !kmscon_conf_is_all_seats(conf) &&
	       shl_string_list_count(conf->seats, true) == 1;
}

#endif /* KMSCON_MAIN_H */
