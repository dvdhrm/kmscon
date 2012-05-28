/*
 * kmscon - VT Emulator
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
 * Virtual Terminal Emulator
 * This is the VT implementation. It is written from scratch. It uses the
 * console subsystem as output and is tightly bound to it. It supports
 * functionality from vt100 up to vt500 series. It doesn't implement an
 * explicitly selected terminal but tries to support the most important commands
 * to be compatible with existing implementations. However, full vt102
 * compatibility is the least that is provided.
 *
 * The main parser in this file controls the parser-state and dispatches the
 * actions to the related handlers. The parser is based on the state-diagram
 * from Paul Williams: http://vt100.net/emu/
 * It is written from scratch, though.
 * This parser is fully compatible up to the vt500 series. It requires UTF-8 and
 * does not support any other input encoding. The G0 and G1 sets are therefore
 * defined as subsets of UTF-8. You may still map G0-G3 into GL, though.
 *
 * However, the CSI/DCS/etc handlers are not designed after a specific VT
 * series. We try to support all vt102 commands but implement several other
 * often used sequences, too. Feel free to add further.
 *
 * See ./doc/vte.txt for more information on this VT-emulator.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <X11/keysym.h>

#include "console.h"
#include "font.h"
#include "log.h"
#include "unicode.h"
#include "vte.h"

#define LOG_SUBSYSTEM "vte"

/* Input parser states */
enum parser_state {
	STATE_NONE,		/* placeholder */
	STATE_GROUND,		/* initial state and ground */
	STATE_ESC,		/* ESC sequence was started */
	STATE_ESC_INT,		/* intermediate escape characters */
	STATE_CSI_ENTRY,	/* starting CSI sequence */
	STATE_CSI_PARAM,	/* CSI parameters */
	STATE_CSI_INT,		/* intermediate CSI characters */
	STATE_CSI_IGNORE,	/* CSI error; ignore this CSI sequence */
	STATE_DCS_ENTRY,	/* starting DCS sequence */
	STATE_DCS_PARAM,	/* DCS parameters */
	STATE_DCS_INT,		/* intermediate DCS characters */
	STATE_DCS_PASS,		/* DCS data passthrough */
	STATE_DCS_IGNORE,	/* DCS error; ignore this DCS sequence */
	STATE_OSC_STRING,	/* parsing OCS sequence */
	STATE_ST_IGNORE,	/* unimplemented seq; ignore until ST */
	STATE_NUM
};

/* Input parser actions */
enum parser_action {
	ACTION_NONE,		/* placeholder */
	ACTION_IGNORE,		/* ignore the character entirely */
	ACTION_PRINT,		/* print the character on the console */
	ACTION_EXECUTE,		/* execute single control character (C0/C1) */
	ACTION_CLEAR,		/* clear current parameter state */
	ACTION_COLLECT,		/* collect intermediate character */
	ACTION_PARAM,		/* collect parameter character */
	ACTION_ESC_DISPATCH,	/* dispatch escape sequence */
	ACTION_CSI_DISPATCH,	/* dispatch csi sequence */
	ACTION_DCS_START,	/* start of DCS data */
	ACTION_DCS_COLLECT,	/* collect DCS data */
	ACTION_DCS_END,		/* end of DCS data */
	ACTION_OSC_START,	/* start of OSC data */
	ACTION_OSC_COLLECT,	/* collect OSC data */
	ACTION_OSC_END,		/* end of OSC data */
	ACTION_NUM
};

/* max CSI arguments */
#define CSI_ARG_MAX 16

/* terminal flags */
#define FLAG_CURSOR_KEY_MODE			0x01
#define FLAG_KEYPAD_APPLICATION_MODE		0x02 /* TODO: toggle on numlock? */
#define FLAG_LINE_FEED_NEW_LINE_MODE		0x04

struct kmscon_vte {
	unsigned long ref;
	struct kmscon_console *con;

	const char *kbd_sym;
	struct kmscon_utf8_mach *mach;

	unsigned int state;
	unsigned int csi_argc;
	int csi_argv[CSI_ARG_MAX];

	struct font_char_attr cattr;
	unsigned int flags;
};

