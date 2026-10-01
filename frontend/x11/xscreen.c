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
#include "style.h"
#include "screen.h"
#include "pixels.h"

int scr_rows = 25, scr_cols = 80;
int scr_color = 1;
int scr_link_color = 4;		/* blue: links on a white page */
enum term_cs scr_cs = TCS_LATIN1;
int scr_mouse_row, scr_mouse_col;
int scr_mouse_x, scr_mouse_y;
int scr_proportional = 1;
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
static int px_mode;			/* the page in fonts, in a pane */
static struct lmetrics metrics;		/* ... measured with these */
static XFontStruct *find_font(const char *family, int bold, int slant, int px);
static XFontStruct *face_font(int attr, int face);
static int m_width(void *ctx, int attr, int face, const char *s, int n);
static int m_height(void *ctx, int attr, int face, int *ascent);
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
static char edit_text[1024];		/* a URL being typed into the field */
static int editing, edit_pos;
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

	/* the page in fonts, unless it's off or none is to be had */
	px_mode = scr_proportional && find_font("helvetica", 0, 0, 12) != NULL;
	if (scr_proportional && !px_mode)
		px_mode = find_font("lucida", 0, 0, 12) != NULL;
	metrics.width = m_width;
	metrics.height = m_height;
	metrics.ctx = NULL;
	metrics.em = px_mode ? XTextWidth(face_font(0, 0), "0", 1) : 1;

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

