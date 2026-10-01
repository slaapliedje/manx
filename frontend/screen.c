/*
 * screen.c - the text screen, over terminfo (with ANSI defaults when the
 * terminal is unknown).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "os.h"
#include "utf8.h"
#include "entropy.h"
#include "screen.h"

/* terminfo, declared here: the headers of ncurses and SVR4 disagree */
extern int setupterm(char *term, int fd, int *err);
extern char *tigetstr(char *cap);
extern int tigetnum(char *cap);
#ifdef MANX_SYSV4
extern char *tparm(char *s, long, long, long, long, long, long, long, long,
	long);
#define TPARM2(s, a, b)	tparm(s, a, b, 0, 0, 0, 0, 0, 0, 0)
#else
extern char *tparm(const char *s, ...);
#define TPARM2(s, a, b)	tparm(s, (long)(a), (long)(b), 0L, 0L, 0L, 0L, 0L, \
	0L, 0L)
#endif

int scr_rows = 24, scr_cols = 80;
int scr_color = 1;
int scr_link_color = 6;
enum term_cs scr_cs = TCS_ASCII;
int scr_mouse_row, scr_mouse_col;
long scr_scroll_target;
const char *scr_font;
const char *scr_needs = "a terminal";

struct cell {
	unsigned char b[4];
	unsigned char n;		/* bytes; 0: right half of a wide char */
	unsigned char a;
};

static struct cell *cur, *nxt;		/* on the terminal / being built */
static int cur_row = -1, cur_col = -1;	/* where scr_cursor asked */
static int opened;
static struct termios saved_tio;

/* capabilities (NULL: not available) */
static char *c_cup, *c_clear, *c_el, *c_sgr0, *c_bold, *c_smul, *c_rev,
	*c_setaf, *c_civis, *c_cnorm, *c_smcup, *c_rmcup, *c_smkx, *c_rmkx,
	*c_bel;
static int has_terminfo, ncolors;

/* --- output ------------------------------------------------------------ */

static char obuf[8192];
static size_t olen;

static void oflush(void)
{
	size_t off = 0;

	while (off < olen) {
		long w = write(1, obuf + off, olen - off);

		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			break;
		}
		off += (size_t)w;
	}
	olen = 0;
}

static void oput(const char *s, size_t n)
{
	if (olen + n > sizeof obuf)
		oflush();
	if (n > sizeof obuf) {
		(void)!write(1, s, n);
		return;
	}
	memcpy(obuf + olen, s, n);
	olen += n;
}

/* a capability string, without its $<..> padding */
static void ocap(const char *s)
{
	if (s == NULL)
		return;
	while (*s) {
		if (s[0] == '$' && s[1] == '<') {
			const char *e = strchr(s, '>');

			if (e) {
				s = e + 1;
				continue;
			}
		}
		oput(s, 1);
		s++;
	}
}

static void omove(int row, int col)
{
	if (c_cup)
		ocap(TPARM2(c_cup, row, col));
	else {
		char b[32];

		sprintf(b, "\033[%d;%dH", row + 1, col + 1);
		oput(b, strlen(b));
	}
}

static int out_attr = -1;

static void oattr(int a)
{
	if (a == out_attr)
		return;
	ocap(c_sgr0 ? c_sgr0 : "\033[m");
	if (a & CA_BOLD)
		ocap(c_bold ? c_bold : "\033[1m");
	if ((a & CA_UNDER) || ((a & CA_LINK) && !ncolors))
		ocap(c_smul ? c_smul : "\033[4m");
	if ((a & CA_REV) || ((a & CA_MARK) && !ncolors))
		ocap(c_rev ? c_rev : "\033[7m");
	if (ncolors && (a & (CA_LINK | CA_MARK)) && !(a & CA_REV)) {
		/* links cyan (or as set), find matches yellow */
		int color = (a & CA_MARK) ? 3 : scr_link_color;

		if (c_setaf)
			ocap(TPARM2(c_setaf, color, 0));
		else {
			char b[12];

			sprintf(b, "\033[3%dm", color);
			oput(b, strlen(b));
		}
	}
	out_attr = a;
}

