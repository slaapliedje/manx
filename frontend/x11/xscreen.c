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
const char *scr_geometry;
const char *scr_needs = "an X display ($DISPLAY)";

#define PAD	2			/* pixels around the cells */
#define SBW	17			/* the scrollbar's width */
#define CPAD	4			/* around the control area's buttons */

struct cell {
	unsigned char ch;
	unsigned char a;
	unsigned char fg;		/* the page's colour (scr_palette), 0: none */
};

static Display *dpy;
static Window win;
static GC gc;
static GC sgc;				/* scrolling: reports what it can't copy */
static XFontStruct *font, *bold;	/* bold: NULL to overstrike */
static int cw, ch, ascent;		/* cell size, baseline */
static Atom wm_delete;
static int win_w, win_h;		/* the window's size */
static int closing;

/* where things are: the control area (0..ctrl_h), the cells (from gx,
 * gy), the scrollbar (sb_x..win_w, gy..win_h) */
static int ctrl_h, bh, gx, gy, sb_x;
static int gx_at = -1, gy_at = -1;	/* where geometry asked to be put */

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
/* the page's colours as pixels: text_px[i] for CA_FG(i) where text_ok[i]
 * (scr_palette) */
static unsigned long text_px[256];
static unsigned char text_ok[256];
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

/*
 * MANX_XTRACE (set to anything, or a number N: stop for good after N
 * requests, to see what the server has drawn): each request waited for
 * before the next goes out, and logged first to stderr with its sequence
 * number - and its opcode (Xproto.h's X_*) where the Display is public
 * (X11R4/R5: AMIX's) - and the events that come in. The last line before
 * a hang names the request a server never answers. MANX_XGET=N: from
 * request N on, a pixel read back after each as well (see below).
 */
static int xtrace_on;			/* MANX_XTRACE: events logged too */
static long xtrace_stop, xtrace_n;	/* MANX_XTRACE=N: stop after N */
static long xtrace_get;		/* MANX_XGET=N: a pixel read back from N on */