int kmscon_vte_new(struct kmscon_vte **out, struct kmscon_console *con)
{
	struct kmscon_vte *vte;
	int ret;

	if (!out || !con)
		return -EINVAL;

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;

	memset(vte, 0, sizeof(*vte));
	vte->ref = 1;
	vte->state = STATE_GROUND;
	vte->con = con;

	vte->cattr.fr = 255;
	vte->cattr.fg = 255;
	vte->cattr.fb = 255;
	vte->cattr.br = 0;
	vte->cattr.bg = 0;
	vte->cattr.bb = 0;
	vte->cattr.bold = 0;
	vte->cattr.underline = 0;
	vte->cattr.inverse = 0;

	ret = kmscon_utf8_mach_new(&vte->mach);
	if (ret)
		goto err_free;

	log_debug("new vte object");
	kmscon_console_ref(vte->con);
	*out = vte;
	return 0;

err_free:
	free(vte);
	return ret;
}

void kmscon_vte_ref(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	vte->ref++;
}

void kmscon_vte_unref(struct kmscon_vte *vte)
{
	if (!vte || !vte->ref)
		return;

	if (--vte->ref)
		return;

	log_debug("destroying vte object");
	kmscon_console_unref(vte->con);
	kmscon_utf8_mach_free(vte->mach);
	kmscon_symbol_free_u8(vte->kbd_sym);
	free(vte);
}

/* execute control character (C0 or C1) */
static void do_execute(struct kmscon_vte *vte, uint32_t ctrl)
{
	switch (ctrl) {
		case 0x00: /* NUL */
			/* Ignore on input */
			break;
		case 0x05: /* ENQ */
			/* Transmit answerback message */
			/* TODO */
			break;
		case 0x07: /* BEL */
			/* Sound bell tone */
			/* TODO: I always considered this annying, however, we
			 * should at least provide some way to enable it if the
			 * user *really* wants it.
			 */
			break;
		case 0x08: /* BS */
			/* Move cursor one position left */
			/* TODO: Do we really have auto-wrap feature here? vt520
			 * doc says nothing about it. We should probably use
			 * kmscon_console_move_left() here.
			 */
			kmscon_console_backspace(vte->con);
			break;
		case 0x09: /* HT */
			/* Move to next tab stop or end of line */
			/* TODO */
			break;
		case 0x0a: /* LF */
		case 0x0b: /* VT */
		case 0x0c: /* FF */
			/* Line feed or newline (CR/NL mode) */
			if (vte->flags & FLAG_LINE_FEED_NEW_LINE_MODE)
				kmscon_console_newline(vte->con);
			else
				kmscon_console_move_down(vte->con, 1, true);
			break;
		case 0x0d: /* CR */
			/* Move cursor to left margin */
			kmscon_console_move_line_home(vte->con);
			break;
		case 0x0e: /* SO */
			/* Map G1 character set into GL */
			/* TODO */
			break;
		case 0x0f: /* SI */
			/* Map G0 character set into Gl */
			/* TODO */
			break;
		case 0x11: /* XON */
			/* Resume transmission */
			/* TODO */
			break;
		case 0x13: /* XOFF */
			/* Stop transmission */
			/* TODO */
			break;
		case 0x18: /* CAN */
			/* Cancel escape sequence */
			/* nothing to do here */
			break;
		case 0x1a: /* SUB */
			/* Discard current escape sequence and show err-sym */
			/* TODO: show reversed question mark */
			kmscon_console_write(vte->con, '?', &vte->cattr);
			break;
		case 0x1b: /* ESC */
			/* Invokes an escape sequence */
			/* nothing to do here */
			break;
		case 0x84: /* IND */
			/* Move down one row, perform scroll-up if needed */
			/* TODO */
			break;
		case 0x85: /* NEL */
			/* CR/NL with scroll-up if needed */
			/* TODO */
			break;
		case 0x88: /* HTS */
			/* Set tab stop at current position */
			/* TODO */
			break;
		case 0x8d: /* RI */
			/* Move up one row, perform scroll-down if needed */
			/* TODO */
			break;
		case 0x8e: /* SS2 */
			/* Temporarily map G2 into GL for next char only */
			/* TODO */
			break;
		case 0x8f: /* SS3 */
			/* Temporarily map G3 into GL for next char only */
			/* TODO */
			break;
		case 0x9a: /* DECID */
			/* Send device attributes response like ANSI DA */
			/* TODO*/
			break;
		case 0x9c: /* ST */
			/* End control string */
			/* nothing to do here */
			break;
		default:
			log_warn("unhandled control char %u", ctrl);
	}
}

