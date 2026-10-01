/*
 * xscreen.c - the screen of screen.h in an X11 window: the same cells,
 * drawn with a server font, plus the mouse and the window's controls in
 * the OPEN LOOK manner, which both targets' desktops use (ASV's olvwm,
 * AMIX's olwm): a control area with Back, Forward, Reload and Stop
 * buttons and a URL field, and a scrollbar with an elevator. Raw Xlib,
 * nothing newer than X11R5 (AMIX's static libX11 talks to ASV's X11R6.3
 * server), and no toolkit: OLIT is AMIX's only and XView too big.
 *
 * X core fonts are 8-bit, so the page comes in Latin-1 whatever the
 * locale says.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include "os.h"
#include "entropy.h"
#include "screen.h"

int scr_rows = 25, scr_cols = 80;
int scr_color = 1;
int scr_link_color = 4;		/* blue: links on a white page */
enum term_cs scr_cs = TCS_LATIN1;
int scr_mouse_row, scr_mouse_col;
long scr_scroll_target;
const char *scr_font;
const char *scr_needs = "an X display ($DISPLAY)";

#define PAD	2			/* pixels around the cells */
#define SBW	17			/* the scrollbar's width */
#define CPAD	4			/* around the control area's buttons */

struct cell {
	unsigned char ch;
	unsigned char a;
};

static Display *dpy;
static Window win;
static GC gc;
static XFontStruct *font, *bold;	/* bold: NULL to overstrike */
static int cw, ch, ascent;		/* cell size, baseline */
static Atom wm_delete;
static int win_w, win_h;		/* the window's size */
static int closing;

/* where things are: the control area (0..ctrl_h), the cells (from gx,
 * gy), the scrollbar (sb_x..win_w, gy..win_h) */
static int ctrl_h, bh, gx, gy, sb_x;

static struct cell *cur, *nxt;		/* in the window / being built */
static int cur_row = -1, cur_col = -1;	/* where scr_cursor asked */
static int drawn_row = -1, drawn_col = -1;	/* where the cursor is drawn */

/* the colours: the page, and OPEN LOOK's BG1-3 and highlight */
static unsigned long px_fg, px_bg, px_mark, px_link[8];
static unsigned long px_bg1, px_bg2, px_bg3, px_hi;
static int link_px_ok;

/* ANSI colours 0-7 for links, darkened for a white page */
static const char *const ansi_x[8] = {
	"#000000", "#b00000", "#007000", "#806000",
	"#0000c0", "#a000a0", "#007878", "#606060"
};

/* --- the controls' state ------------------------------------------------- */

struct button {
	const char *label;
	int key;
	int x, w;
	int on;				/* it can act now */
};

static struct button buttons[] = {
	{ "Back", 'u', 0, 0, 0 },
	{ "Forward", 'f', 0, 0, 0 },
	{ "Reload", 'r', 0, 0, 1 },
	{ "Stop", 'z', 0, 0, 0 },
};
#define NBUTTONS ((int)(sizeof buttons / sizeof buttons[0]))

static int pressed = -1;		/* the button held down, or -1 */
static int url_x, url_label_w;		/* the URL field: label at url_x */
static char url_text[512];
static long sc_top, sc_rows, sc_total;	/* the page, for the scrollbar */
static int dragging, drag_dy;		/* the elevator, held */
static int controls_dirty = 1, scroll_dirty = 1;

/* (XParseColor, not XAllocNamedColor: R5's sends "#rrggbb" to the server,
 * whose colour database knows only names) */
static unsigned long color(const char *name, unsigned long fallback, int *ok)
{
	Colormap cm = DefaultColormap(dpy, DefaultScreen(dpy));
	XColor c;

	if (scr_color && DefaultDepth(dpy, DefaultScreen(dpy)) > 1
		&& XParseColor(dpy, cm, (char *)name, &c) && XAllocColor(dpy, cm, &c)) {
		if (ok)
			*ok = 1;
		return c.pixel;
	}
	if (ok)
		*ok = 0;
	return fallback;
}

/* --- opening ------------------------------------------------------------ */

/*
 * AMIX's X11R5 Xlib reaches a local server through /tmp/.X11-unix, which
 * ASV's X11R6.3 server doesn't make: there ":0" has to go over TCP, as
 * "thishost:0".
 */