/* --- terminal modes ------------------------------------------------------ */

static void raw_mode(void)
{
	struct termios t = saved_tio;

	t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
	t.c_iflag &= ~(tcflag_t)(IXON | ICRNL | INLCR | ISTRIP | BRKINT);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSAFLUSH, &t);
}

static char *cap(char *name)
{
	char *s;

	if (!has_terminfo)
		return NULL;
	s = tigetstr(name);
	if (s == NULL || s == (char *)-1 || !*s)
		return NULL;
	return s;
}

static void load_caps(void)
{
	int err = 0;

	has_terminfo = getenv("TERM") && setupterm(NULL, 1, &err) == 0;
	c_cup = cap("cup");
	c_clear = cap("clear");
	c_el = cap("el");
	c_sgr0 = cap("sgr0");
	c_bold = cap("bold");
	c_smul = cap("smul");
	c_rev = cap("rev");
	c_setaf = cap("setaf");
	c_civis = cap("civis");
	c_cnorm = cap("cnorm");
	c_smcup = cap("smcup");
	c_rmcup = cap("rmcup");
	c_smkx = cap("smkx");
	c_rmkx = cap("rmkx");
	c_bel = cap("bel");
	ncolors = has_terminfo ? tigetnum("colors") : 0;
	if (ncolors < 8 || (c_setaf == NULL && !has_terminfo))
		ncolors = 0;
	if (getenv("MANX_NOCOLOR") || !scr_color)
		ncolors = 0;
	if (!has_terminfo) {
		/* an unknown terminal: assume ANSI (VT100 and later) */
		c_clear = "\033[H\033[J";
		c_el = "\033[K";
	}
}

/* --- keyboard ---------------------------------------------------------- */

static unsigned char ibuf[64];
static int ilen;

/* wait up to ms for input and append it to ibuf: 1 if something came */
static int fill(int ms)
{
	struct pollfd p;
	long n;

	if (ilen == (int)sizeof ibuf)
		return 1;
	p.fd = 0;
	p.events = POLLIN;
	p.revents = 0;
	if (poll(&p, 1, ms) <= 0)
		return 0;
	n = read(0, ibuf + ilen, sizeof ibuf - (size_t)ilen);
	if (n <= 0)
		return 0;
	ilen += (int)n;
	entropy_event();	/* key timing feeds the TLS entropy pool */
	return 1;
}

struct keyseq {
	const char *s;
	int key;
};

static struct keyseq keys[40];
static int nkeys;

static void add_key(const char *s, int key)
{
	int i;

	if (s == NULL || !*s || nkeys == (int)(sizeof keys / sizeof keys[0]))
		return;
	for (i = 0; i < nkeys; i++)
		if (strcmp(keys[i].s, s) == 0)
			return;
	keys[nkeys].s = s;
	keys[nkeys].key = key;
	nkeys++;
}

static void load_keys(void)
{
	static const struct { char *cap; int key; } tk[] = {
		{ "kcuu1", K_UP }, { "kcud1", K_DOWN }, { "kcub1", K_LEFT },
		{ "kcuf1", K_RIGHT }, { "kpp", K_PGUP }, { "knp", K_PGDN },
		{ "khome", K_HOME }, { "kend", K_END }, { "kcbt", K_BTAB },
		{ "kdch1", K_DEL }, { "kich1", K_INS }, { "kf1", K_F1 },
	};
	static const struct keyseq ansi[] = {
		{ "\033[A", K_UP }, { "\033[B", K_DOWN }, { "\033[D", K_LEFT },
		{ "\033[C", K_RIGHT }, { "\033OA", K_UP }, { "\033OB", K_DOWN },
		{ "\033OD", K_LEFT }, { "\033OC", K_RIGHT },
		{ "\033[5~", K_PGUP }, { "\033[6~", K_PGDN },
		{ "\033[H", K_HOME }, { "\033[F", K_END }, { "\033OH", K_HOME },
		{ "\033OF", K_END }, { "\033[1~", K_HOME }, { "\033[4~", K_END },
		{ "\033[7~", K_HOME }, { "\033[8~", K_END }, { "\033[Z", K_BTAB },
		{ "\033[3~", K_DEL }, { "\033[2~", K_INS }, { "\033OP", K_F1 },
		{ "\033[11~", K_F1 },
	};
	size_t i;

	nkeys = 0;
	for (i = 0; i < sizeof tk / sizeof tk[0]; i++)
		add_key(cap(tk[i].cap), tk[i].key);
	for (i = 0; i < sizeof ansi / sizeof ansi[0]; i++)
		add_key(ansi[i].s, ansi[i].key);
}