static void do_clear(struct kmscon_vte *vte)
{
	int i;

	vte->csi_argc = 0;
	for (i = 0; i < CSI_ARG_MAX; ++i)
		vte->csi_argv[i] = -1;
}

static void do_collect(struct kmscon_vte *vte, uint32_t data)
{
}

static void do_param(struct kmscon_vte *vte, uint32_t data)
{
	int new;

	if (data == ';') {
		if (vte->csi_argc < CSI_ARG_MAX)
			vte->csi_argc++;
		return;
	}

	if (vte->csi_argc >= CSI_ARG_MAX)
		return;

	/* avoid integer overflows; max allowed value is 16384 anyway */
	if (vte->csi_argv[vte->csi_argc] > 0xffff)
		return;

	if (data >= '0' && data <= '9') {
		new = vte->csi_argv[vte->csi_argc];
		if (new <= 0)
			new = data - '0';
		else
			new = new * 10 + data - '0';
		vte->csi_argv[vte->csi_argc] = new;
	}
}

static void do_esc(struct kmscon_vte *vte, uint32_t data)
{
	switch (data) {
		case 'D': /* IND */
			/* Move down one row, perform scroll-up if needed */
			/* TODO */
			break;
		case 'E': /* NEL */
			/* CR/NL with scroll-up if needed */
			/* TODO */
			break;
		case 'H': /* HTS */
			/* Set tab stop at current position */
			/* TODO */
			break;
		case 'M': /* RI */
			/* Move up one row, perform scroll-down if needed */
			/* TODO */
			break;
		case 'N': /* SS2 */
			/* Temporarily map G2 into GL for next char only */
			/* TODO */
			break;
		case 'O': /* SS3 */
			/* Temporarily map G3 into GL for next char only */
			/* TODO */
			break;
		case 'Z': /* DECID */
			/* Send device attributes response like ANSI DA */
			/* TODO*/
			break;
		case '\\': /* ST */
			/* End control string */
			/* nothing to do here */
			break;
		default:
			log_warn("unhandled escape seq %u", data);
	}
}

static void do_csi(struct kmscon_vte *vte, uint32_t data)
{
	int num, i;

	if (vte->csi_argc < CSI_ARG_MAX)
		vte->csi_argc++;

	switch (data) {
		case 'A':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_up(vte->con, num, false);
			break;
		case 'B':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_down(vte->con, num, false);
			break;
		case 'C':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_right(vte->con, num);
			break;
		case 'D':
			num = vte->csi_argv[0];
			if (num <= 0)
				num = 1;
			kmscon_console_move_left(vte->con, num);
			break;
		case 'J':
			if (vte->csi_argv[0] <= 0)
				kmscon_console_erase_cursor_to_screen(vte->con);
			else if (vte->csi_argv[0] == 1)
				kmscon_console_erase_screen_to_cursor(vte->con);
			else if (vte->csi_argv[0] == 2)
				kmscon_console_erase_screen(vte->con);
			else
				log_debug("unknown parameter to CSI-J: %d",
					  vte->csi_argv[0]);
			break;
		case 'K':
			if (vte->csi_argv[0] <= 0)
				kmscon_console_erase_cursor_to_end(vte->con);
			else if (vte->csi_argv[0] == 1)
				kmscon_console_erase_home_to_cursor(vte->con);
			else if (vte->csi_argv[0] == 2)
				kmscon_console_erase_current_line(vte->con);
			else
				log_debug("unknown parameter to CSI-K: %d",
					  vte->csi_argv[0]);
			break;
		case 'm':
			for (i = 0; i < CSI_ARG_MAX; ++i) {
				switch (vte->csi_argv[i]) {
				case -1:
					break;
				case 0:
					vte->cattr.fr = 255;
					vte->cattr.fg = 255;
					vte->cattr.fb = 255;
					vte->cattr.br = 0;
					vte->cattr.bg = 0;
					vte->cattr.bb = 0;
					vte->cattr.bold = 0;
					vte->cattr.underline = 0;
					vte->cattr.inverse = 0;
					break;
				case 1:
					vte->cattr.bold = 1;
					break;
				case 4:
					vte->cattr.underline = 1;
					break;
				case 7:
					vte->cattr.inverse = 1;
					break;
				case 22:
					vte->cattr.bold = 0;
					break;
				case 24:
					vte->cattr.underline = 0;
					break;
				case 27:
					vte->cattr.inverse = 0;
					break;
				case 30:
					vte->cattr.fr = 0;
					vte->cattr.fg = 0;
					vte->cattr.fb = 0;
					break;
				case 31:
					vte->cattr.fr = 205;
					vte->cattr.fg = 0;
					vte->cattr.fb = 0;
					break;
				case 32:
					vte->cattr.fr = 0;
					vte->cattr.fg = 205;
					vte->cattr.fb = 0;
					break;
				case 33:
					vte->cattr.fr = 205;
					vte->cattr.fg = 205;
					vte->cattr.fb = 0;
					break;
				case 34:
					vte->cattr.fr = 0;
					vte->cattr.fg = 0;
					vte->cattr.fb = 238;
					break;
				case 35:
					vte->cattr.fr = 205;
					vte->cattr.fg = 0;
					vte->cattr.fb = 205;
					break;
				case 36:
					vte->cattr.fr = 0;
					vte->cattr.fg = 205;
					vte->cattr.fb = 205;
					break;
				case 37:
					vte->cattr.fr = 255;
					vte->cattr.fg = 255;
					vte->cattr.fb = 255;
					break;
				default:
					log_debug("unhandled SGR attr %i",
						  vte->csi_argv[i]);
				}
			}
			break;
		default:
			log_debug("unhandled CSI sequence %c", data);
	}
}