int scr_url_edit(const char *text, int pos)
{
	if (text == NULL) {
		editing = 0;
		controls_dirty = 1;
		return 1;
	}
	strncpy(edit_text, text, sizeof edit_text - 1);
	edit_text[sizeof edit_text - 1] = '\0';
	edit_pos = pos;
	editing = 1;
	controls_dirty = 1;
	return 1;
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
	/* no grey on a mono screen: a grey stipple for a button that can't
	 * act */
	if (!b->on && px_bg3 == px_fg) {
		static Pixmap grey;
		static char bits[] = { 0x01, 0x02 };

		if (grey == 0)
			grey = XCreateBitmapFromData(dpy, win, bits, 2, 2);
		XSetStipple(dpy, gc, grey);
		XSetFillStyle(dpy, gc, FillStippled);
	}
	XDrawString(dpy, win, gc, x + (w - tw) / 2, y + 3 + ascent,
		(char *)b->label, (int)strlen(b->label));
	XSetFillStyle(dpy, gc, FillSolid);
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
		const char *t = editing ? edit_text : url_text;
		int fit = fw / cw - 1, start = 0;

		/* being typed: the part around the cursor, and an OPEN LOOK
		 * caret (a small triangle) under the insertion point */
		if (editing && edit_pos > fit)
			start = edit_pos - fit;
		n = (int)strlen(t + start);
		if (n > fit + 1)
			n = fit + 1;
		XDrawString(dpy, win, gc, fx, y, (char *)t + start, n);
		XSetForeground(dpy, gc, editing ? px_fg : px_bg3);
		XDrawLine(dpy, win, gc, fx, y + 3, fx + fw, y + 3);
		if (editing) {
			int cx = fx + (edit_pos - start) * cw;
			XPoint p[3];

			p[0].x = (short)(cx - 4);
			p[0].y = (short)(y + 6);
			p[1].x = (short)(cx + 4);
			p[1].y = (short)(y + 6);
			p[2].x = (short)cx;
			p[2].y = (short)(y - 3);
			XSetForeground(dpy, gc, px_fg);
			XFillPolygon(dpy, win, gc, p, 3, Convex, CoordModeOrigin);
		}
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

/* --- the page in fonts ---------------------------------------------------- */

/*
 * The pane is the cells' rows 1 to rows-2, in pixels. Each frame the page
 * comes as runs of text (scr_text); flushing paints them, and any cell
 * written over the pane (a menu), into a pixmap, and copies that to the
 * window when it changed.
 */
#define PANE_IN	3			/* the page's left inset */

struct run {
	short x, y, a;			/* y: the line's top; a: its ascent */
	unsigned char attr, face;
	unsigned off, n;		/* in rtext */
};

static struct run *runs;
static int nruns, runs_cap;

/* an image drawn this frame (im NULL: a frame where one will be) */
struct ximg;
struct idraw {
	struct ximg *im;
	unsigned long version;		/* its rows so far (repaint on change) */
	short x, y, w, h;
	int attr;
};

static struct idraw *idraws;
static int nidraws, idraws_cap;
static char *rtext;
static unsigned rtext_len, rtext_cap;
static unsigned long pane_sum = 1, drawn_sum;	/* what's in the pixmap */
static Pixmap pane_pm;
static int pm_w, pm_h;

/* fonts by look: [bold][face] */
static XFontStruct *faces[2][16];
static unsigned char face_tried[2][16];

static int pane_x(void) { return gx; }
static int pane_y(void) { return gy + ch; }
static int pane_wpx(void) { return scr_cols * cw; }
static int pane_hpx(void) { return (scr_rows - 2) * ch; }

/* a font for the look, by XLFD: family, weight, slant and pixel size,
 * trying the sizes around it */
static XFontStruct *find_font(const char *family, int bold, int slant, int px)
{
	static const int near[] = { 0, -1, 1, -2, 2, 3, -3 };
	static const char slants[2][3] = { "r", "oi" };
	char name[200];
	size_t i, j;
	XFontStruct *f;

	for (j = 0; slants[slant ? 1 : 0][j]; j++)
		for (i = 0; i < sizeof near / sizeof near[0]; i++) {
			sprintf(name, "-*-%s-%s-%c-normal--%d-*-*-*-*-*-iso8859-1",
				family, bold ? "bold" : "medium",
				slants[slant ? 1 : 0][j], px + near[i]);
			if ((f = XLoadQueryFont(dpy, name)) != NULL)
				return f;
		}
	return NULL;
}

static XFontStruct *face_font(int attr, int face)
{
	int b = (attr & CA_BOLD) != 0, k = face & 15, h = face & LF_HMASK;
	XFontStruct *f;

	if (faces[b][k])
		return faces[b][k];
	if (!face_tried[b][k]) {
		/* headings: bigger and bold; 12 pixels otherwise */
		static const int sizes[4] = { 12, 18, 14, 12 };
		int bold = b || h, slant = (face & LF_ITALIC) != 0;
		const char *fam = (face & LF_MONO) ? "courier" : "helvetica";

		face_tried[b][k] = 1;
		f = find_font(fam, bold, slant, sizes[h]);
		if (f == NULL && slant)
			f = find_font(fam, bold, 0, sizes[h]);
		if (f == NULL && !(face & LF_MONO))
			f = find_font("lucida", bold, slant, sizes[h]);
		faces[b][k] = f;
	}
	if (faces[b][k] == NULL)
		/* no such font here: the cells' own */
		faces[b][k] = b && bold ? bold : font;
	return faces[b][k];
}

static int m_width(void *ctx, int attr, int face, const char *s, int n)
{
	(void)ctx;
	return XTextWidth(face_font(attr & SA_BOLD ? CA_BOLD : 0, face),
		(char *)s, n);
}

static int m_height(void *ctx, int attr, int face, int *ascent)
{
	XFontStruct *f = face_font(attr & SA_BOLD ? CA_BOLD : 0, face);

	(void)ctx;
	*ascent = f->ascent + 1;
	return f->ascent + f->descent + 2;
}

const struct lmetrics *scr_metrics(void)
{
	return px_mode ? &metrics : NULL;
}

int scr_pane_w(void)
{
	return px_mode ? pane_wpx() - 2 * PANE_IN : scr_cols;
}

int scr_pane_h(void)
{
	return px_mode ? pane_hpx() : scr_rows - 2;
}

void scr_text(int x, int y, int ascent, const char *s, int n, int attr,
	int face)
{
	struct run *r;

	if (!px_mode) {
		scr_put(1 + y, x, s, n, attr);
		return;
	}
	if (n <= 0)
		return;
	if (nruns == runs_cap) {
		int c = runs_cap ? runs_cap * 2 : 256;
		struct run *q = xrealloc(runs, (size_t)c * sizeof *q);

		if (q == NULL)
			return;
		runs = q;
		runs_cap = c;
	}
	if (rtext_len + (unsigned)n > rtext_cap) {
		unsigned c = rtext_cap ? rtext_cap * 2 : 8192;
		char *q;

		while (c < rtext_len + (unsigned)n)
			c *= 2;
		if ((q = xrealloc(rtext, c)) == NULL)
			return;
		rtext = q;
		rtext_cap = c;
	}
	memcpy(rtext + rtext_len, s, (size_t)n);
	r = &runs[nruns++];
	r->x = (short)x;
	r->y = (short)y;
	r->a = (short)ascent;
	r->attr = (unsigned char)attr;
	r->face = (unsigned char)face;
	r->off = rtext_len;
	r->n = (unsigned)n;
	rtext_len += (unsigned)n;
}

/* a checksum of the frame's pane: its runs, and the cells over it */
static unsigned long pane_checksum(void)
{
	unsigned long h = 5381;
	const unsigned char *p;
	size_t i, n;

	p = (const unsigned char *)runs;
	n = (size_t)nruns * sizeof *runs;
	for (i = 0; i < n; i++)
		h = h * 33 + p[i];
	for (i = 0; i < rtext_len; i++)
		h = h * 33 + (unsigned char)rtext[i];
	p = (const unsigned char *)idraws;
	n = (size_t)nidraws * sizeof *idraws;
	for (i = 0; i < n; i++)
		h = h * 33 + p[i];
	p = (const unsigned char *)(nxt + scr_cols);
	n = (size_t)(scr_rows - 2) * (size_t)scr_cols * sizeof *nxt;
	for (i = 0; i < n; i++)
		h = h * 33 + p[i];
	return h | 1;
}

/* one run, into the pixmap */
static void paint_run(const struct run *r)
{
	XFontStruct *f = face_font(r->attr, r->face);
	int x = PANE_IN + r->x, base = r->y + r->a;
	int w = XTextWidth(f, rtext + r->off, (int)r->n);
	unsigned long fg = px_fg, bg = px_bg;
	int fill = 0;

	if ((r->attr & CA_LINK) && link_px_ok)
		fg = px_link[scr_link_color & 7];
	if (r->attr & CA_REV) {
		bg = fg;
		fg = px_bg;
		fill = 1;
	}
	if (r->attr & CA_MARK) {
		fg = px_fg;
		bg = px_mark;
		fill = 1;
	}
	if (fill) {
		XSetForeground(dpy, gc, bg);
		XFillRectangle(dpy, pane_pm, gc, x - 1, base - f->ascent, (unsigned)(w + 2),
			(unsigned)(f->ascent + f->descent));
	}
	XSetForeground(dpy, gc, fg);
	XSetFont(dpy, gc, f->fid);
	XDrawString(dpy, pane_pm, gc, x, base, rtext + r->off, (int)r->n);
	if ((r->attr & CA_UNDER) || ((r->attr & CA_LINK) && !(r->attr & CA_MARK)))
		XDrawLine(dpy, pane_pm, gc, x, base + 1, x + w - 1, base + 1);
}

/* --- images ----------------------------------------------------------------- */

struct ximg {
	Pixmap pm, mask;		/* mask: None without one */
	int w, h;
	unsigned long version;
	XImage *row, *mrow;		/* a row of each: data pointed in */
};

static struct px_format pxf;
static int pxf_state;			/* 0 not yet, 1 ready, -1 no images */
static GC mask_gc;
static unsigned long img_serial;
static int x_error;			/* the last X error's code, 0: none */

/* X errors aren't fatal (Xlib's own handler exits): the image that
 * didn't fit in the server's memory simply isn't shown */
static int on_x_error(Display *d, XErrorEvent *e)
{
	(void)d;
	x_error = e->error_code;
	return 0;
}

/* a PseudoColor screen: a colour cube in the shared colormap, as big as
 * the other clients leave room for */
static int make_cube(void)
{
	static const int tries[5] = { 6, 5, 4, 3, 2 };
	Colormap cm = DefaultColormap(dpy, DefaultScreen(dpy));
	int t;

	for (t = 0; t < 5; t++) {
		int l = tries[t], n = l * l * l, i;

		for (i = 0; i < n; i++) {
			XColor c;

			c.red = (unsigned short)(i / (l * l) * 65535 / (l - 1));
			c.green = (unsigned short)(i / l % l * 65535 / (l - 1));
			c.blue = (unsigned short)(i % l * 65535 / (l - 1));
			c.flags = DoRed | DoGreen | DoBlue;
			if (!XAllocColor(dpy, cm, &c))
				break;
			pxf.pixel[i] = c.pixel;
		}
		if (i == n) {
			pxf.kind = PX_CUBE;
			pxf.levels[0] = pxf.levels[1] = pxf.levels[2] = l;
			return 1;
		}
		if (i > 0)
			XFreeColors(dpy, cm, pxf.pixel, i, 0);
	}
	return 0;
}

/* a GrayScale screen: a ramp of greys allocated, as many as fit */
static int make_ramp(void)
{
	static const int tries[5] = { 64, 32, 16, 8, 4 };
	Colormap cm = DefaultColormap(dpy, DefaultScreen(dpy));
	int t;

	for (t = 0; t < 5; t++) {
		int n = tries[t], i;

		for (i = 0; i < n; i++) {
			XColor c;

			c.red = c.green = c.blue = (unsigned short)(i * 65535 / (n - 1));
			c.flags = DoRed | DoGreen | DoBlue;
			if (!XAllocColor(dpy, cm, &c))
				break;
			pxf.pixel[i] = c.pixel;
		}
		if (i == n) {
			pxf.kind = PX_GRAY;
			pxf.levels[0] = n;
			return 1;
		}
		if (i > 0)
			XFreeColors(dpy, cm, pxf.pixel, i, 0);
	}
	return 0;
}

static int make_format(void)
{
	int s = DefaultScreen(dpy), depth = DefaultDepth(dpy, s);
	Visual *v = DefaultVisual(dpy, s);
	XImage *t;

	memset(&pxf, 0, sizeof pxf);
	if (!px_mode)
		return 0;
	/* how the server packs pixels of this depth: a test image says */
	t = XCreateImage(dpy, v, (unsigned)depth, ZPixmap, 0, NULL, 1, 1, 8, 0);
	if (t == NULL)
		return 0;
	pxf.bpp = t->bits_per_pixel;
	pxf.byte_msb = t->byte_order == MSBFirst;
	pxf.bit_msb = t->bitmap_bit_order == MSBFirst;
	XDestroyImage(t);
	if (pxf.bpp != 1 && pxf.bpp != 8 && pxf.bpp != 16 && pxf.bpp != 24
		&& pxf.bpp != 32)
		return 0;
	XSetErrorHandler(on_x_error);
	if (depth == 1) {
		pxf.kind = PX_GRAY;
		pxf.levels[0] = 2;
		pxf.pixel[0] = BlackPixel(dpy, s);
		pxf.pixel[1] = WhitePixel(dpy, s);
		return 1;
	}
	switch (v->class) {
	case TrueColor:
		pxf.kind = PX_TRUE;
		pxf.mask[0] = v->red_mask;
		pxf.mask[1] = v->green_mask;
		pxf.mask[2] = v->blue_mask;
		return 1;
	case PseudoColor:
		return make_cube();
	case GrayScale:
		return make_ramp();
	case StaticGray:
		/* a fixed ramp, black at 0: pixel i is grey i */
		pxf.kind = PX_GRAY;
		pxf.levels[0] = depth >= 8 ? 256 : 1 << depth;
		{
			int i;

			for (i = 0; i < pxf.levels[0]; i++)
				pxf.pixel[i] = (unsigned long)i;
		}
		return 1;
	}
	return 0;	/* (StaticColor, DirectColor: rare, not done) */
}

const struct px_format *scr_pixels(void)
{
	if (pxf_state == 0)
		pxf_state = dpy && make_format() ? 1 : -1;
	return pxf_state > 0 ? &pxf : NULL;
}

void scr_image_free(void *img)
{
	struct ximg *im = img;

	if (im == NULL)
		return;
	if (im->pm)
		XFreePixmap(dpy, im->pm);
	if (im->mask)
		XFreePixmap(dpy, im->mask);
	if (im->row)
		XDestroyImage(im->row);		/* (data: none of its own) */
	if (im->mrow)
		XDestroyImage(im->mrow);
	xfree(im);
}

void *scr_image_new(int w, int h, int masked)
{
	int s = DefaultScreen(dpy), depth = DefaultDepth(dpy, s);
	Visual *v = DefaultVisual(dpy, s);
	struct ximg *im;

	if (scr_pixels() == NULL || w <= 0 || h <= 0 || w > 8192 || h > 8192
		|| (im = xmalloc(sizeof *im)) == NULL)
		return NULL;
	memset(im, 0, sizeof *im);
	im->w = w;
	im->h = h;
	x_error = 0;
	im->pm = XCreatePixmap(dpy, win, (unsigned)w, (unsigned)h, (unsigned)depth);
	if (masked)
		im->mask = XCreatePixmap(dpy, win, (unsigned)w, (unsigned)h, 1);
	XSync(dpy, False);
	if (x_error) {
		/* (the server's out of memory: no picture) */
		scr_image_free(im);
		return NULL;
	}
	/* rows not yet come: the background (and, masked, not shown) */
	XSetForeground(dpy, gc, px_bg);
	XFillRectangle(dpy, im->pm, gc, 0, 0, (unsigned)w, (unsigned)h);
	if (masked) {
		if (mask_gc == 0)
			mask_gc = XCreateGC(dpy, im->mask, 0, NULL);
		XSetForeground(dpy, mask_gc, 0);
		XFillRectangle(dpy, im->mask, mask_gc, 0, 0, (unsigned)w, (unsigned)h);
		XSetForeground(dpy, mask_gc, 1);
		XSetBackground(dpy, mask_gc, 0);
		im->mrow = XCreateImage(dpy, v, 1, XYBitmap, 0, NULL, (unsigned)w, 1, 8, 0);
	}
	im->row = XCreateImage(dpy, v, (unsigned)depth, ZPixmap, 0, NULL, (unsigned)w, 1, 8, 0);
	if (im->row == NULL || (masked && im->mrow == NULL)
		|| im->row->bytes_per_line != (int)px_row_bytes(&pxf, w)) {
		scr_image_free(im);
		return NULL;
	}
	return im;
}

void scr_image_row(void *img, int y, const unsigned char *px,
	const unsigned char *mask)
{
	struct ximg *im = img;

	if (im == NULL || y < 0 || y >= im->h)
		return;
	im->row->data = (char *)px;
	XPutImage(dpy, im->pm, gc, im->row, 0, 0, 0, y, (unsigned)im->w, 1);
	im->row->data = NULL;
	if (im->mask) {
		if (mask) {
			im->mrow->data = (char *)mask;
			XPutImage(dpy, im->mask, mask_gc, im->mrow, 0, 0, 0, y, (unsigned)im->w, 1);
			im->mrow->data = NULL;
		} else
			XFillRectangle(dpy, im->mask, mask_gc, 0, y, (unsigned)im->w, 1);
	}
	im->version = ++img_serial;
}

void scr_image_draw(void *img, int x, int y, int w, int h, int attr)
{
	struct idraw *d;

	if (!px_mode || w <= 0 || h <= 0)
		return;
	if (nidraws == idraws_cap) {
		int c = idraws_cap ? idraws_cap * 2 : 32;
		struct idraw *q = xrealloc(idraws, (size_t)c * sizeof *q);

		if (q == NULL)
			return;
		idraws = q;
		idraws_cap = c;
	}
	d = &idraws[nidraws++];
	memset(d, 0, sizeof *d);	/* (padding too: it is checksummed) */
	d->im = img;
	d->version = img ? ((struct ximg *)img)->version : 0;
	d->x = (short)x;
	d->y = (short)y;
	d->w = (short)w;
	d->h = (short)h;
	d->attr = attr;
}

/* an image, into the pixmap */
static void paint_image(const struct idraw *d)
{
	int x = PANE_IN + d->x, y = d->y;
	unsigned long fg = (d->attr & CA_LINK) && link_px_ok ?
		px_link[scr_link_color & 7] : px_fg;

	if (d->im) {
		if (d->im->mask) {
			XSetClipMask(dpy, gc, d->im->mask);
			XSetClipOrigin(dpy, gc, x, y);
		}
		XCopyArea(dpy, d->im->pm, pane_pm, gc, 0, 0, (unsigned)d->w,
			(unsigned)d->h, x, y);
		if (d->im->mask) {
			XSetClipMask(dpy, gc, None);
			XSetClipOrigin(dpy, gc, 0, 0);
		}
	} else if (d->w > 2 && d->h > 2) {
		/* not here yet: a frame */
		XSetForeground(dpy, gc, DefaultDepth(dpy, DefaultScreen(dpy)) > 1 ?
			px_bg3 : px_fg);
		XDrawRectangle(dpy, pane_pm, gc, x, y, (unsigned)(d->w - 1),
			(unsigned)(d->h - 1));
	}
	if (d->attr & CA_REV) {
		/* the selected link: a frame round it */
		XSetForeground(dpy, gc, fg);
		XDrawRectangle(dpy, pane_pm, gc, x - 2, y - 2, (unsigned)(d->w + 3),
			(unsigned)(d->h + 3));
		XDrawRectangle(dpy, pane_pm, gc, x - 1, y - 1, (unsigned)(d->w + 1),
			(unsigned)(d->h + 1));
	}
}

/* the pane, painted afresh: the page's runs, then the cells over them */
static void paint_pane(void)
{
	int w = pane_wpx(), h = pane_hpx(), i, r, c;
	Window root;
	int dx, dy;
	unsigned int bw, depth, pw, ph;

	if (w <= 0 || h <= 0)
		return;
	if (pane_pm == 0 || pm_w != w || pm_h != h) {
		if (pane_pm)
			XFreePixmap(dpy, pane_pm);
		XGetGeometry(dpy, win, &root, &dx, &dy, &pw, &ph, &bw, &depth);
		pane_pm = XCreatePixmap(dpy, win, (unsigned)w, (unsigned)h, depth);
		pm_w = w;
		pm_h = h;
	}
	XSetForeground(dpy, gc, px_bg);
	XFillRectangle(dpy, pane_pm, gc, 0, 0, (unsigned)w, (unsigned)h);
	for (i = 0; i < nruns; i++)
		paint_run(&runs[i]);
	for (i = 0; i < nidraws; i++)
		paint_image(&idraws[i]);
	/* cells written over the pane this frame (ch 0: none) */
	for (r = 1; r < scr_rows - 1; r++) {
		const struct cell *cl = nxt + (size_t)r * (size_t)scr_cols;

		for (c = 0; c < scr_cols; c++) {
			char b = (char)(cl[c].ch ? cl[c].ch : ' ');
			unsigned long fg = px_fg, bg = px_bg;
			int x = c * cw, y = (r - 1) * ch;

			if (cl[c].ch == 0)
				continue;
			if ((cl[c].a & CA_LINK) && link_px_ok)
				fg = px_link[scr_link_color & 7];
			if (cl[c].a & CA_REV) {
				unsigned long t = fg;

				fg = bg;
				bg = t;
			}
			XSetForeground(dpy, gc, fg);
			XSetBackground(dpy, gc, bg);
			XSetFont(dpy, gc, (cl[c].a & CA_BOLD) && bold ? bold->fid : font->fid);
			XDrawImageString(dpy, pane_pm, gc, x, y + ascent, &b, 1);
		}
	}
	drawn_sum = pane_sum;
}

/* --- cells ------------------------------------------------------------- */

void scr_erase(void)
{
	size_t i, n = (size_t)scr_rows * (size_t)scr_cols;

	/* over the pane, a cell nothing writes is none at all: the page
	 * shows through */
	for (i = 0; i < n; i++) {
		nxt[i].ch = px_mode ? 0 : ' ';
		nxt[i].a = 0;
	}
	nruns = 0;
	nidraws = 0;
	rtext_len = 0;
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
	if (px_mode) {
		/* the title and status rows as cells; the pane as a picture */
		pane_sum = pane_checksum();
		if (full || pane_sum != drawn_sum || pane_pm == 0) {
			paint_pane();
			XCopyArea(dpy, pane_pm, win, gc, 0, 0, (unsigned)pm_w,
				(unsigned)pm_h, pane_x(), pane_y());
		}
		for (r = 1; r < scr_rows - 1; r++)
			memcpy(cur + (size_t)r * (size_t)scr_cols,
				nxt + (size_t)r * (size_t)scr_cols,
				(size_t)scr_cols * sizeof *cur);
		flush_row(0, full);
		flush_row(scr_rows - 1, full);
	} else
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
	if (px_mode && pane_pm)
		XCopyArea(dpy, pane_pm, win, gc, 0, 0, (unsigned)pm_w,
			(unsigned)pm_h, pane_x(), pane_y());
	for (r = 0; r < scr_rows; r++) {
		const struct cell *cl = cur + (size_t)r * (size_t)scr_cols;

		if (px_mode && r > 0 && r < scr_rows - 1)
			continue;

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
		scr_mouse_x = x - pane_x() - PANE_IN;
		scr_mouse_y = y - pane_y();
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