static int xtrace_after(Display *d)
{
	static int busy;	/* (XGetImage below ends in this function too) */

	if (busy)
		return 0;
	busy = 1;
	xtrace_n++;
	if (xtrace_stop && xtrace_n >= xtrace_stop) {
		XSync(d, False);
		fprintf(stderr, "xt: stopped after %ld requests\n", xtrace_n);
		for (;;)
			sleep(60);
	}
#ifndef XlibSpecificationRelease
	fprintf(stderr, "xt %lu op %d\n", d->request,
		*(unsigned char *)d->last_req);
#else
	fprintf(stderr, "xt %lu\n", NextRequest(d) - 1);
#endif
	XSync(d, False);
	/* reading a pixel back makes a server finish drawing first: a server
	 * that hands its drawing to a board stops here, after the request the
	 * board could not do */
	if (xtrace_get && xtrace_n >= xtrace_get) {
		XImage *im = XGetImage(d, RootWindow(d, DefaultScreen(d)), 0, 0,
			1, 1, AllPlanes, ZPixmap);

		if (im)
			XDestroyImage(im);
		fprintf(stderr, "xt   (drawn)\n");
	}
	busy = 0;
	return 0;
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
	if (getenv("MANX_XTRACE")) {
		xtrace_on = 1;
		xtrace_stop = atol(getenv("MANX_XTRACE"));
		xtrace_get = getenv("MANX_XGET") ? atol(getenv("MANX_XGET")) : 0;
		XSetAfterFunction(dpy, xtrace_after);
	}
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

	/* the size asked for: columns and rows of cells, as xterm takes it */
	if (scr_geometry) {
		int c = 0, r = 0;
		char sx = '+', sy = '+';

		if (sscanf(scr_geometry, "%dx%d%c%d%c%d", &c, &r, &sx, &gx_at, &sy, &gy_at) >= 2
			&& c >= 40 && c <= 400 && r >= 5 && r <= 200) {
			scr_cols = c;
			scr_rows = r;
		}
		if (sx == '-' || sy == '-')
			gx_at = gy_at = -1;	/* (only +X+Y) */
	}
	win_w = 2 * PAD + scr_cols * cw + SBW;
	win_h = gy + PAD + scr_rows * ch;
	sb_x = win_w - SBW;
	wa.background_pixel = px_bg;
	wa.border_pixel = px_fg;
	wa.event_mask = KeyPressMask | ButtonPressMask | ButtonReleaseMask
		| ButtonMotionMask | ExposureMask | StructureNotifyMask;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), gx_at > 0 ? gx_at : 0,
		gy_at > 0 ? gy_at : 0,
		(unsigned)win_w, (unsigned)win_h, 1, CopyFromParent, InputOutput,
		CopyFromParent, CWBackPixel | CWBorderPixel | CWEventMask, &wa);

	/* the window manager: steps of one cell, at least 40x5 */
	if ((sh = XAllocSizeHints()) != NULL) {
		sh->flags = PResizeInc | PMinSize | PBaseSize;
		if (scr_geometry) {
			sh->flags |= USSize;
			if (gx_at >= 0 && gy_at >= 0) {
				sh->flags |= USPosition;
				sh->x = gx_at;
				sh->y = gy_at;
			}
			sh->width = win_w;
			sh->height = win_h;
		}
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
	/* copies with gc make no events; scrolling's (sgc) say what part of
	 * the pane another window hid, which is then painted */
	XSetGraphicsExposures(dpy, gc, False);
	sgc = XCreateGC(dpy, win, 0, NULL);
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

/*
 * Z: the window to fill the screen, and back. The window manager's frame
 * (title, borders) is measured, and the frame asked to sit at 0,0: with
 * the window's gravity NorthWest, a move asks where the frame goes (twm
 * and ICCCM alike).
 */
static int zoomed, zoom_x, zoom_y, zoom_w, zoom_h;

int scr_zoom(void)
{
	Window root, parent, *kids, top = win;
	unsigned n, w, h, bw, depth;
	int x, y, sw, sh;

	if (zoomed) {
		zoomed = 0;
		XMoveResizeWindow(dpy, win, zoom_x, zoom_y, (unsigned)zoom_w,
			(unsigned)zoom_h);
		return 0;
	}
	/* the frame: the window's ancestor just below the root */
	while (XQueryTree(dpy, top, &root, &parent, &kids, &n)) {
		if (kids)
			XFree((char *)kids);
		if (parent == root)
			break;
		top = parent;
	}
	if (!XGetGeometry(dpy, top, &root, &x, &y, &w, &h, &bw, &depth))
		return -1;
	sw = DisplayWidth(dpy, DefaultScreen(dpy));
	sh = DisplayHeight(dpy, DefaultScreen(dpy));
	zoom_x = x;
	zoom_y = y;
	zoom_w = win_w;
	zoom_h = win_h;
	zoomed = 1;
	/* what the frame adds round the window, kept in the new size */
	XMoveResizeWindow(dpy, win, 0, 0,
		(unsigned)(sw - ((int)(w + 2 * bw) - win_w)),
		(unsigned)(sh - ((int)(h + 2 * bw) - win_h)));
	return 0;
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
	unsigned char fg;		/* the page's colour (scr_palette), 0: none */
	unsigned off, n;		/* in rtext */
};

static struct run *runs;
static int nruns, runs_cap;

/* an image drawn this frame (im NULL: a frame where one will be) */
struct ximg;
struct idraw {
	struct ximg *im;
	short x, y, w, h;
	int attr;
};

static struct idraw *idraws;
static int nidraws, idraws_cap;
static char *rtext;
static unsigned rtext_len, rtext_cap;
static unsigned long pane_sum = 1, drawn_sum;	/* this frame's, the shown one's */

/*
 * The pane is drawn straight onto the window (a back-buffer pixmap cost a
 * whole pane's copy over the VME bus for every change: 258 ms at 500x350
 * and 32 bits a pixel on the TT, 828 ms at 1000x650). What is on the
 * window is kept as the shown frame: its runs, their text and its images
 * here, the cells over the pane in cur[]. A new frame is compared with
 * it: when most of it is the shown frame moved up or down, the window's
 * pixels are moved (window to window, which Xatw does with the ATW800's
 * 2D engine: 35-58 ms), and only what is new or different is painted,
 * in bands across the pane.
 */
static struct run *sruns;
static int nsruns, sruns_cap;
static char *stext;
static unsigned stext_len, stext_cap;
static struct idraw *sidraws;
static int nsidraws, sidraws_cap;
static int shown_ok;			/* the window's pane shows that frame */
static int shown_w, shown_h;		/* at this pane size */
static int shown_over;			/* with cells over the pane */

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

/* page text for the X fonts: a no-break space as a space, which looks the
 * same (layout has already kept the words together). X11R4's fonts, the
 * ones Helios has, have no glyph at 0xA0, so it would take no room. */
static const char *x_text(const char *s, int n)
{
	static char buf[512];
	int i;

	if (n > (int)sizeof buf || memchr(s, 0xA0, (size_t)n) == NULL)
		return s;
	for (i = 0; i < n; i++)
		buf[i] = (unsigned char)s[i] == 0xA0 ? ' ' : s[i];
	return buf;
}

static int m_width(void *ctx, int attr, int face, const char *s, int n)
{
	(void)ctx;
	return XTextWidth(face_font(attr & SA_BOLD ? CA_BOLD : 0, face),
		(char *)x_text(s, n), n);
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
	r->fg = (unsigned char)CA_FG_OF(attr);
	r->off = rtext_len;
	r->n = (unsigned)n;
	rtext_len += (unsigned)n;
}

static int cells_over;			/* cells drawn over the pane this frame */

/* a checksum of the frame's pane: its runs, its images' places, and the
 * cells over it (not the images' rows: those come in as they decode, and
 * are copied on their own) */
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
	cells_over = 0;
	n = (size_t)(scr_rows - 2) * (size_t)scr_cols;
	for (i = 0; i < n && !cells_over; i++)
		cells_over = nxt[scr_cols + i].ch != 0;
	return h | 1;
}