/* perform parser action */
static void do_action(struct kmscon_vte *vte, uint32_t data, int action)
{
	kmscon_symbol_t sym;

	switch (action) {
		case ACTION_NONE:
			/* do nothing */
			return;
		case ACTION_IGNORE:
			/* ignore character */
			break;
		case ACTION_PRINT:
			sym = kmscon_symbol_make(data);
			kmscon_console_write(vte->con, sym, &vte->cattr);
			break;
		case ACTION_EXECUTE:
			do_execute(vte, data);
			break;
		case ACTION_CLEAR:
			do_clear(vte);
			break;
		case ACTION_COLLECT:
			do_collect(vte, data);
			break;
		case ACTION_PARAM:
			do_param(vte, data);
			break;
		case ACTION_ESC_DISPATCH:
			do_esc(vte, data);
			break;
		case ACTION_CSI_DISPATCH:
			do_csi(vte, data);
			break;
		case ACTION_DCS_START:
			break;
		case ACTION_DCS_COLLECT:
			break;
		case ACTION_DCS_END:
			break;
		case ACTION_OSC_START:
			break;
		case ACTION_OSC_COLLECT:
			break;
		case ACTION_OSC_END:
			break;
		default:
			log_warn("invalid action %d", action);
	}
}

/* entry actions to be performed when entering the selected state */
static const int entry_action[] = {
	[STATE_CSI_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_ENTRY] = ACTION_CLEAR,
	[STATE_DCS_PASS] = ACTION_DCS_START,
	[STATE_ESC] = ACTION_CLEAR,
	[STATE_OSC_STRING] = ACTION_OSC_START,
	[STATE_NUM] = ACTION_NONE,
};

/* exit actions to be performed when leaving the selected state */
static const int exit_action[] = {
	[STATE_DCS_PASS] = ACTION_DCS_END,
	[STATE_OSC_STRING] = ACTION_OSC_END,
	[STATE_NUM] = ACTION_NONE,
};

/* perform state transision and dispatch related actions */
static void do_trans(struct kmscon_vte *vte, uint32_t data, int state, int act)
{
	if (state != STATE_NONE) {
		/* A state transition occurs. Perform exit-action,
		 * transition-action and entry-action. Even when performing a
		 * transition to the same state as the current state we do this.
		 * Use STATE_NONE if this is not the desired behavior.
		 */
		do_action(vte, data, exit_action[vte->state]);
		do_action(vte, data, act);
		do_action(vte, data, entry_action[state]);
		vte->state = state;
	} else {
		do_action(vte, data, act);
	}
}