static Display *open_display(void)
{
	const char *d = getenv("DISPLAY");
	Display *p;
	struct utsname u;
	char buf[300];
	const char *n;

	if ((p = XOpenDisplay(NULL)) != NULL || d == NULL)
		return p;
	if (d[0] == ':')
		n = d;
	else if (strncmp(d, "unix:", 5) == 0)
		n = d + 4;
	else
		return NULL;
	if (uname(&u) < 0 || strlen(u.nodename) + strlen(n) >= sizeof buf)
		return NULL;
	strcpy(buf, u.nodename);
	strcat(buf, n);
	return XOpenDisplay(buf);
}

/* the bold face of f (same name with Bold for Medium), or NULL */
static XFontStruct *bold_of(XFontStruct *f)
{
	unsigned long a;
	char *name, b[256], *m;
	XFontStruct *bf;

	if (!XGetFontProperty(f, XA_FONT, &a))
		return NULL;
	name = XGetAtomName(dpy, (Atom)a);
	if (name == NULL)
		return NULL;
	bf = NULL;
	if (strlen(name) < sizeof b && (m = strstr(name, "-Medium-")) != NULL) {
		strcpy(b, name);
		memcpy(b + (m - name), "-Bold-", 6);
		strcpy(b + (m - name) + 6, m + 8);
		bf = XLoadQueryFont(dpy, b);
		/* only a face of the same cell size will do */
		if (bf && (bf->max_bounds.width != f->max_bounds.width
			|| bf->ascent + bf->descent != f->ascent + f->descent)) {
			XFreeFont(dpy, bf);
			bf = NULL;
		}
	}
	XFree(name);
	return bf;
}

static int alloc_cells(void)
{
	size_t n = (size_t)scr_rows * (size_t)scr_cols, i;

	xfree(cur);
	xfree(nxt);
	cur = xmalloc(n * sizeof *cur);
	nxt = xmalloc(n * sizeof *nxt);
	if (cur == NULL || nxt == NULL)
		return -1;
	for (i = 0; i < n; i++) {
		nxt[i].ch = ' ';
		nxt[i].a = 0;
		cur[i].ch = 0;		/* unknown: redraw */
		cur[i].a = 0xFF;
	}
	drawn_row = drawn_col = -1;
	return 0;
}

/* where the buttons and the URL field go (they don't move with the width) */
static void place_controls(void)
{
	int i, x = 6, r = bh / 2;

	for (i = 0; i < NBUTTONS; i++) {
		buttons[i].x = x;
		buttons[i].w = XTextWidth(font, (char *)buttons[i].label,
			(int)strlen(buttons[i].label)) + 2 * r + 8;
		x += buttons[i].w + 8;
	}
	url_x = x + 8;
	url_label_w = XTextWidth(font, "URL:", 4) + 6;
	ctrl_h = bh + 2 * CPAD + 1;
	gx = PAD;
	gy = ctrl_h + PAD;
}