/* the pane rows [*y0, *y1) run r can touch: glyphs, a fill behind them,
 * the underline */
static void run_rows(const struct run *r, int *y0, int *y1)
{
	XFontStruct *f = face_font(r->attr, r->face);
	int base = r->y + r->a;
	int up = f->max_bounds.ascent > f->ascent ? f->max_bounds.ascent : f->ascent;
	int down = f->max_bounds.descent > f->descent ? f->max_bounds.descent : f->descent;

	*y0 = base - up;
	*y1 = base + (down > 2 ? down : 2) + 1;
}

/* one run (its text in text), onto the window */
static void paint_run(const struct run *r, const char *text)
{
	XFontStruct *f = face_font(r->attr, r->face);
	int x = pane_x() + PANE_IN + r->x, base = pane_y() + r->y + r->a;
	char *t = (char *)x_text(text + r->off, (int)r->n);
	int w = XTextWidth(f, t, (int)r->n);
	unsigned long fg = px_fg, bg = px_bg;
	int fill = 0;

	if (r->fg && text_ok[r->fg])
		fg = text_px[r->fg];
	else if ((r->attr & CA_LINK) && link_px_ok)
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
		XFillRectangle(dpy, win, gc, x - 1, base - f->ascent, (unsigned)(w + 2),
			(unsigned)(f->ascent + f->descent));
	}
	XSetForeground(dpy, gc, fg);
	XSetFont(dpy, gc, f->fid);
	XDrawString(dpy, win, gc, x, base, t, (int)r->n);
	if ((r->attr & CA_UNDER) || ((r->attr & CA_LINK) && !(r->attr & CA_MARK)))
		XDrawLine(dpy, win, gc, x, base + 1, x + w - 1, base + 1);
}

/* --- images ----------------------------------------------------------------- */

struct ximg {
	Pixmap pm, mask;		/* mask: None without one */
	int w, h;
	int new0, new1;			/* rows put since it was last painted
					 * (new0 > new1: none) */
	XImage *row, *mrow;		/* a row of each: data pointed in */
};

/* an image's place on the window freed under it (scr_image_free): never
 * the same as a new one's, and painted as a frame */
static struct ximg gone;

static struct px_format pxf;
static int pxf_state;			/* 0 not yet, 1 ready, -1 no images */
static GC mask_gc;
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

/* channel v (0-255) into a TrueColor mask */
static unsigned long in_mask(unsigned long v, unsigned long mask)
{
	int shift = 0;

	if (mask == 0)
		return 0;
	while (!(mask >> shift & 1))
		shift++;
	return (v * (mask >> shift) / 255) << shift & mask;
}

/* the pixel for rgb, through the images' format (no server round trip,
 * and on an 8-bit screen no colormap cells beyond the images' cube): 0
 * when the screen can't show it (black and white) */
static int rgb_pixel(unsigned long rgb, unsigned long *px)
{
	const struct px_format *f = scr_pixels();
	unsigned long r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;

	if (f == NULL)
		return 0;
	switch (f->kind) {
	case PX_TRUE:
		*px = in_mask(r, f->mask[0]) | in_mask(g, f->mask[1]) | in_mask(b, f->mask[2]);
		return 1;
	case PX_CUBE:
		*px = f->pixel[((r * (unsigned long)(f->levels[0] - 1) + 127) / 255
			* (unsigned long)f->levels[1]
			+ (g * (unsigned long)(f->levels[1] - 1) + 127) / 255)
			* (unsigned long)f->levels[2]
			+ (b * (unsigned long)(f->levels[2] - 1) + 127) / 255];
		return 1;
	case PX_GRAY:
		if (f->levels[0] <= 2)
			return 0;
		*px = f->pixel[((r * 299 + g * 587 + b * 114) / 1000
			* (unsigned long)(f->levels[0] - 1) + 127) / 255];
		return 1;
	}
	return 0;
}

void scr_palette(const unsigned long *rgb, int n)
{
	int i;

	memset(text_ok, 0, sizeof text_ok);
	for (i = 1; i <= n && i < 256 && scr_color; i++) {
		unsigned long c = rgb[i - 1], r = c >> 16 & 255, g = c >> 8 & 255,
			b = c & 255, y = (r * 299 + g * 587 + b * 114) / 1000;

		/* the page is white here: too pale to read is darkened, its
		 * hue kept */
		if (y > 150) {
			r = r * 150 / y;
			g = g * 150 / y;
			b = b * 150 / y;
		}
		text_ok[i] = (unsigned char)rgb_pixel(r << 16 | g << 8 | b, &text_px[i]);
	}
}