/*
 * Escape sequence parser
 * This parses the new input character \data. It performs state transition and
 * calls the right callbacks for each action.
 */
static void parse_data(struct kmscon_vte *vte, uint32_t raw)
{
	/* events that may occur in any state */
	switch (raw) {
		case 0x18:
		case 0x1a:
		case 0x80 ... 0x8f:
		case 0x91 ... 0x97:
		case 0x99:
		case 0x9a:
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_EXECUTE);
			return;
		case 0x1b:
			do_trans(vte, raw, STATE_ESC, ACTION_NONE);
			return;
		case 0x98:
		case 0x9e:
		case 0x9f:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		case 0x90:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x9d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x9b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
	}

	/* events that depend on the current state */
	switch (vte->state) {
	case STATE_GROUND:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x80 ... 0x8f:
		case 0x91 ... 0x9a:
		case 0x9c:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_PRINT);
		return;
	case STATE_ESC:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
			return;
		case 0x30 ... 0x4f:
		case 0x51 ... 0x57:
		case 0x59:
		case 0x5a:
		case 0x5c:
		case 0x60 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		case 0x5b:
			do_trans(vte, raw, STATE_CSI_ENTRY, ACTION_NONE);
			return;
		case 0x5d:
			do_trans(vte, raw, STATE_OSC_STRING, ACTION_NONE);
			return;
		case 0x50:
			do_trans(vte, raw, STATE_DCS_ENTRY, ACTION_NONE);
			return;
		case 0x58:
		case 0x5e:
		case 0x5f:
			do_trans(vte, raw, STATE_ST_IGNORE, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_ESC_INT, ACTION_COLLECT);
		return;
	case STATE_ESC_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_ESC_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
		return;
	case STATE_CSI_ENTRY:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		case 0x3a:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_PARAM);
			return;
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_CSI_PARAM, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_PARAM:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3a:
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_CSI_INT, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x3f:
			do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_CSI_DISPATCH);
			return;
		}
		do_trans(vte, raw, STATE_CSI_IGNORE, ACTION_NONE);
		return;
	case STATE_CSI_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_EXECUTE);
			return;
		case 0x20 ... 0x3f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_DCS_ENTRY:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x3a:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_PARAM);
			return;
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_DCS_PARAM, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PARAM:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x30 ... 0x39:
		case 0x3b:
			do_trans(vte, raw, STATE_NONE, ACTION_PARAM);
			return;
		case 0x3a:
		case 0x3c ... 0x3f:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_DCS_INT, ACTION_COLLECT);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_INT:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x2f:
			do_trans(vte, raw, STATE_NONE, ACTION_COLLECT);
			return;
		case 0x30 ... 0x3f:
			do_trans(vte, raw, STATE_DCS_IGNORE, ACTION_NONE);
			return;
		case 0x40 ... 0x7e:
			do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_DCS_PASS, ACTION_NONE);
		return;
	case STATE_DCS_PASS:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7e:
			do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
			return;
		case 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_DCS_COLLECT);
		return;
	case STATE_DCS_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	case STATE_OSC_STRING:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_OSC_COLLECT);
		return;
	case STATE_ST_IGNORE:
		switch (raw) {
		case 0x00 ... 0x17:
		case 0x19:
		case 0x1c ... 0x1f:
		case 0x20 ... 0x7f:
			do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
			return;
		case 0x9c:
			do_trans(vte, raw, STATE_GROUND, ACTION_NONE);
			return;
		}
		do_trans(vte, raw, STATE_NONE, ACTION_IGNORE);
		return;
	}

	log_warn("unhandled input %u in state %d", raw, vte->state);
}

void kmscon_vte_input(struct kmscon_vte *vte, const char *u8, size_t len)
{
	int state;
	uint32_t ucs4;
	size_t i;

	if (!vte || !vte->con)
		return;

	for (i = 0; i < len; ++i) {
		state = kmscon_utf8_mach_feed(vte->mach, u8[i]);
		if (state == KMSCON_UTF8_ACCEPT ||
				state == KMSCON_UTF8_REJECT) {
			ucs4 = kmscon_utf8_mach_get(vte->mach);
			parse_data(vte, ucs4);
		}
	}
}