int scr_open(const char *cs_env)
{
	XSetWindowAttributes wa;
	XSizeHints *sh;
	XWMHints *wh;
	XClassHint *chint;
	XEvent ev;
	int scr, i;

	(void)cs_env;
	if ((dpy = open_display()) == NULL)
		return -1;
	scr = DefaultScreen(dpy);
	font = XLoadQueryFont(dpy, (char *)(scr_font && *scr_font ? scr_font : "fixed"));
	if (font == NULL && (font = XLoadQueryFont(dpy, "fixed")) == NULL) {
		XCloseDisplay(dpy);
		dpy = NULL;
		return -1;
	}
	bold = bold_of(font);
	cw = font->max_bounds.width;
	ch = font->ascent + font->descent;
	ascent = font->ascent;
	bh = ch + 6;
	place_controls();

	px_fg = color("black", BlackPixel(dpy, scr), NULL);
	px_bg = color("white", WhitePixel(dpy, scr), NULL);
	px_mark = color("#ffff60", px_bg, NULL);
	link_px_ok = 1;
	for (i = 0; i < 8; i++) {
		int ok;

		px_link[i] = color(ansi_x[i], px_fg, &ok);
		link_px_ok &= ok;
	}
	/* OPEN LOOK's 3D look on a colour screen; outlines on a mono one */
	px_bg1 = color("#cccccc", px_bg, NULL);
	px_bg2 = color("#b2b2b2", px_bg, NULL);
	px_bg3 = color("#7f7f7f", px_fg, NULL);
	px_hi = color("white", px_bg, NULL);

	win_w = 2 * PAD + scr_cols * cw + SBW;
	win_h = gy + PAD + scr_rows * ch;
	sb_x = win_w - SBW;
	wa.background_pixel = px_bg;
	wa.border_pixel = px_fg;
	wa.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask
		| ButtonMotionMask | ExposureMask | StructureNotifyMask;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0,
		(unsigned)win_w, (unsigned)win_h, 1, CopyFromParent, InputOutput,
		CopyFromParent, CWBackPixel | CWBorderPixel | CWEventMask, &wa);

	/* the window manager: steps of one cell, at least 40x5 */
	if ((sh = XAllocSizeHints()) != NULL) {
		sh->flags = PResizeInc | PMinSize | PBaseSize;
		sh->width_inc = cw;
		sh->height_inc = ch;
		sh->base_width = 2 * PAD + SBW;
		sh->base_height = gy + PAD;
		sh->min_width = sh->base_width + 40 * cw;
		sh->min_height = sh->base_height + 5 * ch;
	}
	if ((wh = XAllocWMHints()) != NULL) {
		wh->flags = InputHint;
		wh->input = True;
	}
	if ((chint = XAllocClassHint()) != NULL) {
		chint->res_name = "manx";
		chint->res_class = "Manx";
	}
	XSetWMProperties(dpy, win, NULL, NULL, NULL, 0, sh, wh, chint);
	if (sh)
		XFree((char *)sh);
	if (wh)
		XFree((char *)wh);
	if (chint)
		XFree((char *)chint);
	XStoreName(dpy, win, "Manx");
	XSetIconName(dpy, win, "Manx");
	wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(dpy, win, &wm_delete, 1);

	gc = XCreateGC(dpy, win, 0, NULL);
	XSetFont(dpy, gc, font->fid);
	if (alloc_cells() < 0) {
		XCloseDisplay(dpy);
		dpy = NULL;
		return -1;
	}
	XMapWindow(dpy, win);
	/* wait until it is there to be drawn on */
	do
		XWindowEvent(dpy, win, ExposureMask, &ev);
	while (ev.xexpose.count != 0);
	scr_flush(1);
	return 0;
}

void scr_close(void)
{
	if (dpy == NULL)
		return;
	XCloseDisplay(dpy);
	dpy = NULL;
}

void scr_suspend(void)
{
	if (dpy)
		XFlush(dpy);
}

void scr_resume(void)
{
	scr_flush(1);
}

static int rows_fit(void)
{
	return (win_h - gy - PAD) / ch;
}

static int cols_fit(void)
{
	return (win_w - SBW - 2 * PAD) / cw;
}

int scr_check_size(void)
{
	int r = rows_fit(), c = cols_fit();

	if (r < 3)
		r = 3;
	if (c < 10)
		c = 10;
	if (r > 200)
		r = 200;
	if (c > 400)
		c = 400;
	sb_x = win_w - SBW;
	if (r == scr_rows && c == scr_cols && cur)
		return 0;
	scr_rows = r;
	scr_cols = c;
	alloc_cells();
	controls_dirty = scroll_dirty = 1;
	return 1;
}

void scr_title(const char *title)
{
	static char last[200];
	char b[sizeof last + 8];

	if (dpy == NULL || title == NULL || strncmp(title, last, sizeof last - 1) == 0)
		return;
	strncpy(last, title, sizeof last - 1);
	last[sizeof last - 1] = '\0';
	if (*last)
		sprintf(b, "%s - Manx", last);
	else
		strcpy(b, "Manx");
	XStoreName(dpy, win, b);
}

/* --- the controls --------------------------------------------------------- */

void scr_url(const char *url)
{
	if (url == NULL || strncmp(url, url_text, sizeof url_text - 1) == 0)
		return;
	strncpy(url_text, url, sizeof url_text - 1);
	url_text[sizeof url_text - 1] = '\0';
	controls_dirty = 1;
}