void scr_image_free(void *img)
{
	struct ximg *im = img;
	int i;

	if (im == NULL)
		return;
	/* (the shown frame's places for it: a frame, if painted again) */
	for (i = 0; i < nsidraws; i++)
		if (sidraws[i].im == im)
			sidraws[i].im = &gone;
	for (i = 0; i < nidraws; i++)
		if (idraws[i].im == im)
			idraws[i].im = &gone;
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
	im->new0 = h;
	im->new1 = -1;
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
	if (y < im->new0)
		im->new0 = y;
	if (y > im->new1)
		im->new1 = y;
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
	d->x = (short)x;
	d->y = (short)y;
	d->w = (short)w;
	d->h = (short)h;
	d->attr = attr;
}

/* the band being painted: its clip rectangle, which an image's mask
 * replaces for a moment */
static XRectangle band_clip;
static int band_on;

/* repainting part of the window only (what Expose events asked for):
 * bands keep to this box too */
static XRectangle ex_box;
static int ex_clip;

static void clip_band(int b0, int b1)
{
	int x0 = pane_x(), y0 = pane_y() + b0;
	int x1 = x0 + pane_wpx(), y1 = pane_y() + b1;

	if (ex_clip) {
		if (x0 < ex_box.x)
			x0 = ex_box.x;
		if (y0 < ex_box.y)
			y0 = ex_box.y;
		if (x1 > ex_box.x + ex_box.width)
			x1 = ex_box.x + ex_box.width;
		if (y1 > ex_box.y + ex_box.height)
			y1 = ex_box.y + ex_box.height;
		if (x1 < x0)
			x1 = x0;
		if (y1 < y0)
			y1 = y0;
	}
	band_clip.x = (short)x0;
	band_clip.y = (short)y0;
	band_clip.width = (unsigned short)(x1 - x0);
	band_clip.height = (unsigned short)(y1 - y0);
	XSetClipRectangles(dpy, gc, 0, 0, &band_clip, 1, Unsorted);
	band_on = 1;
}

static void clip_none(void)
{
	XSetClipMask(dpy, gc, None);
	XSetClipOrigin(dpy, gc, 0, 0);
	band_on = 0;
}

/* rows y0 to y1 - 1 of an image (those inside the pane), onto the window */
static void copy_rows(const struct idraw *d, int y0, int y1)
{
	int x = pane_x() + PANE_IN + d->x, y = pane_y() + d->y, H = pane_hpx();
	int w = d->w;

	/* (rows of the image, and in the pane) */
	if (y0 < 0)
		y0 = 0;
	if (y0 < -d->y)
		y0 = -d->y;
	if (y1 > d->im->h)
		y1 = d->im->h;
	if (y1 > H - d->y)
		y1 = H - d->y;
	if (w > pane_x() + pane_wpx() - x)
		w = pane_x() + pane_wpx() - x;
	if (y0 >= y1 || w <= 0)
		return;
	if (d->im->mask) {
		XSetClipMask(dpy, gc, d->im->mask);
		XSetClipOrigin(dpy, gc, x, y);
	}
	XCopyArea(dpy, d->im->pm, win, gc, 0, y0, (unsigned)w, (unsigned)(y1 - y0),
		x, y + y0);
	if (d->im->mask) {
		if (band_on)
			XSetClipRectangles(dpy, gc, 0, 0, &band_clip, 1, Unsorted);
		else
			clip_none();
	}
}

/* an image, the part of it in pane rows [b0, b1) */
static void paint_image(const struct idraw *d, int b0, int b1)
{
	int x = pane_x() + PANE_IN + d->x, y = pane_y() + d->y;
	unsigned long fg = (d->attr & CA_LINK) && link_px_ok ?
		px_link[scr_link_color & 7] : px_fg;

	if (d->im && d->im != &gone)
		copy_rows(d, b0 - d->y, b1 - d->y);
	else if (d->w > 2 && d->h > 2) {
		/* not here yet: a frame */
		XSetForeground(dpy, gc, DefaultDepth(dpy, DefaultScreen(dpy)) > 1 ?
			px_bg3 : px_fg);
		XDrawRectangle(dpy, win, gc, x, y, (unsigned)(d->w - 1),
			(unsigned)(d->h - 1));
	}
	if (d->attr & CA_REV) {
		/* the selected link: a frame round it */
		XSetForeground(dpy, gc, fg);
		XDrawRectangle(dpy, win, gc, x - 2, y - 2, (unsigned)(d->w + 3),
			(unsigned)(d->h + 3));
		XDrawRectangle(dpy, win, gc, x - 1, y - 1, (unsigned)(d->w + 1),
			(unsigned)(d->h + 1));
	}
}