static void consume(int n)
{
	memmove(ibuf, ibuf + n, (size_t)(ilen - n));
	ilen -= n;
}

int scr_getkey(int timeout_ms)
{
	int c, i, waited = 0;

	if (ilen == 0 && !fill(timeout_ms))
		return -1;
	c = ibuf[0];
	if (c != 27) {
		consume(1);
		if (c == '\n')
			c = '\r';
		return c;
	}
	/* an escape sequence, or the Escape key: match what has come */
	for (;;) {
		int prefix = 0;

		for (i = 0; i < nkeys; i++) {
			size_t n = strlen(keys[i].s);

			if ((size_t)ilen >= n
				&& memcmp(ibuf, keys[i].s, n) == 0) {
				consume((int)n);
				return keys[i].key;
			}
			if ((size_t)ilen < n
				&& memcmp(ibuf, keys[i].s, (size_t)ilen) == 0)
				prefix = 1;
		}
		/* more may be on its way (slow links: allow 300 ms) */
		if (!prefix || waited >= 300 || !fill(100)) {
			if (prefix && waited < 300) {
				waited += 100;
				continue;
			}
			break;
		}
		waited += 100;
	}
	if (ilen >= 2 && ibuf[1] == '[') {
		/* an unknown CSI sequence: drop it whole */
		for (i = 2; i < ilen; i++)
			if (ibuf[i] >= 0x40 && ibuf[i] <= 0x7E) {
				consume(i + 1);
				return -1;
			}
	}
	consume(1);
	return 27;
}

/* --- the character set -------------------------------------------------- */

static int has_utf8(const char *s)
{
	if (s == NULL)
		return 0;
	for (; *s; s++)
		if ((s[0] == 'U' || s[0] == 'u') && (s[1] == 'T' || s[1] == 't')
			&& (s[2] == 'F' || s[2] == 'f'))
			return 1;
	return 0;
}

/* Write é and ask where the cursor went: one column on a UTF-8
 * terminal, two on an 8-bit one, no answer from a terminal that can't
 * say. */
static enum term_cs probe_cs(void)
{
	static const char q[] = "\r\303\251\033[6n";
	unsigned char r[32];
	int n = 0, row = 0, col = 0, i;
	unsigned long t0 = os_msec();

	(void)!write(1, q, sizeof q - 1);
	while (os_msec() - t0 < 500 && n < (int)sizeof r - 1) {
		struct pollfd p;
		long k;

		p.fd = 0;
		p.events = POLLIN;
		p.revents = 0;
		if (poll(&p, 1, 100) <= 0)
			continue;
		k = read(0, r + n, sizeof r - 1 - (size_t)n);
		if (k <= 0)
			break;
		n += (int)k;
		if (memchr(r, 'R', (size_t)n))
			break;
	}
	(void)!write(1, "\r   \r", 5);
	r[n] = '\0';
	for (i = 0; i < n; i++)
		if (r[i] == 27 && sscanf((char *)r + i, "\033[%d;%dR", &row,
			&col) == 2)
			break;
	if (col == 2)
		return TCS_UTF8;
	if (col == 3)
		return TCS_LATIN1;
	return TCS_ASCII;
}

/* --- cells ------------------------------------------------------------- */

static void blank(struct cell *c)
{
	c->b[0] = ' ';
	c->n = 1;
	c->a = 0;
}