void scr_state(int can_back, int can_forward, int loading)
{
	int on[NBUTTONS], i;

	on[0] = can_back;
	on[1] = can_forward;
	on[2] = !loading;
	on[3] = loading;
	for (i = 0; i < NBUTTONS; i++)
		if (buttons[i].on != on[i]) {
			buttons[i].on = on[i];
			controls_dirty = 1;
		}
}

void scr_scroll(long top, long rows, long total)
{
	if (top == sc_top && rows == sc_rows && total == sc_total)
		return;
	sc_top = top;
	sc_rows = rows;
	sc_total = total;
	scroll_dirty = 1;
}

/* an OPEN LOOK button: an oblong, lit from the top left (pressed: from the
 * bottom right); a label in grey when it can't act */
static void draw_button(const struct button *b, int down)
{
	int x = b->x, y = CPAD, w = b->w, r = bh / 2;
	unsigned long top = down ? px_bg3 : px_hi, bot = down ? px_hi : px_bg3;
	int tw = XTextWidth(font, (char *)b->label, (int)strlen(b->label));

	XSetForeground(dpy, gc, down ? px_bg2 : px_bg1);
	XFillArc(dpy, win, gc, x, y, (unsigned)bh, (unsigned)bh, 90 * 64, 180 * 64);
	XFillArc(dpy, win, gc, x + w - bh, y, (unsigned)bh, (unsigned)bh, -90 * 64, 180 * 64);
	XFillRectangle(dpy, win, gc, x + r, y, (unsigned)(w - 2 * r), (unsigned)bh);
	/* the upper half lit, the lower half in shadow */
	XSetForeground(dpy, gc, top);
	XDrawArc(dpy, win, gc, x, y, (unsigned)(bh - 1), (unsigned)(bh - 1), 90 * 64, 90 * 64);
	XDrawArc(dpy, win, gc, x + w - bh, y, (unsigned)(bh - 1), (unsigned)(bh - 1), 45 * 64, 45 * 64);
	XDrawLine(dpy, win, gc, x + r, y, x + w - r, y);
	XSetForeground(dpy, gc, bot);
	XDrawArc(dpy, win, gc, x, y, (unsigned)(bh - 1), (unsigned)(bh - 1), 180 * 64, 45 * 64);
	XDrawArc(dpy, win, gc, x + w - bh, y, (unsigned)(bh - 1), (unsigned)(bh - 1), -90 * 64, 135 * 64);
	XDrawLine(dpy, win, gc, x + r, y + bh - 1, x + w - r, y + bh - 1);
	/* on a mono screen the 3D is lost: an outline instead */
	if (px_bg1 == px_bg) {
		XSetForeground(dpy, gc, px_fg);
		XDrawArc(dpy, win, gc, x, y, (unsigned)(bh - 1), (unsigned)(bh - 1), 90 * 64, 180 * 64);
		XDrawArc(dpy, win, gc, x + w - bh, y, (unsigned)(bh - 1), (unsigned)(bh - 1), -90 * 64, 180 * 64);
		XDrawLine(dpy, win, gc, x + r, y, x + w - r, y);
		XDrawLine(dpy, win, gc, x + r, y + bh - 1, x + w - r, y + bh - 1);
	}
	XSetForeground(dpy, gc, b->on ? px_fg : px_bg3);
	XSetFont(dpy, gc, font->fid);
	XDrawString(dpy, win, gc, x + (w - tw) / 2, y + 3 + ascent,
		(char *)b->label, (int)strlen(b->label));
}

/* the control area: the buttons, then "URL:" and the address on a line */
static void draw_controls(void)
{
	int i, fx = url_x + url_label_w, fw = win_w - 8 - fx, n, y = CPAD + 3 + ascent;

	XSetForeground(dpy, gc, px_bg1);
	XFillRectangle(dpy, win, gc, 0, 0, (unsigned)win_w, (unsigned)ctrl_h);
	XSetForeground(dpy, gc, px_bg3);
	XDrawLine(dpy, win, gc, 0, ctrl_h - 1, win_w, ctrl_h - 1);
	for (i = 0; i < NBUTTONS; i++)
		draw_button(&buttons[i], i == pressed);
	XSetFont(dpy, gc, font->fid);
	XSetForeground(dpy, gc, px_fg);
	XDrawString(dpy, win, gc, url_x, y, "URL:", 4);
	if (fw > cw) {
		n = (int)strlen(url_text);
		if (n * cw > fw)
			n = fw / cw;
		XDrawString(dpy, win, gc, fx, y, url_text, n);
		XSetForeground(dpy, gc, px_bg3);
		XDrawLine(dpy, win, gc, fx, y + 3, fx + fw, y + 3);
	}
	controls_dirty = 0;
}

