/*
 * screen.h - a screen of character cells: raw keyboard, a cell buffer,
 * and flushes that send only what changed. Two implementations: the text
 * screen through terminfo (screen.c; it matters at 9600 baud and over
 * telnet; not curses: SVR4 curses counts bytes as columns, which breaks
 * on UTF-8 terminals), and an X11 window (x11/xscreen.c), which adds the
 * mouse.
 */
#ifndef MANX_SCREEN_H
#define MANX_SCREEN_H

#include "layout.h"

/* keys besides plain bytes */
enum {
	K_UP = 0x101, K_DOWN, K_LEFT, K_RIGHT, K_PGUP, K_PGDN, K_HOME, K_END,
	K_BTAB, K_DEL, K_INS, K_F1,
	K_MOUSE,		/* button 1 at scr_mouse_row/col */
	K_WHEELUP, K_WHEELDN,	/* the scroll wheel */
	K_CLOSE,		/* the window is being closed; repeats */
	K_SCROLL		/* the scrollbar: show from line scr_scroll_target */
};

/* cell attributes */
#define CA_BOLD		0x01
#define CA_UNDER	0x02
#define CA_REV		0x04
#define CA_LINK		0x08	/* a link's colour (underline without colour) */
#define CA_MARK		0x10	/* a find match's colour (reverse without) */
#define CA_FG(i)	((i) << 8)	/* the page's colour i (scr_palette) */
#define CA_FG_OF(a)	(((a) >> 8) & 255)

extern int scr_rows, scr_cols;
extern enum term_cs scr_cs;
extern int scr_color;			/* 0: no colours even if available */
extern int scr_link_color;		/* ANSI colour 0-7 for links (6: cyan) */
extern int scr_mouse_row, scr_mouse_col;	/* where K_MOUSE was */
extern int scr_mouse_x, scr_mouse_y;	/* ... in the page's pane, in units */
extern int scr_proportional;		/* X11: draw the page in fonts (1) */
extern long scr_scroll_target;		/* where K_SCROLL goes */
extern const char *scr_font;		/* X11: the font (NULL: "fixed") */
extern const char *scr_geometry;	/* X11: "COLSxROWS[+X+Y]" (NULL: 80x25) */
extern const char *scr_needs;		/* what scr_open needs, for errors */

/*
 * Take over the terminal: raw mode, terminfo, the alternate screen.
 * cs_env: "utf-8", "latin1", "ascii" or NULL to ask the terminal (a
 * cursor-position probe) and the locale. 0, or -1 (not a terminal).
 */
int scr_open(const char *cs_env);
void scr_close(void);

/* Check the window size: 1 when it changed (everything will be redrawn). */
int scr_check_size(void);

/* xmanx: fill the screen with the window, or (zoomed) put it back as it
 * was. 0, or -1 where there is no window to zoom. */
int scr_zoom(void);

/*
 * The page's text colours, 0xRRGGBB, for CA_FG(1..n): each screen shows
 * them as it can (a terminal: clear colours in its own eight, greys as
 * its text; X: the colour, darkened if too pale to read on white). The
 * array is the caller's and must stay until the next call.
 */
void scr_palette(const unsigned long *rgb, int n);

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

/* What the window is called (X11; a no-op on a terminal). */
void scr_title(const char *title);

/*
 * The window's own controls (X11; a terminal has none, and these do
 * nothing there): Back, Forward, Reload and Stop buttons, which send u,
 * f, r and z, a URL field, which sends G when clicked, and a scrollbar,
 * which sends K_SCROLL.
 */
void scr_url(const char *url);
void scr_scroll(long top, long rows, long total);
void scr_state(int can_back, int can_forward, int loading);

/*
 * A URL being typed, with the cursor at pos, shown in the URL field: 1, or
 * 0 when the screen has no field (the caller shows it itself). NULL ends
 * the editing.
 */
int scr_url_edit(const char *text, int pos);

/*
 * The page in proportional fonts (X11): the layout measures with
 * scr_metrics(), and the page is drawn with scr_text() into a pane
 * scr_pane_w() x scr_pane_h() units (pixels) between the title row and
 * the status row; cells written over the pane (menus) lie on top of it.
 * NULL from scr_metrics(): a screen of cells only, where the page goes
 * through scr_put() like everything else.
 */
const struct lmetrics *scr_metrics(void);	/* (layout.h) */
int scr_pane_w(void);
int scr_pane_h(void);
/* page text at x, y (the top of its line, whose baseline is ascent
 * below): attr CA_*, face LF_* */
void scr_text(int x, int y, int ascent, const char *s, int n, int attr,
	int face);

/*
 * Images (X11; a terminal shows none). scr_pixels(): the format an
 * image's rows are to come in (image/pixels.h), or NULL when the screen
 * shows no images. scr_image_new(): an image w x h, with a mask if
 * masked, filled a row at a time by scr_image_row() (mask: a bit a pixel,
 * 1 shown; NULL: all of the row shown); NULL when it can't be made. Rows
 * not yet sent show as the background. scr_image_draw(): at x, y of the
 * pane, its top left, w x h; img NULL: a frame where one will be. attr:
 * CA_* (CA_REV: a selected link's highlight).
 */
#ifndef MANX_PIXELS_H		/* (C89 lets it be declared again; Helios C does not) */
struct px_format;
#endif
const struct px_format *scr_pixels(void);
void *scr_image_new(int w, int h, int masked);
void scr_image_row(void *img, int y, const unsigned char *px,
	const unsigned char *mask);
void scr_image_draw(void *img, int x, int y, int w, int h, int attr);
void scr_image_free(void *img);

/* Leave the screen for a moment (e.g. to show a long message) and come
 * back. */
void scr_suspend(void);
void scr_resume(void);

#endif /* MANX_SCREEN_H */