int kmscon_vte_handle_keyboard(struct kmscon_vte *vte,
	const struct uterm_input_event *ev, const char **u8, size_t *len)
{
	kmscon_symbol_t sym;
	const char *val;

	if (UTERM_INPUT_HAS_MODS(ev, UTERM_CONTROL_MASK)) {
		switch (ev->keysym) {
		case XK_2:
		case XK_space:
			val = "\x00";
			break;
		case XK_a:
		case XK_A: val = "\x01"; break;
		case XK_b:
		case XK_B: val = "\x02"; break;
		case XK_c:
		case XK_C: val = "\x03"; break;
		case XK_d:
		case XK_D: val = "\x04"; break;
		case XK_e:
		case XK_E: val = "\x05"; break;
		case XK_f:
		case XK_F: val = "\x06"; break;
		case XK_g:
		case XK_G: val = "\x07"; break;
		case XK_h:
		case XK_H: val = "\x08"; break;
		case XK_i:
		case XK_I: val = "\x09"; break;
		case XK_j:
		case XK_J: val = "\x0a"; break;
		case XK_k:
		case XK_K: val = "\x0b"; break;
		case XK_l:
		case XK_L: val = "\x0c"; break;
		case XK_m:
		case XK_M: val = "\x0d"; break;
		case XK_n:
		case XK_N: val = "\x0e"; break;
		case XK_o:
		case XK_O: val = "\x0f"; break;
		case XK_p:
		case XK_P: val = "\x10"; break;
		case XK_q:
		case XK_Q: val = "\x11"; break;
		case XK_r:
		case XK_R: val = "\x12"; break;
		case XK_s:
		case XK_S: val = "\x13"; break;
		case XK_t:
		case XK_T: val = "\x14"; break;
		case XK_u:
		case XK_U: val = "\x15"; break;
		case XK_v:
		case XK_V: val = "\x16"; break;
		case XK_w:
		case XK_W: val = "\x17"; break;
		case XK_x:
		case XK_X: val = "\x18"; break;
		case XK_y:
		case XK_Y: val = "\x19"; break;
		case XK_z:
		case XK_Z: val = "\x1a"; break;
		case XK_3:
		case XK_bracketleft:
		case XK_braceleft:
			val = "\x1b";
			break;
		case XK_4:
		case XK_backslash:
		case XK_bar:
			val = "\x1c";
			break;
		case XK_5:
		case XK_bracketright:
		case XK_braceright:
			val = "\x1d";
			break;
		case XK_6:
		case XK_grave:
		case XK_asciitilde:
			val = "\x1e";
			break;
		case XK_7:
		case XK_slash:
		case XK_question:
			val = "\x1f";
			break;
		case XK_8:
			val ="\x7f";
			break;
		default:
			val = NULL;
			break;
		}

		if (val) {
			*u8 = val;
			*len = 1;
			return KMSCON_VTE_SEND;
		}
	}

	switch (ev->keysym) {
		case XK_BackSpace:
			*u8 = "\x08";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Tab:
		case XK_KP_Tab:
			*u8 = "\x09";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Linefeed:
			*u8 = "\x0a";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Clear:
			*u8 = "\x0b";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Pause:
			*u8 = "\x13";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Scroll_Lock:
			/* TODO: do we need scroll lock impl.? */
			*u8 = "\x14";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Sys_Req:
			*u8 = "\x15";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Escape:
			*u8 = "\x1b";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_KP_Enter:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOM";
				*len = 3;
				return KMSCON_VTE_SEND;
			}
			/* fallthrough */
		case XK_Return:
			if (vte->flags & FLAG_LINE_FEED_NEW_LINE_MODE) {
				*u8 = "\x0d\x0a";
				*len = 2;
			} else {
				*u8 = "\x0d";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_Insert:
			*u8 = "\e[2~";
			*len = 4;
			return KMSCON_VTE_SEND;
		case XK_Delete:
			*u8 = "\e[3~";
			*len = 4;
			return KMSCON_VTE_SEND;
		case XK_Page_Up:
			*u8 = "\e[5~";
			*len = 4;
			return KMSCON_VTE_SEND;
		case XK_Page_Down:
			*u8 = "\e[6~";
			*len = 4;
			return KMSCON_VTE_SEND;
		case XK_Up:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				*u8 = "\eOA";
			else
				*u8 = "\e[A";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_Down:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				*u8 = "\eOB";
			else
				*u8 = "\e[B";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_Right:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				*u8 = "\eOC";
			else
				*u8 = "\e[C";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_Left:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				*u8 = "\eOD";
			else
				*u8 = "\e[D";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_KP_Insert:
		case XK_KP_0:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOp";
				*len = 3;
			} else {
				*u8 = "0";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_End:
		case XK_KP_1:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOq";
				*len = 3;
			} else {
				*u8 = "1";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Down:
		case XK_KP_2:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOr";
				*len = 3;
			} else {
				*u8 = "2";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Page_Down:
		case XK_KP_3:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOs";
				*len = 3;
			} else {
				*u8 = "3";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Left:
		case XK_KP_4:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOt";
				*len = 3;
			} else {
				*u8 = "4";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Begin:
		case XK_KP_5:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOu";
				*len = 3;
			} else {
				*u8 = "5";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Right:
		case XK_KP_6:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOv";
				*len = 3;
			} else {
				*u8 = "6";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Home:
		case XK_KP_7:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOw";
				*len = 3;
			} else {
				*u8 = "7";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Up:
		case XK_KP_8:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOx";
				*len = 3;
			} else {
				*u8 = "8";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Page_Up:
		case XK_KP_9:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOy";
				*len = 3;
			} else {
				*u8 = "9";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Subtract:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOm";
				*len = 3;
			} else {
				*u8 = "-";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Separator:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOl";
				*len = 3;
			} else {
				*u8 = ",";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Delete:
		case XK_KP_Decimal:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOn";
				*len = 3;
			} else {
				*u8 = ".";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Equal:
		case XK_KP_Divide:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOj";
				*len = 3;
			} else {
				*u8 = "/";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Multiply:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOo";
				*len = 3;
			} else {
				*u8 = "*";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_KP_Add:
			if (vte->flags & FLAG_KEYPAD_APPLICATION_MODE) {
				*u8 = "\eOk";
				*len = 3;
			} else {
				*u8 = "+";
				*len = 1;
			}
			return KMSCON_VTE_SEND;
		case XK_F1:
		case XK_KP_F1:
			*u8 = "\eOP";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_F2:
		case XK_KP_F2:
			*u8 = "\eOQ";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_F3:
		case XK_KP_F3:
			*u8 = "\eOR";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_F4:
		case XK_KP_F4:
			*u8 = "\eOS";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_KP_Space:
			*u8 = " ";
			*len = 1;
			return KMSCON_VTE_SEND;
		case XK_Home:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				*u8 = "\eOH";
			else
				*u8 = "\e[H";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_End:
			if (vte->flags & FLAG_CURSOR_KEY_MODE)
				*u8 = "\eOF";
			else
				*u8 = "\e[F";
			*len = 3;
			return KMSCON_VTE_SEND;
		case XK_F5:
			*u8 = "\e[15~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F6:
			*u8 = "\e[17~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F7:
			*u8 = "\e[18~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F8:
			*u8 = "\e[19~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F9:
			*u8 = "\e[20~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F10:
			*u8 = "\e[21~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F11:
			*u8 = "\e[23~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F12:
			*u8 = "\e[24~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F13:
			*u8 = "\e[25~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F14:
			*u8 = "\e[26~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F15:
			*u8 = "\e[28~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F16:
			*u8 = "\e[29~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F17:
			*u8 = "\e[31~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F18:
			*u8 = "\e[32~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F19:
			*u8 = "\e[33~";
			*len = 5;
			return KMSCON_VTE_SEND;
		case XK_F20:
			*u8 = "\e[34~";
			*len = 5;
			return KMSCON_VTE_SEND;
	}

	if (ev->unicode != UTERM_INPUT_INVALID) {
		kmscon_symbol_free_u8(vte->kbd_sym);
		sym = kmscon_symbol_make(ev->unicode);
		vte->kbd_sym = kmscon_symbol_get_u8(sym, len);
		*u8 = vte->kbd_sym;
		return KMSCON_VTE_SEND;
	}

	return KMSCON_VTE_DROP;
}