/* the scrollbar's parts: anchors, the cable between them, and the
 * elevator (up arrow, drag area, down arrow) on it */
#define ANCHOR	6
static int cable_top(void) { return gy + ANCHOR + 2; }
static int cable_len(void) { return win_h - PAD - ANCHOR - 2 - cable_top(); }
static int elev_h(void) { return 3 * (SBW - 3); }
static long max_top(void) { return sc_total > sc_rows ? sc_total - sc_rows : 0; }

static int elev_y(void)
{
	int room = cable_len() - elev_h();

	if (room <= 0 || max_top() == 0)
		return cable_top();
	return cable_top() + (int)((long)room * (sc_top < max_top() ? sc_top
		: max_top()) / max_top());
}

static void bevel(int x, int y, int w, int h, int down)
{
	XSetForeground(dpy, gc, down ? px_bg2 : px_bg1);
	XFillRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
	XSetForeground(dpy, gc, down ? px_bg3 : px_hi);
	XDrawLine(dpy, win, gc, x, y, x + w - 1, y);
	XDrawLine(dpy, win, gc, x, y, x, y + h - 1);
	XSetForeground(dpy, gc, down ? px_hi : px_bg3);
	XDrawLine(dpy, win, gc, x, y + h - 1, x + w - 1, y + h - 1);
	XDrawLine(dpy, win, gc, x + w - 1, y, x + w - 1, y + h - 1);
	if (px_bg1 == px_bg) {
		XSetForeground(dpy, gc, px_fg);
		XDrawRectangle(dpy, win, gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
	}
}

static void triangle(int cx, int cy, int up)
{
	XPoint p[3];
	int s = (SBW - 3) / 3;

	p[0].x = (short)(cx - s);
	p[0].y = (short)(up ? cy + s / 2 : cy - s / 2);
	p[1].x = (short)(cx + s);
	p[1].y = p[0].y;
	p[2].x = (short)cx;
	p[2].y = (short)(up ? cy - s / 2 - 1 : cy + s / 2 + 1);
	XSetForeground(dpy, gc, px_fg);
	XFillPolygon(dpy, win, gc, p, 3, Convex, CoordModeOrigin);
}

static void draw_scrollbar(void)
{
	int x = sb_x, w = SBW, ct = cable_top(), cl = cable_len(), ey = elev_y();
	int eh = elev_h(), part = eh / 3, cx = x + w / 2;

	XSetForeground(dpy, gc, px_bg1);
	XFillRectangle(dpy, win, gc, x, gy - PAD, (unsigned)w, (unsigned)(win_h - gy + PAD));
	XSetForeground(dpy, gc, px_bg3);
	XDrawLine(dpy, win, gc, x, gy - PAD, x, win_h);
	/* the anchors */
	bevel(x + 3, gy, w - 5, ANCHOR, 0);
	bevel(x + 3, win_h - PAD - ANCHOR, w - 5, ANCHOR, 0);
	/* the cable, darker where the page in view is */
	if (cl > 0) {
		XSetForeground(dpy, gc, px_bg3);
		XFillRectangle(dpy, win, gc, cx - 1, ct, 3, (unsigned)cl);
		if (sc_total > sc_rows && sc_total > 0) {
			int py = ct + (int)((long)cl * sc_top / sc_total);
			int ph = (int)((long)cl * sc_rows / sc_total);

			if (ph < 2)
				ph = 2;
			XSetForeground(dpy, gc, px_fg);
			XFillRectangle(dpy, win, gc, cx - 1, py, 3, (unsigned)ph);
		}
	}
	/* the elevator */
	bevel(x + 2, ey, w - 3, part, 0);
	bevel(x + 2, ey + part, w - 3, part, dragging);
	bevel(x + 2, ey + 2 * part, w - 3, eh - 2 * part, 0);
	triangle(cx, ey + part / 2, 1);
	triangle(cx, ey + 2 * part + (eh - 2 * part) / 2, 0);
	XSetForeground(dpy, gc, px_bg3);
	XDrawLine(dpy, win, gc, cx - 3, ey + part + part / 2, cx + 3, ey + part + part / 2);
	scroll_dirty = 0;
}

/* --- cells ------------------------------------------------------------- */

void scr_erase(void)
{
	size_t i, n = (size_t)scr_rows * (size_t)scr_cols;

	for (i = 0; i < n; i++) {
		nxt[i].ch = ' ';
		nxt[i].a = 0;
	}
}

int scr_put(int row, int col, const char *s, int n, int attr)
{
	struct cell *line;
	int c0 = col;

	if (row < 0 || row >= scr_rows || col < 0)
		return 0;
	line = nxt + (size_t)row * (size_t)scr_cols;
	for (; n > 0 && col < scr_cols; n--, col++) {
		line[col].ch = (unsigned char)*s++;
		line[col].a = (unsigned char)attr;
	}
	return col - c0;
}

void scr_fill(int row, int col, int ncols, int c, int attr)
{
	struct cell *line;

	if (row < 0 || row >= scr_rows)
		return;
	line = nxt + (size_t)row * (size_t)scr_cols;
	for (; ncols > 0 && col < scr_cols; ncols--, col++) {
		line[col].ch = (unsigned char)c;
		line[col].a = (unsigned char)attr;
	}
}

void scr_cursor(int row, int col)
{
	cur_row = row;
	cur_col = col;
}

/* n cells of one attribute at row/col (cursor: draw them inverted) */
static void draw_run(int row, int col, const struct cell *c, int n, int cursor)
{
	char buf[512];
	int a = c->a, x = gx + col * cw, y = gy + row * ch + ascent, i;
	unsigned long fg = px_fg, bg = px_bg;

	if (n > (int)sizeof buf)
		n = sizeof buf;
	for (i = 0; i < n; i++)
		buf[i] = (char)(c[i].ch ? c[i].ch : ' ');
	if ((a & CA_LINK) && link_px_ok)
		fg = px_link[scr_link_color & 7];
	if (((a & CA_REV) != 0) != (cursor != 0)) {
		unsigned long t = fg;

		fg = bg;
		bg = t;
	}
	/* a find match: black on yellow, reversed or not */
	if ((a & CA_MARK) && !cursor) {
		fg = px_fg;
		bg = px_mark;
	}
	XSetForeground(dpy, gc, fg);
	XSetBackground(dpy, gc, bg);
	XSetFont(dpy, gc, (a & CA_BOLD) && bold ? bold->fid : font->fid);
	XDrawImageString(dpy, win, gc, x, y, buf, n);
	if ((a & CA_BOLD) && !bold)
		XDrawString(dpy, win, gc, x + 1, y, buf, n);
	/* links are underlined too: colour alone may not be there */
	if ((a & CA_UNDER) || ((a & CA_LINK) && !(a & CA_MARK)))
		XDrawLine(dpy, win, gc, x, y + 1, x + n * cw - 1, y + 1);
}

/* draw the changed cells of row r (all of them when full) */
static void flush_row(int r, int full)
{
	struct cell *nl = nxt + (size_t)r * (size_t)scr_cols;
	struct cell *cl = cur + (size_t)r * (size_t)scr_cols;
	int c = 0;

	while (c < scr_cols) {
		int c1;

		if (!full && nl[c].ch == cl[c].ch && nl[c].a == cl[c].a) {
			c++;
			continue;
		}
		/* a run of changed cells with the same attribute */
		for (c1 = c + 1; c1 < scr_cols && nl[c1].a == nl[c].a
			&& (full || nl[c1].ch != cl[c1].ch || nl[c1].a != cl[c1].a);
			c1++)
			;
		draw_run(r, c, nl + c, c1 - c, 0);
		c = c1;
	}
	memcpy(cl, nl, (size_t)scr_cols * sizeof *cl);
}

void scr_flush(int full)
{
	int r;

	if (dpy == NULL)
		return;
	if (full) {
		XClearWindow(dpy, win);
		drawn_row = drawn_col = -1;
		controls_dirty = scroll_dirty = 1;
	}
	if (controls_dirty)
		draw_controls();
	if (scroll_dirty)
		draw_scrollbar();
	/* the old cursor's cell comes back as it is */
	if (drawn_row >= 0 && drawn_row < scr_rows && drawn_col < scr_cols)
		cur[(size_t)drawn_row * (size_t)scr_cols + (size_t)drawn_col].a = 0xFF;
	for (r = 0; r < scr_rows; r++)
		flush_row(r, full);
	drawn_row = drawn_col = -1;
	if (cur_row >= 0 && cur_row < scr_rows && cur_col >= 0 && cur_col < scr_cols) {
		draw_run(cur_row, cur_col,
			cur + (size_t)cur_row * (size_t)scr_cols + (size_t)cur_col, 1, 1);
		drawn_row = cur_row;
		drawn_col = cur_col;
	}
	XFlush(dpy);
}

/* the window was uncovered: draw again what is in it */
static void redraw(void)
{
	int r, c;

	XClearWindow(dpy, win);
	draw_controls();
	draw_scrollbar();
	for (r = 0; r < scr_rows; r++) {
		const struct cell *cl = cur + (size_t)r * (size_t)scr_cols;

		for (c = 0; c < scr_cols; ) {
			int c1;

			for (c1 = c + 1; c1 < scr_cols && cl[c1].a == cl[c].a; c1++)
				;
			if (cl[c].a != 0xFF)
				draw_run(r, c, cl + c, c1 - c, 0);
			c = c1;
		}
	}
	if (drawn_row >= 0)
		draw_run(drawn_row, drawn_col,
			cur + (size_t)drawn_row * (size_t)scr_cols + (size_t)drawn_col, 1, 1);
	XFlush(dpy);
}

void scr_bell(void)
{
	if (dpy) {
		XBell(dpy, 0);
		XFlush(dpy);
	}
}

/* --- input --------------------------------------------------------------- */

static int key_of(XKeyEvent *e)
{
	char b[8];
	KeySym ks = NoSymbol;
	int n = XLookupString(e, b, sizeof b, &ks, NULL);

	switch (ks) {
#ifdef XK_KP_Up			/* X11R6; R5 has no keypad cursor keys */
	case XK_KP_Up: return K_UP;
	case XK_KP_Down: return K_DOWN;
	case XK_KP_Left: return K_LEFT;
	case XK_KP_Right: return K_RIGHT;
	case XK_KP_Prior: return K_PGUP;
	case XK_KP_Next: return K_PGDN;
	case XK_KP_Home: return K_HOME;
	case XK_KP_End: return K_END;
	case XK_KP_Delete: return K_DEL;
	case XK_KP_Insert: return K_INS;
#endif
	case XK_Up: return K_UP;
	case XK_Down: return K_DOWN;
	case XK_Left: return K_LEFT;
	case XK_Right: return K_RIGHT;
	case XK_Prior: return K_PGUP;
	case XK_Next: return K_PGDN;
	case XK_Home: return K_HOME;
	case XK_End: return K_END;
	case XK_Delete: return K_DEL;
	case XK_Insert: return K_INS;
	case XK_F1: return K_F1;
#ifdef XK_ISO_Left_Tab
	case XK_ISO_Left_Tab: return K_BTAB;
#endif
	case XK_Tab:
		return (e->state & ShiftMask) ? K_BTAB : '\t';
	case XK_Return: case XK_KP_Enter: return '\r';
	}
	if (n == 1)
		return (unsigned char)b[0];
	return -1;			/* a modifier, or nothing we use */
}

static int button_at(int x, int y)
{
	int i;

	if (y < CPAD || y >= CPAD + bh)
		return -1;
	for (i = 0; i < NBUTTONS; i++)
		if (x >= buttons[i].x && x < buttons[i].x + buttons[i].w)
			return i;
	return -1;
}

/* a line on the elevator's drag area: where the page would start */
static long drag_target(int y)
{
	int room = cable_len() - elev_h();
	long t;

	if (room <= 0 || max_top() == 0)
		return 0;
	t = (long)(y - drag_dy - cable_top()) * max_top() / room;
	return t < 0 ? 0 : t > max_top() ? max_top() : t;
}

/* button 1 down on the scrollbar: K_SCROLL, or -1 (a drag begins) */
static int scrollbar_press(int y)
{
	int ey = elev_y(), part = elev_h() / 3;
	long page = sc_rows > 1 ? sc_rows - 1 : 1;

	if (y < cable_top())
		scr_scroll_target = 0;
	else if (y >= win_h - PAD - ANCHOR - 2)
		scr_scroll_target = max_top();
	else if (y < ey)
		scr_scroll_target = sc_top - page;
	else if (y < ey + part)
		scr_scroll_target = sc_top - 1;
	else if (y < ey + 2 * part) {
		dragging = 1;
		drag_dy = y - ey;
		scroll_dirty = 1;
		draw_scrollbar();
		return -1;
	} else if (y < ey + elev_h())
		scr_scroll_target = sc_top + 1;
	else
		scr_scroll_target = sc_top + page;
	return K_SCROLL;
}

/* one event: a key, or -1 */
static int event(void)
{
	XEvent ev;
	int b, x, y;

	XNextEvent(dpy, &ev);
	switch (ev.type) {
	case KeyPress:
		entropy_event();	/* key timing feeds the TLS entropy pool */
		return key_of(&ev.xkey);
	case ButtonPress:
		entropy_event();
		if (ev.xbutton.button == Button4)
			return K_WHEELUP;
		if (ev.xbutton.button == Button5)
			return K_WHEELDN;
		if (ev.xbutton.button != Button1)
			return -1;
		x = ev.xbutton.x;
		y = ev.xbutton.y;
		if (y < ctrl_h) {
			/* a button acts when it is let go over it */
			if ((b = button_at(x, y)) >= 0 && buttons[b].on) {
				pressed = b;
				draw_button(&buttons[b], 1);
				return -1;
			}
			return x >= url_x ? 'G' : -1;
		}
		if (x >= sb_x)
			return scrollbar_press(y);
		scr_mouse_row = (y - gy) / ch;
		scr_mouse_col = (x - gx) / cw;
		if (y < gy || x < gx || scr_mouse_row >= scr_rows
			|| scr_mouse_col >= scr_cols)
			return -1;
		return K_MOUSE;
	case ButtonRelease:
		if (ev.xbutton.button != Button1)
			return -1;
		if (dragging) {
			dragging = 0;
			draw_scrollbar();
			return -1;
		}
		if (pressed >= 0) {
			b = pressed;
			pressed = -1;
			draw_button(&buttons[b], 0);
			if (button_at(ev.xbutton.x, ev.xbutton.y) == b && buttons[b].on)
				return buttons[b].key;
		}
		return -1;
	case MotionNotify:
		if (!dragging)
			return -1;
		/* only where the pointer is now */
		while (XCheckTypedWindowEvent(dpy, win, MotionNotify, &ev))
			;
		scr_scroll_target = drag_target(ev.xmotion.y);
		return scr_scroll_target != sc_top ? K_SCROLL : -1;
	case Expose:
		if (ev.xexpose.count == 0)
			redraw();
		return -1;
	case ConfigureNotify:
		win_w = ev.xconfigure.width;
		win_h = ev.xconfigure.height;
		sb_x = win_w - SBW;
		return -1;
	case MappingNotify:
		XRefreshKeyboardMapping(&ev.xmapping);
		return -1;
	case ClientMessage:
		if ((Atom)ev.xclient.data.l[0] == wm_delete)
			closing = 1;
		return closing ? K_CLOSE : -1;
	}
	return -1;
}

int scr_getkey(int timeout_ms)
{
	unsigned long t0 = os_msec();
	int fd = ConnectionNumber(dpy);

	for (;;) {
		struct pollfd p;
		long left = -1;

		if (closing)
			return K_CLOSE;
		while (XPending(dpy)) {
			int k = event();

			if (k >= 0)
				return k;
			/* a size change: let the caller look */
			if (cols_fit() != scr_cols || rows_fit() != scr_rows)
				return -1;
		}
		if (timeout_ms >= 0) {
			left = (long)timeout_ms - (long)(os_msec() - t0);
			if (left <= 0)
				return -1;
		}
		p.fd = fd;
		p.events = POLLIN;
		p.revents = 0;
		if (poll(&p, 1, (int)left) < 0 && errno != EINTR)
			return -1;
	}
}
