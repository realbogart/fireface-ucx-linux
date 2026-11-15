/* SPDX-License-Identifier: MIT */
/*
 * A small terminal layer for ucxtui: raw mode, a cell buffer that is drawn
 * as a whole and sent as a difference, and decoded keys and mouse events.
 * No curses: escape sequences every xterm-like terminal takes, with 24-bit
 * colour where the terminal says it has it and the 256-colour cube elsewhere.
 */
#ifndef TUI_TERM_H
#define TUI_TERM_H

#include <stdbool.h>
#include <stdint.h>

/* Colours are 0xRRGGBB */
enum { ATTR_BOLD = 1, ATTR_ITALIC = 2, ATTR_UNDERLINE = 4 };

/* A key is a Unicode code point, or one of these */
enum {
	KEY_NONE = 0,
	KEY_ENTER = 0x200000,
	KEY_TAB,
	KEY_BACKTAB,
	KEY_ESC,
	KEY_BACKSPACE,
	KEY_DELETE,
	KEY_UP,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_HOME,
	KEY_END,
	KEY_PGUP,
	KEY_PGDN,
	KEY_F1,		/* F1..F12 follow each other */
	KEY_F12 = KEY_F1 + 11,
	KEY_MOUSE,	/* a button press: x, y, button */
	KEY_DRAG,	/* the mouse moved to another cell with a button down */
	KEY_MOVE,	/* the same with no button down */
	KEY_RELEASE,
	KEY_WHEEL,	/* x, y, button: -1 up, 1 down */
	KEY_RESIZE,
	KEY_FOCUS_OUT,	/* the terminal window is no longer the active one */
};

enum { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4 };

struct event {
	int key;
	int mods;	/* a control character is its letter with MOD_CTRL */
	int x, y, button;
};

/* Returns -1 if standard input or output is not a terminal */
int term_init(void);
void term_exit(void);
void term_size(int *w, int *h);

/* One event, or false after @timeout_ms without one */
bool term_poll(struct event *ev, int timeout_ms);

/* Drawing goes to a buffer; nothing outside the clip rectangle is touched */
void term_clip(int x, int y, int w, int h);
void term_unclip(void);
void term_put(int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, int attr);
void term_fill(int x, int y, int w, int h, uint32_t ch, uint32_t fg, uint32_t bg);
/* UTF-8 text, at most @maxw cells (cut with an ellipsis); returns the cells used */
int term_text(int x, int y, int maxw, const char *text, uint32_t fg, uint32_t bg, int attr);
/* Change only the background of what is already drawn */
void term_tint(int x, int y, int w, int h, uint32_t bg);
/* Where the real cursor shows after the next flush; x < 0 hides it */
void term_cursor(int x, int y);
void term_flush(void);

/* Cells a UTF-8 string takes (every code point counts as one) */
int term_width(const char *text);

#endif /* TUI_TERM_H */