/* a cell's colours and font */
static void cell_look(const struct cell *cl, unsigned long *fg, unsigned long *bg,
	XFontStruct **f)
{
	*fg = px_fg;
	*bg = px_bg;
	if (cl->fg && text_ok[cl->fg])
		*fg = text_px[cl->fg];
	else if ((cl->a & CA_LINK) && link_px_ok)
		*fg = px_link[scr_link_color & 7];
	if (cl->a & CA_REV) {
		unsigned long t = *fg;

		*fg = *bg;
		*bg = t;
	}
	*f = (cl->a & CA_BOLD) && bold ? bold : font;
}

/* the cells over the pane (ch 0: none) in pane rows [b0, b1), from the
 * grid g: a row's cells that look alike, side by side, in one request (a
 * screen of single characters was some 2000 requests: slow on every
 * server, and more than AMIX's X2410 on an emulated A2410 could take) */
static void paint_cells(const struct cell *g, int b0, int b1)
{
	char buf[400];			/* (scr_cols is at most 400) */
	int r, c;

	for (r = 1; r < scr_rows - 1; r++) {
		const struct cell *cl = g + (size_t)r * (size_t)scr_cols;
		int y = (r - 1) * ch;

		if (y >= b1 || y + ch <= b0)
			continue;
		c = 0;
		while (c < scr_cols) {
			unsigned long fg, bg, fg2, bg2;
			XFontStruct *f, *f2;
			int c0 = c, n = 0;

			if (cl[c].ch == 0) {
				c++;
				continue;
			}
			cell_look(&cl[c], &fg, &bg, &f);
			do {
				buf[n++] = (char)cl[c].ch;
				if (++c >= scr_cols || cl[c].ch == 0)
					break;
				cell_look(&cl[c], &fg2, &bg2, &f2);
			} while (fg2 == fg && bg2 == bg && f2 == f);
			XSetForeground(dpy, gc, fg);
			XSetBackground(dpy, gc, bg);
			XSetFont(dpy, gc, f->fid);
			XDrawImageString(dpy, win, gc, pane_x() + c0 * cw,
				pane_y() + y + ascent, buf, n);
		}
	}
}

/* a frame: runs (their text in text), images, the grid of cells over the
 * pane */
struct fview {
	const struct run *runs;
	int nruns;
	const char *text;
	const struct idraw *idraws;
	int nidraws;
	const struct cell *cells;
};

/* pane rows [b0, b1) of frame v, painted afresh */
static void paint_band(const struct fview *v, int b0, int b1)
{
	int i, y0, y1;

	if (b0 >= b1)
		return;
	clip_band(b0, b1);
	XSetForeground(dpy, gc, px_bg);
	XFillRectangle(dpy, win, gc, band_clip.x, band_clip.y, band_clip.width,
		band_clip.height);
	for (i = 0; i < v->nruns; i++) {
		run_rows(&v->runs[i], &y0, &y1);
		if (y1 > b0 && y0 < b1)
			paint_run(&v->runs[i], v->text);
	}
	for (i = 0; i < v->nidraws; i++) {
		const struct idraw *d = &v->idraws[i];

		if (d->y + d->h + 2 > b0 && d->y - 2 < b1)
			paint_image(d, b0, b1);
	}
	paint_cells(v->cells, b0, b1);
	clip_none();
}

/* --- what changed --------------------------------------------------------- */

struct band {
	int y0, y1;			/* pane rows [y0, y1) */
};

static struct band *bands;
static int nbands, bands_cap;

/* pane rows [y0, y1) to paint again */
static void damage(int y0, int y1)
{
	int h = pane_hpx();

	if (y0 < 0)
		y0 = 0;
	if (y1 > h)
		y1 = h;
	if (y0 >= y1)
		return;
	if (nbands == bands_cap) {
		int c = bands_cap ? bands_cap * 2 : 64;
		struct band *q = xrealloc(bands, (size_t)c * sizeof *q);

		if (q == NULL) {
			/* (no room to say which: all of it) */
			if (nbands > 0) {
				bands[0].y0 = 0;
				bands[0].y1 = h;
				nbands = 1;
			}
			return;
		}
		bands = q;
		bands_cap = c;
	}
	bands[nbands].y0 = y0;
	bands[nbands].y1 = y1;
	nbands++;
}

/* the bands in order, overlapping and touching ones joined */
static void merge_bands(void)
{
	int i, j, n = 0;

	for (i = 1; i < nbands; i++) {
		struct band t = bands[i];

		for (j = i; j > 0 && bands[j - 1].y0 > t.y0; j--)
			bands[j] = bands[j - 1];
		bands[j] = t;
	}
	for (i = 0; i < nbands; i++) {
		if (n > 0 && bands[i].y0 <= bands[n - 1].y1) {
			if (bands[i].y1 > bands[n - 1].y1)
				bands[n - 1].y1 = bands[i].y1;
		} else
			bands[n++] = bands[i];
	}
	nbands = n;
}

