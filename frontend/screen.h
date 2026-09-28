/*
 * screen.h - a text screen through terminfo: raw keyboard, a cell buffer,
 * and flushes that send only what changed (it matters at 9600 baud and
 * over telnet). Not curses: SVR4 curses counts bytes as columns, which
 * breaks on UTF-8 terminals.
 */
#ifndef UB_SCREEN_H
#define UB_SCREEN_H

#include "layout.h"

/* keys besides plain bytes */
enum {
	K_UP = 0x101, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END,
	K_BTAB, K_DEL, K_INS, K_F1
};

/* cell attributes */
#define CA_BOLD		0x01
#define CA_UNDER	0x02
#define CA_REV		0x04
#define CA_LINK		0x08	/* a link's colour (underline without colour) */
#define CA_MARK		0x10	/* a find match's colour (reverse without) */

extern int scr_rows, scr_cols;
extern enum term_cs scr_cs;
extern int scr_color;			/* 0: no colours even if available */

/*
 * Take over the terminal: raw mode, terminfo, the alternate screen.
 * cs_env: "utf-8", "latin1", "ascii" or NULL to ask the terminal (a
 * cursor-position probe) and the locale. 0, or -1 (not a terminal).
 */
int scr_open(const char *cs_env);
void scr_close(void);

/* Check the window size: 1 when it changed (everything will be redrawn). */
int scr_check_size(void);

/* Build the next frame: */
void scr_erase(void);
/* text in the terminal's character set at row/col; returns the columns
 * used (it stops at the right edge) */
int scr_put(int row, int col, const char *s, int n, int attr);
void scr_fill(int row, int col, int ncols, int ch, int attr);
/* where the cursor is left (-1: hidden) */
void scr_cursor(int row, int col);
/* ... and send the differences. full: redraw everything. */
void scr_flush(int full);

/* The next key, waiting up to timeout_ms (-1: forever); -1 on timeout. */
int scr_getkey(int timeout_ms);

void scr_bell(void);

/* Leave the screen for a moment (e.g. to show a long message) and come
 * back. */
void scr_suspend(void);
void scr_resume(void);

#endif /* UB_SCREEN_H */
