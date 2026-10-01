/*
 * xscreen.c - the screen of screen.h in an X11 window: the same cells,
 * drawn with a server font, plus the mouse. Raw Xlib, nothing newer than
 * X11R5 (AMIX's static libX11 talks to ASV's X11R6.3 server).
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
const char *scr_font;
const char *scr_needs = "an X display ($DISPLAY)";

#define PAD	2			/* pixels around the cells */

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

static struct cell *cur, *nxt;		/* in the window / being built */
static int cur_row = -1, cur_col = -1;	/* where scr_cursor asked */
static int drawn_row = -1, drawn_col = -1;	/* where the cursor is drawn */

/* the colours */
static unsigned long px_fg, px_bg, px_mark, px_link[8];
static int link_px_ok;

/* ANSI colours 0-7 for links, darkened for a white page */
static const char *const ansi_x[8] = {
	"#000000", "#b00000", "#007000", "#806000",
	"#0000c0", "#a000a0", "#007878", "#606060"
};

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

	px_fg = color("black", BlackPixel(dpy, scr), NULL);
	px_bg = color("white", WhitePixel(dpy, scr), NULL);
	px_mark = color("#ffff60", px_bg, NULL);
	link_px_ok = 1;
	for (i = 0; i < 8; i++) {
		int ok;

		px_link[i] = color(ansi_x[i], px_fg, &ok);
		link_px_ok &= ok;
	}

	win_w = 2 * PAD + scr_cols * cw;
	win_h = 2 * PAD + scr_rows * ch;
	wa.background_pixel = px_bg;
	wa.border_pixel = px_fg;
	wa.event_mask = KeyPressMask | ButtonPressMask | ExposureMask
		| StructureNotifyMask;
	win = XCreateWindow(dpy, RootWindow(dpy, scr), 0, 0,
		(unsigned)win_w, (unsigned)win_h, 1, CopyFromParent, InputOutput,
		CopyFromParent, CWBackPixel | CWBorderPixel | CWEventMask, &wa);

	/* the window manager: steps of one cell, at least 20x5 */
	if ((sh = XAllocSizeHints()) != NULL) {
		sh->flags = PResizeInc | PMinSize | PBaseSize;
		sh->width_inc = cw;
		sh->height_inc = ch;
		sh->base_width = 2 * PAD;
		sh->base_height = 2 * PAD;
		sh->min_width = 2 * PAD + 20 * cw;
		sh->min_height = 2 * PAD + 5 * ch;
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

int scr_check_size(void)
{
	int r = (win_h - 2 * PAD) / ch, c = (win_w - 2 * PAD) / cw;

	if (r < 3)
		r = 3;
	if (c < 10)
		c = 10;
	if (r > 200)
		r = 200;
	if (c > 400)
		c = 400;
	if (r == scr_rows && c == scr_cols && cur)
		return 0;
	scr_rows = r;
	scr_cols = c;
	alloc_cells();
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
	int a = c->a, x = PAD + col * cw, y = PAD + row * ch + ascent, i;
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
	}
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

/* one event: a key, or -1 */
static int event(void)
{
	XEvent ev;

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
		scr_mouse_row = (ev.xbutton.y - PAD) / ch;
		scr_mouse_col = (ev.xbutton.x - PAD) / cw;
		if (ev.xbutton.y < PAD || ev.xbutton.x < PAD
			|| scr_mouse_row >= scr_rows || scr_mouse_col >= scr_cols)
			return -1;
		return K_MOUSE;
	case Expose:
		if (ev.xexpose.count == 0)
			redraw();
		return -1;
	case ConfigureNotify:
		win_w = ev.xconfigure.width;
		win_h = ev.xconfigure.height;
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
			if ((win_w - 2 * PAD) / cw != scr_cols
				|| (win_h - 2 * PAD) / ch != scr_rows)
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