/* a run's hash, all but its y */
static unsigned long run_hash(const struct run *r, const char *text)
{
	unsigned long h = 5381;
	unsigned i;

	h = h * 33 + (unsigned short)r->x;
	h = h * 33 + (unsigned short)r->a;
	h = h * 33 + r->attr;
	h = h * 33 + r->face;
	h = h * 33 + r->fg;
	h = h * 33 + r->n;
	for (i = 0; i < r->n; i++)
		h = h * 33 + (unsigned char)text[r->off + i];
	return h;
}

/* the same run but for its y */
static int run_like(const struct run *a, const char *ta, const struct run *b,
	const char *tb)
{
	return a->x == b->x && a->a == b->a && a->attr == b->attr
		&& a->face == b->face && a->fg == b->fg && a->n == b->n
		&& memcmp(ta + a->off, tb + b->off, (size_t)a->n) == 0;
}

/* (arg: char *, as R5 has no XPointer) */
static Bool copy_event(Display *d, XEvent *e, char *arg)
{
	(void)d;
	(void)arg;
	return (e->type == GraphicsExpose && e->xgraphicsexpose.drawable == win)
		|| (e->type == NoExpose && e->xnoexpose.drawable == win);
}

/* move the pane's pixels by s rows (s < 0: up), and mark the rows that
 * leaves to paint: the strip uncovered, and any part another window hid
 * (nothing there to copy) */
static void scroll_pane(int s)
{
	int W = pane_wpx(), H = pane_hpx(), a = s < 0 ? -s : s;
	int px = pane_x(), py = pane_y();
	XEvent ev;

	if (s < 0) {
		XCopyArea(dpy, win, win, sgc, px, py + a, (unsigned)W, (unsigned)(H - a),
			px, py);
		damage(H - a, H);
	} else {
		XCopyArea(dpy, win, win, sgc, px, py, (unsigned)W, (unsigned)(H - a),
			px, py + a);
		damage(0, a);
	}
	for (;;) {
		XIfEvent(dpy, &ev, copy_event, NULL);
		if (ev.type == NoExpose)
			break;
		damage(ev.xgraphicsexpose.y - py,
			ev.xgraphicsexpose.y - py + ev.xgraphicsexpose.height);
		if (ev.xgraphicsexpose.count == 0)
			break;
	}
}

/*
 * The new frame (runs, rtext, idraws, nxt) against the shown one: find a
 * scroll, do it, and mark what is different afterwards. A run or image
 * the same as a shown one, moved by the scroll, is on the window
 * already; one that isn't is painted, and where a shown one was that
 * isn't any more is painted over.
 */