static int alloc_cells(void)
{
	size_t n = (size_t)scr_rows * (size_t)scr_cols;
	size_t i;

	xfree(cur);
	xfree(nxt);
	cur = xmalloc(n * sizeof *cur);
	nxt = xmalloc(n * sizeof *nxt);
	if (cur == NULL || nxt == NULL)
		return -1;
	for (i = 0; i < n; i++) {
		blank(&nxt[i]);
		cur[i].n = 0xFF;	/* unknown: redraw */
	}
	return 0;
}

static void get_size(int *rows, int *cols)
{
	struct winsize w;
	const char *e;

	*rows = 0;
	*cols = 0;
	if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row > 2 && w.ws_col > 10) {
		*rows = w.ws_row;
		*cols = w.ws_col;
		return;
	}
	if ((e = getenv("LINES")) != NULL)
		*rows = atoi(e);
	if ((e = getenv("COLUMNS")) != NULL)
		*cols = atoi(e);
	if (*rows < 3 && has_terminfo)
		*rows = tigetnum("lines");
	if (*cols < 10 && has_terminfo)
		*cols = tigetnum("cols");
	if (*rows < 3)
		*rows = 24;
	if (*cols < 10)
		*cols = 80;
}

int scr_check_size(void)
{
	int r, c;

	get_size(&r, &c);
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

/* --- public ------------------------------------------------------------ */

int scr_open(const char *cs_env)
{
	if (!isatty(0) || !isatty(1))
		return -1;
	tcgetattr(0, &saved_tio);
	load_caps();
	load_keys();
	raw_mode();
	if (cs_env && *cs_env) {
		if (has_utf8(cs_env))
			scr_cs = TCS_UTF8;
		else if (strstr(cs_env, "latin") || strstr(cs_env, "8859"))
			scr_cs = TCS_LATIN1;
		else
			scr_cs = TCS_ASCII;
	} else if (has_utf8(getenv("LC_ALL")) || has_utf8(getenv("LC_CTYPE"))
		|| has_utf8(getenv("LANG")))
		scr_cs = TCS_UTF8;
	else
		scr_cs = probe_cs();
	ocap(c_smcup);
	ocap(c_smkx);
	oflush();
	opened = 1;
	scr_check_size();
	scr_flush(1);
	return 0;
}

void scr_close(void)
{
	if (!opened)
		return;
	oattr(0);
	ocap(c_sgr0 ? c_sgr0 : "\033[m");
	ocap(c_cnorm);
	ocap(c_rmkx);
	if (c_rmcup)
		ocap(c_rmcup);
	else {
		omove(scr_rows - 1, 0);
		oput("\r\n", 2);
	}
	oflush();
	tcsetattr(0, TCSAFLUSH, &saved_tio);
	opened = 0;
}

void scr_suspend(void)
{
	scr_close();
}

void scr_resume(void)
{
	raw_mode();
	ocap(c_smcup);
	ocap(c_smkx);
	oflush();
	opened = 1;
	out_attr = -1;
	scr_flush(1);
}

void scr_erase(void)
{
	size_t i, n = (size_t)scr_rows * (size_t)scr_cols;

	for (i = 0; i < n; i++)
		blank(&nxt[i]);
}

int scr_put(int row, int col, const char *s, int n, int attr)
{
	const char *e = s + n;
	struct cell *line;
	int c0 = col;

	if (row < 0 || row >= scr_rows || col < 0)
		return 0;
	line = nxt + (size_t)row * (size_t)scr_cols;
	while (s < e && col < scr_cols) {
		if (scr_cs != TCS_UTF8 || (unsigned char)*s < 0x80) {
			line[col].b[0] = (unsigned char)*s++;
			line[col].n = 1;
			line[col].a = (unsigned char)attr;
			col++;
			continue;
		}
		{
			const char *c = s;
			unsigned long cp = utf8_get(&s);
			int w = ucs_width(cp), k = (int)(s - c);

			if (w == 0) {
				/* combining: onto the previous cell, if room */
				if (col > c0 && line[col - 1].n
					&& line[col - 1].n + k <= 4) {
					memcpy(line[col - 1].b + line[col - 1].n,
						c, (size_t)k);
					line[col - 1].n = (unsigned char)
						(line[col - 1].n + k);
				}
				continue;
			}
			if (w == 2 && col + 1 >= scr_cols) {
				blank(&line[col]);
				col++;
				break;
			}
			memcpy(line[col].b, c, (size_t)k);
			line[col].n = (unsigned char)k;
			line[col].a = (unsigned char)attr;
			col++;
			if (w == 2) {
				line[col].n = 0;
				line[col].a = (unsigned char)attr;
				col++;
			}
		}
	}
	return col - c0;
}

void scr_fill(int row, int col, int ncols, int ch, int attr)
{
	struct cell *line;

	if (row < 0 || row >= scr_rows)
		return;
	line = nxt + (size_t)row * (size_t)scr_cols;
	for (; ncols > 0 && col < scr_cols; ncols--, col++) {
		line[col].b[0] = (unsigned char)ch;
		line[col].n = 1;
		line[col].a = (unsigned char)attr;
	}
}

void scr_cursor(int row, int col)
{
	cur_row = row;
	cur_col = col;
}

static int same(const struct cell *a, const struct cell *b)
{
	return a->n == b->n && a->a == b->a
		&& (a->n == 0 || memcmp(a->b, b->b, a->n) == 0);
}

void scr_flush(int full)
{
	int r;

	if (full) {
		size_t i, n = (size_t)scr_rows * (size_t)scr_cols;

		out_attr = -1;
		oattr(0);
		ocap(c_clear);
		for (i = 0; i < n; i++)
			blank(&cur[i]);
	}
	ocap(c_civis);
	for (r = 0; r < scr_rows; r++) {
		struct cell *nl = nxt + (size_t)r * (size_t)scr_cols;
		struct cell *cl = cur + (size_t)r * (size_t)scr_cols;
		/* the bottom right cell is never written: it would scroll */
		int last = r == scr_rows - 1 ? scr_cols - 2 : scr_cols - 1;
		int c0, c1, lnb, c;

		for (c0 = 0; c0 <= last && same(&nl[c0], &cl[c0]); c0++)
			;
		if (c0 > last)
			continue;
		for (c1 = last; c1 > c0 && same(&nl[c1], &cl[c1]); c1--)
			;
		while (c0 > 0 && nl[c0].n == 0)
			c0--;		/* start at a wide char's left half */
		/* a blank tail is cleared with el */
		for (lnb = last; lnb >= c0 && nl[lnb].n == 1
			&& nl[lnb].b[0] == ' ' && nl[lnb].a == 0; lnb--)
			;
		omove(r, c0);
		if (c_el && c1 > lnb) {
			for (c = c0; c <= lnb; c++)
				if (nl[c].n) {
					oattr(nl[c].a);
					oput((char *)nl[c].b, nl[c].n);
				}
			oattr(0);
			ocap(c_el);
		} else
			for (c = c0; c <= c1; c++)
				if (nl[c].n) {
					oattr(nl[c].a);
					oput((char *)nl[c].b, nl[c].n);
				}
		memcpy(cl, nl, (size_t)scr_cols * sizeof *cl);
	}
	oattr(0);
	if (cur_row >= 0) {
		omove(cur_row, cur_col);
		ocap(c_cnorm);
	}
	oflush();
}

void scr_bell(void)
{
	ocap(c_bel ? c_bel : "\007");
	oflush();
}

void scr_title(const char *title)
{
	(void)title;
}

/* (a terminal has no controls) */
void scr_url(const char *url)
{
	(void)url;
}

void scr_scroll(long top, long rows, long total)
{
	(void)top;
	(void)rows;
	(void)total;
}

void scr_state(int can_back, int can_forward, int loading)
{
	(void)can_back;
	(void)can_forward;
	(void)loading;
}

int scr_url_edit(const char *text, int pos)
{
	(void)text;
	(void)pos;
	return 0;
}