static void diff_frames(void)
{
	static int *slot;		/* hash table of shown runs: index, -1 */
	static unsigned char *used;
	static int slot_n, used_n;
	int votes_s[16], votes_n[16], nv = 0, i, k, s = 0, best = 0, mask;
	unsigned long h;
	int y0, y1;

	/* the shown runs, by hash */
	for (k = 64; k < 2 * nsruns; k *= 2)
		;
	if (k > slot_n) {
		int *q = xrealloc(slot, (size_t)k * sizeof *q);

		if (q == NULL) {
			damage(0, pane_hpx());
			return;
		}
		slot = q;
		slot_n = k;
	}
	if (nsruns > used_n) {
		unsigned char *q = xrealloc(used, (size_t)nsruns);

		if (q == NULL) {
			damage(0, pane_hpx());
			return;
		}
		used = q;
		used_n = nsruns;
	}
	mask = k - 1;
	for (i = 0; i < k; i++)
		slot[i] = -1;
	for (i = 0; i < nsruns; i++) {
		h = run_hash(&sruns[i], stext);
		while (slot[h & (unsigned long)mask] >= 0)
			h++;
		slot[h & (unsigned long)mask] = i;
	}
	memset(used, 0, (size_t)nsruns);

	/* a scroll: the shift most runs agree on */
	for (i = 0; i < nruns; i++) {
		int tries = 0;

		h = run_hash(&runs[i], rtext);
		for (; slot[h & (unsigned long)mask] >= 0 && tries < 4; h++) {
			const struct run *o = &sruns[slot[h & (unsigned long)mask]];
			int d, v;

			if (!run_like(&runs[i], rtext, o, stext))
				continue;
			tries++;
			d = runs[i].y - o->y;
			for (v = 0; v < nv && votes_s[v] != d; v++)
				;
			if (v == nv) {
				if (nv == 16)
					continue;
				votes_s[nv] = d;
				votes_n[nv++] = 0;
			}
			votes_n[v]++;
		}
	}
	for (i = 0; i < nv; i++)
		if (votes_n[i] > best) {
			best = votes_n[i];
			s = votes_s[i];
		}
	if (s == 0 || best < 3 || best * 3 < nruns || (s < 0 ? -s : s) >= pane_hpx()
		|| shown_over || cells_over)
		s = 0;
	if (s != 0)
		scroll_pane(s);

	/* the runs: kept, new, gone */
	for (i = 0; i < nruns; i++) {
		int found = 0;

		h = run_hash(&runs[i], rtext);
		for (; slot[h & (unsigned long)mask] >= 0; h++) {
			int j = slot[h & (unsigned long)mask];

			if (!used[j] && sruns[j].y + s == runs[i].y
				&& run_like(&runs[i], rtext, &sruns[j], stext)) {
				used[j] = 1;
				found = 1;
				break;
			}
		}
		if (!found) {
			run_rows(&runs[i], &y0, &y1);
			damage(y0, y1);
		}
	}
	for (i = 0; i < nsruns; i++)
		if (!used[i]) {
			run_rows(&sruns[i], &y0, &y1);
			damage(y0 + s, y1 + s);
		}

	/* the images (few: each against each) */
	for (i = 0; i < nidraws; i++) {
		const struct idraw *d = &idraws[i];

		for (k = 0; k < nsidraws; k++) {
			const struct idraw *o = &sidraws[k];

			if (o->im == d->im && o->x == d->x && o->y + s == d->y
				&& o->w == d->w && o->h == d->h && o->attr == d->attr)
				break;
		}
		if (k == nsidraws)
			damage(d->y - 2, d->y + d->h + 2);
	}
	for (k = 0; k < nsidraws; k++) {
		const struct idraw *o = &sidraws[k];

		for (i = 0; i < nidraws; i++) {
			const struct idraw *d = &idraws[i];

			if (o->im == d->im && o->x == d->x && o->y + s == d->y
				&& o->w == d->w && o->h == d->h && o->attr == d->attr)
				break;
		}
		if (i == nidraws)
			damage(o->y - 2 + s, o->y + o->h + 2 + s);
	}

	/* the cells over the pane (a menu): rows where they changed */
	if (s == 0 && (shown_over || cells_over)) {
		int r;

		for (r = 1; r < scr_rows - 1; r++)
			if (memcmp(cur + (size_t)r * (size_t)scr_cols,
				nxt + (size_t)r * (size_t)scr_cols,
				(size_t)scr_cols * sizeof *cur) != 0)
				damage((r - 1) * ch, r * ch);
	}
}

/* the new frame becomes the shown one */
static void keep_shown(void)
{
	if (nruns > sruns_cap) {
		struct run *q = xrealloc(sruns, (size_t)nruns * sizeof *q);

		if (q == NULL) {
			shown_ok = 0;
			return;
		}
		sruns = q;
		sruns_cap = nruns;
	}
	if (rtext_len > stext_cap) {
		char *q = xrealloc(stext, rtext_len);

		if (q == NULL) {
			shown_ok = 0;
			return;
		}
		stext = q;
		stext_cap = rtext_len;
	}
	if (nidraws > sidraws_cap) {
		struct idraw *q = xrealloc(sidraws, (size_t)nidraws * sizeof *q);

		if (q == NULL) {
			shown_ok = 0;
			return;
		}
		sidraws = q;
		sidraws_cap = nidraws;
	}
	if (nruns)
		memcpy(sruns, runs, (size_t)nruns * sizeof *runs);
	if (rtext_len)
		memcpy(stext, rtext, rtext_len);
	if (nidraws)
		memcpy(sidraws, idraws, (size_t)nidraws * sizeof *idraws);
	nsruns = nruns;
	stext_len = rtext_len;
	nsidraws = nidraws;
	shown_ok = 1;
}

/*
 * Images with rows come since they were painted: just those rows onto
 * the window (and the cells over them again, if a menu is open).
 */
static void paint_new_rows(void)
{
	int i;

	for (i = 0; i < nidraws; i++) {
		const struct idraw *d = &idraws[i];
		int y0, y1;

		if (d->im == NULL || d->im == &gone || d->im->new0 > d->im->new1)
			continue;
		y0 = d->im->new0;
		y1 = d->im->new1 + 1;
		copy_rows(d, y0, y1);
		if (cells_over) {
			clip_band(d->y + y0, d->y + y1);
			paint_cells(nxt, d->y + y0, d->y + y1);
			clip_none();
		}
	}
	for (i = 0; i < nidraws; i++)
		if (idraws[i].im && idraws[i].im != &gone) {
			idraws[i].im->new0 = idraws[i].im->h;
			idraws[i].im->new1 = -1;
		}
}

/* the pane of this frame onto the window: only what changed */
static void flush_pane(int full)
{
	struct fview v;
	int W = pane_wpx(), H = pane_hpx(), i;

	pane_sum = pane_checksum();
	nbands = 0;
	if (full || !shown_ok || shown_w != W || shown_h != H)
		damage(0, H);
	else if (pane_sum != drawn_sum)
		diff_frames();
	merge_bands();
	v.runs = runs;
	v.nruns = nruns;
	v.text = rtext;
	v.idraws = idraws;
	v.nidraws = nidraws;
	v.cells = nxt;
	for (i = 0; i < nbands; i++)
		paint_band(&v, bands[i].y0, bands[i].y1);
	if (pane_sum != drawn_sum || !shown_ok || full)
		keep_shown();
	drawn_sum = pane_sum;
	shown_w = W;
	shown_h = H;
	shown_over = cells_over;
	paint_new_rows();
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
		line[col].fg = (unsigned char)CA_FG_OF(attr);
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
		line[col].fg = (unsigned char)CA_FG_OF(attr);
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
	if (c->fg && text_ok[c->fg])
		fg = text_px[c->fg];
	else if ((a & CA_LINK) && link_px_ok)
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

		if (!full && nl[c].ch == cl[c].ch && nl[c].a == cl[c].a
			&& nl[c].fg == cl[c].fg) {
			c++;
			continue;
		}
		/* a run of changed cells with the same attribute */
		for (c1 = c + 1; c1 < scr_cols && nl[c1].a == nl[c].a && nl[c1].fg == nl[c].fg
			&& (full || nl[c1].ch != cl[c1].ch || nl[c1].a != cl[c1].a
			|| nl[c1].fg != cl[c1].fg);
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
		flush_pane(full);
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

/* Expose events' rectangles, until the last of a batch: their bounding box */
static int ex_x0, ex_y0, ex_x1, ex_y1, ex_n;

static void expose_add(const XExposeEvent *e)
{
	if (ex_n++ == 0) {
		ex_x0 = e->x;
		ex_y0 = e->y;
		ex_x1 = e->x + e->width;
		ex_y1 = e->y + e->height;
		return;
	}
	if (e->x < ex_x0)
		ex_x0 = e->x;
	if (e->y < ex_y0)
		ex_y0 = e->y;
	if (e->x + e->width > ex_x1)
		ex_x1 = e->x + e->width;
	if (e->y + e->height > ex_y1)
		ex_y1 = e->y + e->height;
}

/*
 * The window again where it was exposed: only that box is cleared and
 * drawn in, so dragging the window about (twm moves it opaquely)
 * repaints the strips it uncovers, not the whole page each time.
 */
static void redraw_box(void)
{
	int r, c, b0, b1;

	ex_n = 0;
	ex_box.x = (short)ex_x0;
	ex_box.y = (short)ex_y0;
	ex_box.width = (unsigned short)(ex_x1 - ex_x0);
	ex_box.height = (unsigned short)(ex_y1 - ex_y0);
	XClearArea(dpy, win, ex_x0, ex_y0, (unsigned)(ex_x1 - ex_x0),
		(unsigned)(ex_y1 - ex_y0), False);
	XSetClipRectangles(dpy, gc, 0, 0, &ex_box, 1, Unsorted);
	if (ex_y0 < ctrl_h)
		draw_controls();
	if (ex_x1 > sb_x)
		draw_scrollbar();
	for (r = 0; r < scr_rows; r++) {
		const struct cell *cl = cur + (size_t)r * (size_t)scr_cols;

		if ((px_mode && r > 0 && r < scr_rows - 1)
			|| gy + (r + 1) * ch <= ex_y0 || gy + r * ch >= ex_y1)
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
	b0 = ex_y0 - pane_y();
	b1 = ex_y1 - pane_y();
	if (b0 < 0)
		b0 = 0;
	if (b1 > pane_hpx())
		b1 = pane_hpx();
	if (px_mode && shown_ok && b0 < b1) {
		struct fview v;

		v.runs = sruns;
		v.nruns = nsruns;
		v.text = stext;
		v.idraws = sidraws;
		v.nidraws = nsidraws;
		v.cells = cur;
		ex_clip = 1;
		paint_band(&v, b0, b1);
		ex_clip = 0;
	}
	clip_none();
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
	if (xtrace_on)
		fprintf(stderr, "xe type %d %d,%d %dx%d count %d\n", ev.type,
			ev.xexpose.x, ev.xexpose.y, ev.xexpose.width,
			ev.xexpose.height, ev.xexpose.count);
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
		expose_add(&ev.xexpose);
		if (ev.xexpose.count == 0) {
			/* (a move's later exposures, if here already, join in) */
			while (XCheckTypedWindowEvent(dpy, win, Expose, &ev))
				expose_add(&ev.xexpose);
			redraw_box();
		}
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
