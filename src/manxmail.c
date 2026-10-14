/*
 * manxmail.c - mail on a terminal: IMAP to read it (mail/imap.h), each
 * message shown through Manx's HTML engine, over Manx's TLS. A program
 * of its own, so the browser stays as small as it is.
 *
 *   manxmail [FOLDER]
 *
 * Settings, in ~/.manx/config (or MANX_MAIL_IMAP and so on in the
 * environment):
 *   mail_imap = imap.gmail.com	the server: port 993, TLS (host:port)
 *   mail_user = name@gmail.com	the login
 *   mail_html = off		show a message's plain text first
 * The password: the first line of ~/.manx/mailpass, a file only its owner
 * can read (chmod 600) - else asked for. (Gmail: an app password, which
 * needs 2-Step Verification on.)
 *
 * Nothing a message links to is fetched: no pictures, no style sheets
 * (they tell the sender it was read). A link opens in manx.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "os.h"
#include "config.h"
#include "entropy.h"
#include "dns.h"
#include "tls.h"
#include "conn.h"
#include "screen.h"
#include "doc.h"
#include "load.h"
#include "layout.h"
#include "tags.h"
#include "utf8.h"
#include "imap.h"
#include "mime.h"
#include "smtp.h"
#include "compose.h"

#define PAGE_MSGS	50		/* messages fetched at a time */
#define MAX_PARTS	32

struct msg {
	unsigned long seq, uid;
	char from[96], subject[200], date[16];
	int seen, flagged, answered;
};

/* the message open, for a reply */
#define ORIG_ADDRS	32
static struct {
	struct addr from[4], reply_to[4], to[ORIG_ADDRS], cc[ORIG_ADDRS];
	int nfrom, nreply_to, nto, ncc;
	char date[80], subject[300], msgid[200];
	unsigned long uid;
} g_orig;

static struct imap g_m;
static char g_host[256], g_user[256], g_pass[256];
static unsigned g_port = 993;
static int g_tls = 1;
static char g_box[300] = "INBOX";	/* as the server names it */
static struct msg *g_msgs;		/* newest first */
static int g_n, g_cap, g_sel, g_top;
static char g_status[512];
static int g_quit;
static char g_manx[600];		/* manx, where manxmail was run from */

/* the message open */
static struct doc g_doc;
static struct page g_page;
static struct html_load g_load;
static int g_have_page;
static char g_hdr[8][480];
static int g_nhdr;
static struct mpart g_parts[MAX_PARTS];
static int g_nparts;
static long g_vtop, g_link = -1;

static void say(const char *fmt, const char *arg)
{
	snprintf(g_status, sizeof g_status, fmt, arg ? arg : "");
}

/* s into out (n bytes), cut short at a character's start if need be */
static void copy_to(char *out, size_t n, const char *s)
{
	size_t k = strlen(s);

	if (n == 0)
		return;
	if (k >= n) {
		k = n - 1;
		while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80)
			k--;
	}
	memcpy(out, s, k);
	out[k] = '\0';
}

/* --- text for the terminal --------------------------------------------- */

/* UTF-8 u into the terminal's character set, at most cols columns */
static void term_fit(const char *u, char *out, size_t n, int cols)
{
	size_t o = 0;
	int c = 0;

	while (*u && o + 6 < n && c < cols) {
		const char *s = u;
		unsigned long cp;
		int w;

		if ((unsigned char)*u < 0x80) {
			if ((unsigned char)*u >= 32)
				out[o++] = *u, c++;
			u++;
			continue;
		}
		cp = utf8_get(&u);
		if (scr_cs == TCS_UTF8) {
			w = ucs_width(cp);
			if (c + w > cols)
				break;
			memcpy(out + o, s, (size_t)(u - s));
			o += (size_t)(u - s);
			c += w;
		} else {
			o += (size_t)translit(cp, scr_cs == TCS_LATIN1, out + o);
			c++;
		}
	}
	out[o] = '\0';
}

static void put_fit(int row, int col, const char *u, int cols, int attr)
{
	char t[1200];

	if (cols <= 0)
		return;
	term_fit(u, t, sizeof t, cols);
	scr_put(row, col, t, (int)strlen(t), attr);
}

static void bar(int row, const char *u)
{
	scr_fill(row, 0, scr_cols, ' ', CA_REV);
	put_fit(row, 0, u, scr_cols, CA_REV | CA_BOLD);
}

static void status(const char *help)
{
	scr_fill(scr_rows - 1, 0, scr_cols, ' ', 0);
	put_fit(scr_rows - 1, 0, g_status[0] ? g_status : help, scr_cols, 0);
}

/* a line editor on the status line: 1 on Enter; mask: show stars */
static int prompt(const char *label, char *buf, size_t n, int mask)
{
	size_t len = strlen(buf);
	int lw = (int)strlen(label);

	for (;;) {
		int k, r = scr_rows - 1, start = 0;
		char shown[400];

		scr_fill(r, 0, scr_cols, ' ', 0);
		scr_put(r, 0, label, lw, CA_BOLD);
		if ((int)len > scr_cols - lw - 2)
			start = (int)len - (scr_cols - lw - 2);
		if (mask) {
			size_t i;

			for (i = 0; i < len - (size_t)start && i < sizeof shown - 1; i++)
				shown[i] = '*';
			shown[i] = '\0';
		} else
			snprintf(shown, sizeof shown, "%s", buf + start);
		scr_put(r, lw, shown, (int)strlen(shown), 0);
		scr_cursor(r, lw + (int)strlen(shown));
		scr_flush(0);
		k = scr_getkey(-1);
		if (k == '\r') {
			scr_cursor(-1, -1);
			return 1;
		}
		if (k == 27 || k == 3 || k == 7 || k == K_CLOSE) {
			scr_cursor(-1, -1);
			return 0;
		}
		if ((k == 8 || k == 127) && len > 0)
			buf[--len] = '\0';
		else if (k == 21)
			buf[len = 0] = '\0';
		else if (k >= 32 && k < 256 && k != 127 && len + 1 < n) {
			buf[len++] = (char)k;
			buf[len] = '\0';
		}
	}
}

/* a list to choose from (UTF-8 items): its index, or -1 */
static int menu(const char *title, char **items, int n, int cur)
{
	int top = 0;

	for (;;) {
		int rows = scr_rows - 2, i, k;

		if (cur < top)
			top = cur;
		if (cur >= top + rows)
			top = cur - rows + 1;
		scr_erase();
		bar(0, title);
		for (i = 0; i < rows && top + i < n; i++) {
			int a = top + i == cur ? CA_REV : 0;

			scr_fill(1 + i, 0, scr_cols, ' ', a);
			put_fit(1 + i, 2, items[top + i], scr_cols - 2, a);
		}
		say("", NULL);
		status("Enter choose  Esc back");
		scr_flush(0);
		k = scr_getkey(-1);
		switch (k) {
		case K_UP: case 'k':
			if (cur > 0)
				cur--;
			break;
		case K_DOWN: case 'j':
			if (cur + 1 < n)
				cur++;
			break;
		case K_PGUP:
			cur = cur > rows ? cur - rows : 0;
			break;
		case K_PGDN:
			cur = cur + rows < n ? cur + rows : n - 1;
			break;
		case '\r': case K_RIGHT:
			return cur;
		case 27: case 'q': case K_LEFT: case K_CLOSE:
			return -1;
		}
	}
}

/* --- the server -------------------------------------------------------- */

/* draw what's happening, before a wait on the network */
static void busy(const char *what)
{
	say("%s", what);
	status("");
	scr_flush(0);
}

static int connect_server(void)
{
	char what[300];

	snprintf(what, sizeof what, "Connecting to %s...", g_host);
	busy(what);
	imap_close(&g_m);
	if (imap_open(&g_m, g_host, g_port, g_tls) < 0)
		return -1;
	busy("Logging in...");
	if (imap_auth(&g_m, g_user, g_pass) < 0)
		return -1;
	return 0;
}

static int g_stale;			/* the list may be out of date */

/* connected again after the connection went (a server drops one left
 * idle half an hour or so), the mailbox selected again */
static int reconnect(void)
{
	unsigned long had = g_m.exists, uv = g_m.uidvalidity;

	if (connect_server() < 0 || imap_select(&g_m, g_box, 0) < 0)
		return -1;
	if (g_m.exists != had || g_m.uidvalidity != uv)
		g_stale = 1;
	return 0;
}

/* a command; if the connection had gone, connected again and the command
 * tried once more (they are all ones that can be: FETCH, STORE +FLAGS,
 * LIST, NOOP) */
static int cmd(imap_cb cb, void *ctx, const char *fmt, ...)
{
	va_list ap;
	int rc;

	if (g_m.c == NULL && reconnect() < 0)
		return -1;
	va_start(ap, fmt);
	rc = imap_vcmd(&g_m, cb, ctx, fmt, ap);
	va_end(ap);
	if (rc < 0 && g_m.c == NULL && reconnect() == 0) {
		va_start(ap, fmt);
		rc = imap_vcmd(&g_m, cb, ctx, fmt, ap);
		va_end(ap);
	}
	return rc;
}

/* days since 1970-01-01 of a date (month 1-12) */
static long days_from_civil(long y, int m, int d)
{
	long era, yoe, doy;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* "14-Oct-2026 08:00:00 -0600" (INTERNALDATE), in local time: its time
 * if today, its day if this year, else its year */
static void short_date(const char *d, char *out, size_t n)
{
	static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	int day, m, year, hh, mm, ss, zone;
	char ms[4], sign;
	time_t when, now = time(NULL);
	struct tm t, today, *tp;

	out[0] = '\0';
	if (sscanf(d, " %d-%3s-%d %d:%d:%d %c%4d", &day, ms, &year, &hh, &mm, &ss, &sign,
		&zone) != 8)
		return;
	for (m = 0; m < 12 && strncmp(mon + 3 * m, ms, 3) != 0; m++)
		;
	if (m == 12)
		return;
	when = (time_t)((days_from_civil(year, m + 1, day) * 24 + hh) * 3600L + mm * 60L + ss
		- (sign == '-' ? -1 : 1) * ((zone / 100) * 3600L + (zone % 100) * 60L));
	if ((tp = localtime(&now)) == NULL)
		return;
	today = *tp;
	if ((tp = localtime(&when)) == NULL)
		return;
	t = *tp;
	if (t.tm_year == today.tm_year && t.tm_yday == today.tm_yday)
		snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
	else if (t.tm_year == today.tm_year)
		snprintf(out, n, "%.3s %2d", mon + 3 * t.tm_mon, t.tm_mday);
	else
		snprintf(out, n, "%d", t.tm_year + 1900);
}

/* an address list's first: its name, else mailbox@host (UTF-8) */
static void first_addr(struct imap *m, struct ival *list, char *out, size_t n, int full)
{
	struct ival *a = iv_at(m, list, 0);
	char name[200], box[120], host[120], raw[200], t[460];

	out[0] = '\0';
	if (a == NULL)
		return;
	iv_str(iv_at(m, a, 0), raw, sizeof raw);
	mime_header(raw, strlen(raw), name, sizeof name);
	iv_str(iv_at(m, a, 2), box, sizeof box);
	iv_str(iv_at(m, a, 3), host, sizeof host);
	if (name[0] && full)
		snprintf(t, sizeof t, "%s <%s@%s>", name, box, host);
	else if (name[0])
		snprintf(t, sizeof t, "%s", name);
	else
		snprintf(t, sizeof t, "%s@%s", box, host);
	copy_to(out, n, t);
}

/* an ENVELOPE's address list into out (max): how many */
static int env_addrs(struct imap *m, struct ival *list, struct addr *out, int max)
{
	unsigned long i;
	int n = 0;

	for (i = 0; list && list->type == IV_LIST && i < list->n && n < max; i++) {
		struct ival *a = iv_at(m, list, i);
		char raw[200], box[120], host[120];

		iv_str(iv_at(m, a, 2), box, sizeof box);
		iv_str(iv_at(m, a, 3), host, sizeof host);
		if (!box[0] || !host[0])
			continue;		/* (a group's start or end) */
		iv_str(iv_at(m, a, 0), raw, sizeof raw);
		mime_header(raw, strlen(raw), out[n].name, sizeof out[n].name);
		snprintf(out[n].mbox, sizeof out[n].mbox, "%s@%s", box, host);
		n++;
	}
	return n;
}

/* addresses as a header has them: Ann <a@b>, "Doe, J" <c@d>, e@f */
static void addrs_text(const struct addr *a, int n, char *out, size_t on)
{
	struct sbuf b;
	int i;

	memset(&b, 0, sizeof b);
	for (i = 0; i < n; i++) {
		if (i)
			sb_str(&b, ", ");
		if (a[i].name[0] && strpbrk(a[i].name, "()<>[]:;@\\,.\"")) {
			const char *c;

			sb_str(&b, "\"");
			for (c = a[i].name; *c; c++) {
				if (*c == '"' || *c == '\\')
					sb_str(&b, "\\");
				sb_put(&b, c, 1);
			}
			sb_str(&b, "\" <");
		} else if (a[i].name[0]) {
			sb_str(&b, a[i].name);
			sb_str(&b, " <");
		}
		sb_str(&b, a[i].mbox);
		if (a[i].name[0])
			sb_str(&b, ">");
	}
	copy_to(out, on, b.s ? b.s : "");
	xfree(b.s);
}

static struct msg *msg_at_seq(unsigned long seq)
{
	int i;

	for (i = 0; i < g_n; i++)
		if (g_msgs[i].seq == seq)
			return &g_msgs[i];
	return NULL;
}

static void flags_of(struct imap *m, struct ival *fl, struct msg *mg)
{
	unsigned long i;

	mg->seen = mg->flagged = mg->answered = 0;
	for (i = 0; fl && i < fl->n; i++) {
		struct ival *f = iv_at(m, fl, i);

		if (iv_is(f, "\\Seen"))
			mg->seen = 1;
		else if (iv_is(f, "\\Flagged"))
			mg->flagged = 1;
		else if (iv_is(f, "\\Answered"))
			mg->answered = 1;
	}
}

/* * N FETCH (UID u FLAGS (...) INTERNALDATE "..." ENVELOPE (...)) */
static void on_list_fetch(void *ctx, struct imap *m, struct ival *v, unsigned long n)
{
	struct ival *what = iv_next(m, v), *items = iv_next(m, what), *k;
	struct msg *mg;
	unsigned long seq = iv_num(v);

	(void)ctx;
	if (n < 3 || !iv_is(what, "FETCH") || items == NULL || items->type != IV_LIST)
		return;
	if ((mg = msg_at_seq(seq)) == NULL) {
		if (g_n == g_cap) {
			int c = g_cap ? g_cap * 2 : 64;
			struct msg *q = xrealloc(g_msgs, (size_t)c * sizeof *q);

			if (q == NULL)
				return;
			g_msgs = q;
			g_cap = c;
		}
		mg = &g_msgs[g_n++];
		memset(mg, 0, sizeof *mg);
		mg->seq = seq;
	}
	for (k = iv_at(m, items, 0); k && iv_next(m, k); k = iv_next(m, iv_next(m, k))) {
		struct ival *val = iv_next(m, k);
		char raw[400];

		if (iv_is(k, "UID"))
			mg->uid = iv_num(val);
		else if (iv_is(k, "FLAGS"))
			flags_of(m, val, mg);
		else if (iv_is(k, "INTERNALDATE"))
			short_date(iv_str(val, raw, sizeof raw), mg->date, sizeof mg->date);
		else if (iv_is(k, "ENVELOPE")) {
			iv_str(iv_at(m, val, 1), raw, sizeof raw);
			mime_header(raw, strlen(raw), mg->subject, sizeof mg->subject);
			first_addr(m, iv_at(m, val, 2), mg->from, sizeof mg->from, 0);
		}
	}
}

static int by_seq_down(const void *a, const void *b)
{
	const struct msg *x = a, *y = b;

	return x->seq < y->seq ? 1 : x->seq > y->seq ? -1 : 0;
}

/* messages lo..hi (sequence numbers) into the list */
static int fetch_range(unsigned long lo, unsigned long hi)
{
	if (cmd(on_list_fetch, NULL,
		"FETCH %lu:%lu (UID FLAGS INTERNALDATE ENVELOPE)", lo, hi) < 0)
		return -1;
	qsort(g_msgs, (size_t)g_n, sizeof *g_msgs, by_seq_down);
	return 0;
}

/* the mailbox, from its newest messages */
static int open_box(const char *box)
{
	char what[400];

	snprintf(what, sizeof what, "Opening %s...", box);
	busy(what);
	if ((g_m.c == NULL && reconnect() < 0) || imap_select(&g_m, box, 0) < 0)
		return -1;
	if (box != g_box)
		snprintf(g_box, sizeof g_box, "%s", box);
	g_n = g_sel = g_top = 0;
	if (g_m.exists == 0)
		return 0;
	busy("Fetching the messages...");
	return fetch_range(g_m.exists > PAGE_MSGS ? g_m.exists - PAGE_MSGS + 1 : 1, g_m.exists);
}

/* older ones, when the list's end is reached */
static void fetch_older(void)
{
	unsigned long oldest = g_n ? g_msgs[g_n - 1].seq : 0;

	if (oldest <= 1)
		return;
	busy("Fetching older messages...");
	if (fetch_range(oldest > PAGE_MSGS ? oldest - PAGE_MSGS : 1, oldest - 1) < 0)
		say("%s", g_m.err);
	else
		say("", NULL);
}

/* --- a message --------------------------------------------------------- */

static char *g_body;			/* the fetched part (a copy) */
static size_t g_body_len;
static const char *g_want;		/* BODY[...] looked for */

static void on_struct(void *ctx, struct imap *m, struct ival *v, unsigned long n)
{
	struct ival *items = iv_next(m, iv_next(m, v)), *k;
	char line[400];

	(void)ctx;
	if (n < 3 || items == NULL || items->type != IV_LIST)
		return;
	for (k = iv_at(m, items, 0); k && iv_next(m, k); k = iv_next(m, iv_next(m, k))) {
		struct ival *val = iv_next(m, k);

		if (iv_is(k, "BODYSTRUCTURE"))
			g_nparts = mime_parts(m, val, g_parts, MAX_PARTS);
		else if (iv_is(k, "ENVELOPE")) {
			char raw[400], *c;

			g_orig.nfrom = env_addrs(m, iv_at(m, val, 2), g_orig.from, 4);
			g_orig.nreply_to = env_addrs(m, iv_at(m, val, 4), g_orig.reply_to, 4);
			g_orig.nto = env_addrs(m, iv_at(m, val, 5), g_orig.to, ORIG_ADDRS);
			g_orig.ncc = env_addrs(m, iv_at(m, val, 6), g_orig.cc, ORIG_ADDRS);
			iv_str(iv_at(m, val, 0), g_orig.date, sizeof g_orig.date);
			iv_str(iv_at(m, val, 1), raw, sizeof raw);
			mime_header(raw, strlen(raw), g_orig.subject, sizeof g_orig.subject);
			iv_str(iv_at(m, val, 9), g_orig.msgid, sizeof g_orig.msgid);
			for (c = g_orig.msgid; *c; c++)
				if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 127)
					*c = '_';	/* (it goes in a header) */
			for (c = g_orig.date; *c; c++)
				if ((unsigned char)*c < ' ')
					*c = ' ';
			g_nhdr = 0;
			addrs_text(g_orig.from, g_orig.nfrom, line, sizeof line);
			snprintf(g_hdr[g_nhdr++], sizeof g_hdr[0], "From: %s", line);
			addrs_text(g_orig.to, g_orig.nto, line, sizeof line);
			if (line[0])
				snprintf(g_hdr[g_nhdr++], sizeof g_hdr[0], "To: %s", line);
			addrs_text(g_orig.cc, g_orig.ncc, line, sizeof line);
			if (line[0])
				snprintf(g_hdr[g_nhdr++], sizeof g_hdr[0], "Cc: %s", line);
			snprintf(g_hdr[g_nhdr++], sizeof g_hdr[0], "Date: %s", g_orig.date);
			snprintf(g_hdr[g_nhdr++], sizeof g_hdr[0], "Subject: %s", g_orig.subject);
		}
	}
}

/* the BODY[section] of a FETCH: copied into g_body */
static void on_body(void *ctx, struct imap *m, struct ival *v, unsigned long n)
{
	struct ival *items = iv_next(m, iv_next(m, v)), *k;
	size_t wl = strlen(g_want);

	(void)ctx;
	if (n < 3 || items == NULL || items->type != IV_LIST)
		return;
	for (k = iv_at(m, items, 0); k && iv_next(m, k); k = iv_next(m, iv_next(m, k))) {
		struct ival *val = iv_next(m, k);

		if (k->type == IV_ATOM && k->len >= wl && memcmp(k->s, g_want, wl) == 0
			&& (val->type == IV_STR || val->type == IV_NIL)) {
			xfree(g_body);
			g_body_len = val->type == IV_STR ? val->len : 0;
			if ((g_body = xmalloc(g_body_len + 1)) != NULL) {
				memcpy(g_body, val->s, g_body_len);
				g_body[g_body_len] = '\0';
			}
		}
	}
}

/* part p of message uid, its transfer encoding undone (*len); NULL */
static unsigned char *fetch_part(unsigned long uid, const struct mpart *p, size_t *len)
{
	char want[48];
	unsigned char *d;

	snprintf(want, sizeof want, "BODY[%s]", p->section);
	g_want = want;
	xfree(g_body);
	g_body = NULL;
	if (cmd(on_body, NULL, "UID FETCH %lu (BODY.PEEK[%s])", uid, p->section) < 0
		|| g_body == NULL)
		return NULL;
	d = mime_decode(p, g_body, g_body_len, len);
	xfree(g_body);
	g_body = NULL;
	return d;
}

/* plain text as HTML: escaped, its lines kept, its URLs links */
static char *text_html(const char *u)
{
	struct sbuf b;
	const char *p = u;

	memset(&b, 0, sizeof b);
	sb_str(&b, "<div>");
	while (*p) {
		if ((strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0)
			&& (p == u || p[-1] == ' ' || p[-1] == '\n' || p[-1] == '(' || p[-1] == '<')) {
			const char *e = p;
			const char *q;

			while (*e && *e != ' ' && *e != '\n' && *e != '\r' && *e != '>'
				&& *e != '"' && *e != '<')
				e++;
			while (e > p && (e[-1] == '.' || e[-1] == ',' || e[-1] == ')'))
				e--;
			sb_str(&b, "<a href=\"");
			for (q = p; q < e; q++)
				if (*q == '&')
					sb_str(&b, "&amp;");
				else
					sb_put(&b, q, 1);
			sb_str(&b, "\">");
			for (q = p; q < e; q++)
				if (*q == '&')
					sb_str(&b, "&amp;");
				else
					sb_put(&b, q, 1);
			sb_str(&b, "</a>");
			p = e;
			continue;
		}
		switch (*p) {
		case '&':
			sb_str(&b, "&amp;");
			break;
		case '<':
			sb_str(&b, "&lt;");
			break;
		case '>':
			sb_str(&b, "&gt;");
			break;
		case '\r':
			break;
		case '\n':
			sb_str(&b, "<br>\n");
			break;
		default:
			sb_put(&b, p, 1);
		}
		p++;
	}
	sb_str(&b, "</div>");
	return b.s;
}

/* g_doc laid out at the screen's width */
static void relayout(void)
{
	if (g_have_page) {
		layout_free(&g_page);
		g_have_page = 0;
	}
	if (layout_run(&g_page, &g_doc, scr_cols, scr_cs, 0, 0, NULL) == 0)
		g_have_page = 1;
}

static void show_html(const char *html)
{
	doc_free(&g_doc);
	doc_init(&g_doc, 0);
	html_load_begin(&g_load, &g_doc, "utf-8", 0);
	html_load_feed(&g_load, (const unsigned char *)html, strlen(html));
	html_load_end(&g_load);
	relayout();
}

/* the message's text, fetched and laid out */
static void load_message(struct msg *mg)
{
	int k, i;
	size_t len;
	unsigned char *d;
	char *u, *html;

	g_nparts = 0;
	g_nhdr = 0;
	g_vtop = 0;
	g_link = -1;
	memset(&g_orig, 0, sizeof g_orig);
	g_orig.uid = mg->uid;
	busy("Fetching the message...");
	if (cmd(on_struct, NULL, "UID FETCH %lu (BODYSTRUCTURE ENVELOPE)", mg->uid) < 0) {
		say("%s", g_m.err);
		show_html("<p>(the message couldn't be fetched)</p>");
		return;
	}
	/* the attachments, in the header lines */
	for (i = 0; i < g_nparts && g_nhdr < 8; i++)
		if (g_parts[i].attachment) {
			char *h = g_hdr[g_nhdr++];
			int a, n = 0;

			for (a = 0; a <= i; a++)
				n += g_parts[a].attachment;
			snprintf(h, sizeof g_hdr[0], "Attached [%d]: %s (%s/%s, %lu KB)", n,
				g_parts[i].name[0] ? g_parts[i].name : "(no name)",
				g_parts[i].type, g_parts[i].subtype,
				(g_parts[i].size * 3 / 4 + 1023) / 1024);
		}
	k = mime_pick(g_parts, g_nparts, config_bool("mail_html", 1));
	if (k < 0) {
		show_html("<p>(no text in this message)</p>");
		return;
	}
	if ((d = fetch_part(mg->uid, &g_parts[k], &len)) == NULL) {
		say("%s", g_m.err[0] ? g_m.err : "out of memory");
		show_html("<p>(the message's text couldn't be fetched)</p>");
		return;
	}
	u = mime_utf8(g_parts[k].charset[0] ? g_parts[k].charset : "us-ascii", d, len);
	xfree(d);
	if (u == NULL)
		return;
	html = strcmp(g_parts[k].subtype, "html") == 0 ? u : text_html(u);
	show_html(html ? html : "");
	if (html != u)
		xfree(html);
	xfree(u);
	if (!mg->seen && cmd(NULL, NULL, "UID STORE %lu +FLAGS.SILENT (\\Seen)",
		mg->uid) == 0)
		mg->seen = 1;
	say("", NULL);
}

/* --- writing ------------------------------------------------------------ */

enum { W_NEW, W_REPLY, W_REPLY_ALL, W_FORWARD };

static char g_from[300];		/* "Name <a@b>" or a@b: mail_from */
static char g_from_mbox[ADDR_MAX];	/* its a@b */
static char g_smtp[256];		/* mail_smtp's host */
static unsigned g_smtp_port = 465;
static char g_sent[300];		/* mail_sent: where a copy goes, "" */

static int lower_c(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int same_mbox(const char *a, const char *b)
{
	while (*a && lower_c((unsigned char)*a) == lower_c((unsigned char)*b))
		a++, b++;
	return *a == '\0' && *b == '\0';
}

/* does s begin with w, case aside? */
static int ci_begins(const char *s, const char *w)
{
	while (*w)
		if (lower_c((unsigned char)*s++) != lower_c((unsigned char)*w++))
			return 0;
	return 1;
}

static int is_me(const char *mbox)
{
	return same_mbox(mbox, g_from_mbox) || same_mbox(mbox, g_user);
}

/* the open message's text as it would be quoted: each line after prefix,
 * width columns at most, in UTF-8; no blank lines at either end */
static void page_text(struct sbuf *b, int width, const char *prefix)
{
	struct page p;
	unsigned long i;
	long first = -1, last = -1;
	size_t pl = strlen(prefix);

	memset(&p, 0, sizeof p);
	if (layout_run(&p, &g_doc, width, TCS_UTF8, 0, 0, NULL) < 0)
		return;
	for (i = 0; i < p.nlines; i++)
		if (p.lines[i].len > 0) {
			if (first < 0)
				first = (long)i;
			last = (long)i;
		}
	for (i = (unsigned long)(first < 0 ? 0 : first); first >= 0 && (long)i <= last; i++) {
		const struct lline *l = &p.lines[i];
		int k;

		if (l->len == 0) {
			/* (an empty line's prefix without its trailing space) */
			size_t n = pl;

			while (n > 0 && prefix[n - 1] == ' ')
				n--;
			sb_put(b, prefix, n);
		} else {
			sb_str(b, prefix);
			for (k = 0; k < l->indent; k++)
				sb_str(b, " ");
			sb_put(b, p.text + l->off, l->len);
		}
		sb_str(b, "\n");
	}
	layout_free(&p);
}

/* the open message's References with its own Message-ID after, for a
 * reply's (the newest kept when there are too many) */
static void references(char *out, size_t n)
{
	char want[] = "BODY[HEADER.FIELDS";
	struct sbuf b;
	const char *p;

	memset(&b, 0, sizeof b);
	g_want = want;
	xfree(g_body);
	g_body = NULL;
	if (cmd(on_body, NULL, "UID FETCH %lu (BODY.PEEK[HEADER.FIELDS (REFERENCES)])",
		g_orig.uid) == 0 && g_body != NULL && (p = strchr(g_body, ':')) != NULL) {
		/* unfolded, one space between ids, nothing but printable ASCII */
		for (p++; *p; p++) {
			if ((unsigned char)*p > ' ' && (unsigned char)*p < 127)
				sb_put(&b, p, 1);
			else if (b.len && b.s[b.len - 1] != ' ')
				sb_str(&b, " ");
		}
	}
	xfree(g_body);
	g_body = NULL;
	if (b.len && b.s[b.len - 1] != ' ')
		sb_str(&b, " ");
	sb_str(&b, g_orig.msgid);
	p = b.s ? b.s : "";
	while (strlen(p) + 1 > n && strchr(p + 1, '<'))
		p = strchr(p + 1, '<');
	copy_to(out, n, p);
	xfree(b.s);
}

/* the draft's first text: headers to fill in, what's replied to */
static void template(int kind, struct sbuf *b)
{
	char to[1600] = "", cc[1600] = "", subj[400], path[600], refs[1000];
	static struct addr list[2 * ORIG_ADDRS];
	unsigned char *sig;
	size_t siglen;

	subj[0] = '\0';
	if (kind == W_REPLY || kind == W_REPLY_ALL) {
		const struct addr *r = g_orig.nreply_to ? g_orig.reply_to : g_orig.from;
		int nr = g_orig.nreply_to ? g_orig.nreply_to : g_orig.nfrom, n = 0, i, j;

		addrs_text(r, nr, to, sizeof to);
		for (i = 0; kind == W_REPLY_ALL && i < g_orig.nto + g_orig.ncc; i++) {
			const struct addr *a = i < g_orig.nto ? &g_orig.to[i] : &g_orig.cc[i - g_orig.nto];

			if (is_me(a->mbox))
				continue;
			for (j = 0; j < nr && !same_mbox(r[j].mbox, a->mbox); j++)
				;
			if (j < nr)
				continue;
			for (j = 0; j < n && !same_mbox(list[j].mbox, a->mbox); j++)
				;
			if (j == n)
				list[n++] = *a;
		}
		addrs_text(list, n, cc, sizeof cc);
		snprintf(subj, sizeof subj, "%s%s", ci_begins(g_orig.subject, "Re:") ? "" : "Re: ",
			g_orig.subject);
	} else if (kind == W_FORWARD)
		snprintf(subj, sizeof subj, "%s%s", ci_begins(g_orig.subject, "Fwd:") ? "" : "Fwd: ",
			g_orig.subject);
	sb_str(b, "To: ");
	sb_str(b, to);
	sb_str(b, "\nCc: ");
	sb_str(b, cc);
	sb_str(b, "\nBcc: \nSubject: ");
	sb_str(b, subj);
	sb_str(b, "\n");
	if ((kind == W_REPLY || kind == W_REPLY_ALL) && g_orig.msgid[0]) {
		references(refs, sizeof refs);
		sb_str(b, "In-Reply-To: ");
		sb_str(b, g_orig.msgid);
		sb_str(b, "\nReferences: ");
		sb_str(b, refs);
		sb_str(b, "\n");
	}
	sb_str(b, "\n");
	if (kind == W_REPLY || kind == W_REPLY_ALL) {
		char who[300];

		addrs_text(g_orig.from, g_orig.nfrom > 0 ? 1 : 0, who, sizeof who);
		sb_str(b, "\n\nOn ");
		sb_str(b, g_orig.date);
		sb_str(b, ", ");
		sb_str(b, g_orig.from[0].name[0] ? g_orig.from[0].name : who);
		sb_str(b, " wrote:\n");
		page_text(b, 72, "> ");
	} else if (kind == W_FORWARD) {
		char line[1600];

		sb_str(b, "\n\n---------- Forwarded message ----------\nFrom: ");
		addrs_text(g_orig.from, g_orig.nfrom, line, sizeof line);
		sb_str(b, line);
		sb_str(b, "\nDate: ");
		sb_str(b, g_orig.date);
		sb_str(b, "\nSubject: ");
		sb_str(b, g_orig.subject);
		sb_str(b, "\nTo: ");
		addrs_text(g_orig.to, g_orig.nto, line, sizeof line);
		sb_str(b, line);
		sb_str(b, "\n\n");
		page_text(b, 72, "");
	} else
		sb_str(b, "\n");
	/* ~/.manx/signature, after "-- " */
	if ((sig = os_read_file(os_datapath(path, sizeof path, "signature"), &siglen)) != NULL) {
		sb_str(b, "\n-- \n");
		sb_put(b, (const char *)sig, siglen);
		if (siglen && sig[siglen - 1] != '\n')
			sb_str(b, "\n");
		xfree(sig);
	}
}

/* is there a program called name in a directory of $PATH? */
static int on_path(const char *name)
{
	const char *p = getenv("PATH");
	char f[1024];

	if (p == NULL)
		return 0;
	for (;;) {
		size_t n = strcspn(p, ":");

		/* (an empty entry is the current directory) */
		snprintf(f, sizeof f, "%.*s/%s", n ? (int)n : 1, n ? p : ".", name);
		if (access(f, X_OK) == 0)
			return 1;
		if (p[n] == '\0')
			return 0;
		p += n + 1;
	}
}

/* is the editor command ed vim, whatever directory and options? */
static int is_vim(const char *ed)
{
	const char *w, *e, *b;

	for (w = ed; *w == ' '; w++)
		;
	for (e = w; *e && *e != ' '; e++)
		;
	for (b = e; b > w && b[-1] != '/'; b--)
		;
	return (e - b == 3 && strncmp(b, "vim", 3) == 0)
		|| (e - b == 4 && (strncmp(b, "nvim", 4) == 0 || strncmp(b, "gvim", 4) == 0));
}

/* $VISUAL or $EDITOR on path (vim if it is on $PATH, else vi), the
 * terminal handed over meanwhile. vim is told the file is mail, which it
 * can't tell from the name: lines wrap at 72, quotes are coloured. */
static int edit_file(const char *path)
{
	const char *ed = getenv("VISUAL");
	char line[600];
	pid_t pid;
	int st = 0;

	if (ed == NULL || *ed == '\0')
		ed = getenv("EDITOR");
	if (ed == NULL || *ed == '\0')
		ed = on_path("vim") ? "vim" : "vi";
	/* (through the shell: an editor with options, "emacs -nw") */
	snprintf(line, sizeof line, "%s %s\"$1\"", ed, is_vim(ed) ? "'+setf mail' " : "");
	scr_suspend();
	if ((pid = fork()) == 0) {
		execl("/bin/sh", "sh", "-c", line, "sh", path, (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, &st, 0) < 0)
			;
	scr_resume();
	return pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) != 127 ? 0 : -1;
}

struct draft {
	char to[2000], cc[2000], bcc[2000], subject[600], irt[300], refs[1000];
	const char *body;
};

/* a draft's text (changed in place: CRs out, headers unfolded) into d: 0,
 * or -1 with why in err */
static int parse_draft(char *text, struct draft *d, char *err, size_t en)
{
	char *p = text, *o = text;

	memset(d, 0, sizeof *d);
	for (; *p; p++)
		if (*p != '\r')
			*o++ = *p;
	*o = '\0';
	p = text;
	while (*p && *p != '\n') {
		char *e = p, *colon, *v, name[24];
		size_t k;

		/* the header, its following lines that begin with a space on it */
		for (;;) {
			e = strchr(e, '\n');
			if (e == NULL) {
				e = p + strlen(p);
				break;
			}
			if (e[1] == ' ' || e[1] == '\t') {
				*e = ' ';
				continue;
			}
			break;
		}
		colon = memchr(p, ':', (size_t)(e - p));
		if (colon == NULL || colon - p >= (long)sizeof name) {
			snprintf(err, en, "\"%.40s\" isn't a header: the text goes after an empty line",
				p);
			return -1;
		}
		for (k = 0; p + k < colon; k++)
			name[k] = (char)lower_c((unsigned char)p[k]);
		name[k] = '\0';
		for (v = colon + 1; v < e && (*v == ' ' || *v == '\t'); v++)
			;
		k = (size_t)(e - v);
		while (k > 0 && (v[k - 1] == ' ' || v[k - 1] == '\t'))
			k--;
#define TAKE(f) snprintf(d->f, sizeof d->f, "%.*s", (int)k, v)
		if (strcmp(name, "to") == 0)
			TAKE(to);
		else if (strcmp(name, "cc") == 0)
			TAKE(cc);
		else if (strcmp(name, "bcc") == 0)
			TAKE(bcc);
		else if (strcmp(name, "subject") == 0)
			TAKE(subject);
		else if (strcmp(name, "in-reply-to") == 0)
			TAKE(irt);
		else if (strcmp(name, "references") == 0)
			TAKE(refs);
		else {
			snprintf(err, en, "manxmail doesn't send a \"%s:\" header (a typo?)", name);
			return -1;
		}
#undef TAKE
		p = *e ? e + 1 : e;
	}
	d->body = *p == '\n' ? p + 1 : p;
	return 0;
}

/* everyone it goes to (To, Cc, Bcc; each once) into out: how many, or -1
 * with err */
static int recipients(const struct draft *d, struct addr *out, int max, char *err, size_t en)
{
	const char *f[3];
	struct addr a[64];
	char bad[120];
	int n = 0, i, j, k;

	f[0] = d->to;
	f[1] = d->cc;
	f[2] = d->bcc;
	for (i = 0; i < 3; i++) {
		if ((k = addr_parse(f[i], a, 64, bad, sizeof bad)) < 0) {
			snprintf(err, en, "\"%s\" isn't an address", bad);
			return -1;
		}
		for (j = 0; j < k; j++) {
			int x;

			for (x = 0; x < n && !same_mbox(out[x].mbox, a[j].mbox); x++)
				;
			if (x == n && n < max)
				out[n++] = a[j];
		}
	}
	if (n == 0)
		snprintf(err, en, "it isn't to anyone (To:)");
	return n;
}

/* "<random.time@the-from-address's-domain>" */
static void make_msgid(char *out, size_t n)
{
	unsigned char r[32];
	char hex[25];
	const char *at = strchr(g_from_mbox, '@');
	int i;

	if (entropy_extract(r) < 0) {
		unsigned long x = os_usec() ^ (unsigned long)getpid() << 16;

		for (i = 0; i < 12; i++, x = x * 1103515245UL + 12345)
			r[i] = (unsigned char)(x >> 16);
	}
	for (i = 0; i < 12; i++)
		sprintf(hex + 2 * i, "%02x", r[i]);
	snprintf(out, n, "<%s.%lx@%s>", hex, (unsigned long)time(NULL),
		at ? at + 1 : "manxmail.invalid");
}

/* send it: 0, or -1 with err */
static int send_draft(const struct draft *d, char *err, size_t en)
{
	static struct addr to[192];
	static struct smtp s;
	char *rcpt[192], date[40], msgid[160];
	struct outmsg o;
	char *msg;
	size_t len;
	int n, i;

	if ((n = recipients(d, to, 192, err, en)) <= 0)
		return -1;
	for (i = 0; i < n; i++)
		rcpt[i] = to[i].mbox;
	mail_date((long)time(NULL), date);
	make_msgid(msgid, sizeof msgid);
	memset(&o, 0, sizeof o);
	o.from = g_from;
	o.to = d->to;
	o.cc = d->cc;
	o.subject = d->subject;
	o.date = date;
	o.msgid = msgid;
	o.in_reply_to = d->irt;
	o.references = d->refs;
	o.body = d->body;
	if ((msg = mail_build(&o, &len)) == NULL) {
		snprintf(err, en, "out of memory");
		return -1;
	}
	if (!g_smtp[0]) {
		snprintf(err, en, "set mail_smtp in ~/.manx/config: the server to send through");
		xfree(msg);
		return -1;
	}
	snprintf(err, en, "Sending to %s...", g_smtp);
	busy(err);
	if (smtp_open(&s, g_smtp, g_smtp_port, g_tls) < 0 || smtp_auth(&s, g_user, g_pass) < 0
		|| smtp_send(&s, g_from_mbox, rcpt, n, msg, len) < 0) {
		snprintf(err, en, "%s", s.err);
		smtp_close(&s);
		xfree(msg);
		return -1;
	}
	smtp_close(&s);
	err[0] = '\0';
	if (g_sent[0]) {
		char what[400];

		snprintf(what, sizeof what, "Filing a copy in %s...", g_sent);
		busy(what);
		if ((g_m.c == NULL && reconnect() < 0)
			|| imap_append(&g_m, g_sent, "\\Seen", msg, len) < 0)
			snprintf(err, en, "Sent, but no copy filed in %.40s: %.150s", g_sent, g_m.err);
	}
	xfree(msg);
	return 0;
}

/* what is about to go: its headers, its first lines; the choice */
static int confirm(const struct draft *d, const char *err)
{
	const char *h[4], *v[4], *p;
	int i, r = 1;

	scr_erase();
	bar(0, "Send this message?");
	h[0] = "To:";
	v[0] = d->to;
	h[1] = "Cc:";
	v[1] = d->cc;
	h[2] = "Bcc:";
	v[2] = d->bcc;
	h[3] = "Subject:";
	v[3] = d->subject;
	for (i = 0; i < 4; i++)
		if (v[i][0] || i == 0 || i == 3) {
			put_fit(r, 0, h[i], 9, CA_BOLD);
			put_fit(r++, 9, v[i][0] ? v[i] : "(none)", scr_cols - 9, 0);
		}
	r++;
	for (p = d->body; *p && r < scr_rows - 3; r++) {
		const char *e = strchr(p, '\n');
		char line[600];
		size_t k = e ? (size_t)(e - p) : strlen(p);

		snprintf(line, sizeof line, "%.*s", (int)(k < sizeof line ? k : sizeof line - 1), p);
		put_fit(r, 0, line, scr_cols, 0);
		p = e ? e + 1 : p + k;
	}
	if (err && err[0])
		put_fit(scr_rows - 2, 0, err, scr_cols, CA_BOLD);
	say("", NULL);
	status("y send   e edit again   n not now (it is kept, for next time)");
	scr_flush(0);
	for (;;) {
		int k = scr_getkey(-1);

		if (k == 'y' || k == 'Y')
			return 'y';
		if (k == 'e' || k == 'E')
			return 'e';
		if (k == 'n' || k == 'N' || k == 27 || k == 'q' || k == K_CLOSE)
			return 'n';
	}
}

/* write a message (kind: W_*, to the open one) and send it */
static void compose(int kind)
{
	char path[600], err[300], *text = NULL, *first = NULL;
	struct sbuf t;
	static struct draft d;
	unsigned long reply_uid = 0;
	long sz, mt;
	size_t len;

	if (!g_from_mbox[0]) {
		say("Set mail_from in ~/.manx/config: the address to send from.", NULL);
		return;
	}
	os_datapath(path, sizeof path, "draft");
	if (os_file_info(path, &sz, &mt) == 0) {
		char yn[4] = "";

		if (!prompt("A message from before wasn't sent. Go on with it (y) or start anew (n)? ",
			yn, sizeof yn, 0))
			return;
		if (yn[0] == 'y' || yn[0] == 'Y')
			kind = -1;
	}
	if (kind >= 0) {
		memset(&t, 0, sizeof t);
		if (kind != W_NEW)
			busy("Getting the message ready...");
		template(kind, &t);
		if (t.oom || os_write_file(path, t.s ? t.s : "", t.len, 0600) < 0) {
			say("Couldn't write the draft, %.300s.", path);
			xfree(t.s);
			return;
		}
		first = t.s;
		if (kind == W_REPLY || kind == W_REPLY_ALL)
			reply_uid = g_orig.uid;
	}
	err[0] = '\0';
	for (;;) {
		int c, bad;

		if (edit_file(path) < 0) {
			say("Couldn't run the editor ($EDITOR, else vim or vi).", NULL);
			break;
		}
		xfree(text);
		if ((text = (char *)os_read_file(path, &len)) == NULL) {
			say("The draft is gone (%.300s).", path);
			break;
		}
		if (first && strcmp(text, first) == 0) {
			unlink(path);
			say("Not sent: nothing was written.", NULL);
			break;
		}
		bad = 1;
		if (parse_draft(text, &d, err, sizeof err) == 0) {
			static struct addr a[192];

			if (recipients(&d, a, 192, err, sizeof err) > 0) {
				err[0] = '\0';
				bad = 0;
			}
		}
		for (;;) {
			c = confirm(&d, err);
			if (c != 'y')
				break;
			if (bad || send_draft(&d, err, sizeof err) < 0)
				continue;	/* (err says why: the choice again) */
			/* sent */
			unlink(path);
			if (reply_uid && cmd(NULL, NULL, "UID STORE %lu +FLAGS.SILENT (\\Answered)",
				reply_uid) == 0) {
				int i;

				for (i = 0; i < g_n; i++)
					if (g_msgs[i].uid == reply_uid)
						g_msgs[i].answered = 1;
			}
			say(err[0] ? "%s" : "Sent.", err);
			xfree(text);
			xfree(first);
			return;
		}
		if (c == 'n') {
			say("Not sent: it is kept in ~/.manx/draft, for next time.", NULL);
			break;
		}
		/* (e: edited again) */
	}
	xfree(text);
	xfree(first);
}

/* --- drawing a message ------------------------------------------------- */

static int cell_attr(int sa, int link, long k)
{
	int a = 0;

	if (sa & SA_BOLD)
		a |= CA_BOLD;
	if (sa & (SA_UNDER | SA_FIELD))
		a |= CA_UNDER;
	if (link) {
		a |= CA_LINK;
		if (link - 1 == k)
			a |= CA_REV;
	}
	return a;
}

static void draw_page_line(int row, unsigned long ln)
{
	const struct page *p = &g_page;
	const struct lline *l = &p->lines[ln];
	unsigned long off = l->off, end = l->off + l->len, s = l->span;
	int col = l->indent;

	while (off < end && col < scr_cols) {
		unsigned long next = end;
		const struct lspan *sp;

		s = layout_span_at(p, s, off);
		sp = &p->spans[s];
		if (s + 1 < p->nspans && p->spans[s + 1].off < end)
			next = p->spans[s + 1].off;
		col += scr_put(row, col, p->text + off, (int)(next - off),
			cell_attr(sp->attr, sp->link, g_link) | CA_FG(sp->color));
		off = next;
	}
}

/* the view's rows: the header lines, a blank one, the page's lines */
static long view_rows(void)
{
	return g_nhdr + 1 + (g_have_page ? (long)g_page.nlines : 0);
}

static void draw_message(const struct msg *mg)
{
	int rows = scr_rows - 2, i;

	scr_erase();
	scr_palette(g_page.palette, g_have_page ? g_page.npalette : 0);
	bar(0, mg->subject[0] ? mg->subject : "(no subject)");
	for (i = 0; i < rows; i++) {
		long v = g_vtop + i;

		if (v < g_nhdr) {
			const char *h = g_hdr[v], *c = strchr(h, ':');
			int lw = c ? (int)(c - h) + 1 : 0;

			put_fit(1 + i, 0, h, lw, CA_BOLD);
			put_fit(1 + i, lw, c ? c + 1 : h, scr_cols - lw, 0);
		} else if (v > g_nhdr && g_have_page && v - g_nhdr - 1 < (long)g_page.nlines)
			draw_page_line(1 + i, (unsigned long)(v - g_nhdr - 1));
	}
	status("Space more  u list  n/p next/prev  r reply  a all  f fwd  m new  Tab link  s save");
	scr_flush(0);
}

/* the next (dir 1) or previous link on screen, else scrolling to one */
static void next_link(int dir)
{
	long rows = scr_rows - 2, k = g_link;

	if (!g_have_page || g_page.nlinks == 0)
		return;
	for (k += dir; k >= 0 && k < (long)g_page.nlinks; k += dir) {
		long row = (long)g_page.links[k].line + g_nhdr + 1;

		if (row >= g_vtop && row < g_vtop + rows) {
			g_link = k;
			return;
		}
		if ((dir > 0 && row >= g_vtop + rows) || (dir < 0 && row < g_vtop)) {
			g_link = k;
			g_vtop = row - rows / 2;
			if (g_vtop < 0)
				g_vtop = 0;
			return;
		}
	}
}

/* a link: in manx, the terminal handed over to it meanwhile */
static void open_link(void)
{
	const char *href;
	pid_t pid;
	int st = 0;

	if (g_link < 0 || !g_have_page || g_link >= (long)g_page.nlinks)
		return;
	href = doc_attr(&g_doc, g_page.links[g_link].node, ATTR_HREF);
	if (href == NULL || (strncmp(href, "http://", 7) && strncmp(href, "https://", 8)
		&& strncmp(href, "gopher://", 9))) {
		say("Only web links open (this one: %s).", href ? href : "none");
		return;
	}
	scr_suspend();
	if ((pid = fork()) == 0) {
		/* (the manx beside this program first: a build tree's) */
		if (g_manx[0])
			execl(g_manx, "manx", href, (char *)NULL);
		execlp("manx", "manx", href, (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, &st, 0) < 0)
			;
	scr_resume();
	say(pid > 0 && !(WIFEXITED(st) && WEXITSTATUS(st) == 127) ? "" : "Couldn't start manx.",
		NULL);
}

/* the n-th attachment, saved where the user says */
static void save_attachment(const struct msg *mg)
{
	int n = 0, i, want = 1, k = -1;
	char num[8] = "", name[300];
	size_t len;
	unsigned char *d;
	long sz, mt;

	for (i = 0; i < g_nparts; i++)
		n += g_parts[i].attachment;
	if (n == 0) {
		say("Nothing is attached.", NULL);
		return;
	}
	if (n > 1) {
		if (!prompt("Save which (1-9)? ", num, sizeof num, 0) || (want = atoi(num)) < 1 || want > n)
			return;
	}
	for (i = 0; i < g_nparts && k < 0; i++)
		if (g_parts[i].attachment && --want == 0)
			k = i;
	/* (its own name, but no directories in it) */
	{
		const char *b = strrchr(g_parts[k].name, '/');

		snprintf(name, sizeof name, "%s", b ? b + 1 : g_parts[k].name[0] ? g_parts[k].name
			: "attachment");
		if (name[0] == '.')
			name[0] = '_';
	}
	if (!prompt("Save as: ", name, sizeof name, 0) || !name[0])
		return;
	if (os_file_info(name, &sz, &mt) == 0) {
		char yn[4] = "";

		if (!prompt("It exists. Replace it (y/n)? ", yn, sizeof yn, 0) || yn[0] != 'y')
			return;
	}
	busy("Fetching the attachment...");
	if ((d = fetch_part(mg->uid, &g_parts[k], &len)) == NULL) {
		say("%s", g_m.err[0] ? g_m.err : "out of memory");
		return;
	}
	if (os_write_file(name, d, len, 0644) < 0)
		say("Couldn't write %s.", name);
	else {
		char t[400];

		snprintf(t, sizeof t, "Saved %s (%lu KB).", name, (unsigned long)(len + 1023) / 1024);
		say("%s", t);
	}
	xfree(d);
}

/* message g_sel (n and p move it), until back to the list or quitting */
static void read_message(void)
{
	load_message(&g_msgs[g_sel]);
	for (;;) {
		long rows = scr_rows - 2, max = view_rows() - rows;
		int k;

		if (max < 0)
			max = 0;
		if (g_vtop > max)
			g_vtop = max;
		if (g_vtop < 0)
			g_vtop = 0;
		if (scr_check_size()) {
			relayout();
			continue;
		}
		draw_message(&g_msgs[g_sel]);
		k = scr_getkey(-1);
		say("", NULL);
		switch (k) {
		case ' ': case K_PGDN:
			g_vtop += rows - 2;
			break;
		case 'b': case K_PGUP:
			g_vtop -= rows - 2;
			break;
		case 'j': case K_DOWN:
			g_vtop++;
			break;
		case 'k': case K_UP:
			g_vtop--;
			break;
		case K_HOME:
			g_vtop = 0;
			break;
		case K_END:
			g_vtop = max;
			break;
		case '\t':
			next_link(1);
			break;
		case K_BTAB:
			next_link(-1);
			break;
		case '\r':
			open_link();
			break;
		case 's':
			save_attachment(&g_msgs[g_sel]);
			break;
		case 'm':
			compose(W_NEW);
			break;
		case 'r':
			compose(W_REPLY);
			break;
		case 'a':
			compose(W_REPLY_ALL);
			break;
		case 'f':
			compose(W_FORWARD);
			break;
		case 'n':
			if (g_sel + 1 < g_n || (fetch_older(), g_sel + 1 < g_n)) {
				g_sel++;
				load_message(&g_msgs[g_sel]);
			}
			break;
		case 'p':
			if (g_sel > 0) {
				g_sel--;
				load_message(&g_msgs[g_sel]);
			}
			break;
		case 'u': case K_LEFT: case 8: case 127: case 'i':
			return;
		case 'q': case K_CLOSE:
			g_quit = 1;
			return;
		}
	}
}

/* --- the list ------------------------------------------------------------ */

static void draw_list(void)
{
	int rows = scr_rows - 2, i, fw = scr_cols / 4;
	char t[700], name[300];
	unsigned long unseen = 0;

	if (fw < 12)
		fw = 12;
	if (fw > 24)
		fw = 24;
	for (i = 0; i < g_n; i++)
		unseen += !g_msgs[i].seen;
	scr_erase();
	mime_mutf7(g_box, name, sizeof name);
	snprintf(t, sizeof t, "manxmail  %s  %s  %lu message%s, %lu not read%s", g_user, name,
		g_m.exists, g_m.exists == 1 ? "" : "s", unseen,
		(unsigned long)g_n < g_m.exists ? " (of those listed)" : "");
	bar(0, t);
	if (g_sel < g_top)
		g_top = g_sel;
	if (g_sel >= g_top + rows)
		g_top = g_sel - rows + 1;
	for (i = 0; i < rows && g_top + i < g_n; i++) {
		const struct msg *mg = &g_msgs[g_top + i];
		int r = 1 + i, a = g_top + i == g_sel ? CA_REV : 0;

		scr_fill(r, 0, scr_cols, ' ', a);
		scr_put(r, 0, mg->flagged ? "!" : !mg->seen ? "N" : mg->answered ? "r" : " ", 1,
			a | CA_BOLD);
		put_fit(r, 2, mg->date, 6, a);
		put_fit(r, 9, mg->from[0] ? mg->from : "?", fw, a | (mg->seen ? 0 : CA_BOLD));
		put_fit(r, 10 + fw, mg->subject[0] ? mg->subject : "(no subject)",
			scr_cols - 10 - fw, a | (mg->seen ? 0 : CA_BOLD));
	}
	if (g_n == 0)
		put_fit(2, 2, "(no messages)", scr_cols - 2, 0);
	status("Enter read  m new  r reply  a all  f forward  c folders  g get mail  q quit");
	scr_flush(0);
}

static char **g_names;
static int g_nnames;

static void on_list(void *ctx, struct imap *m, struct ival *v, unsigned long n)
{
	struct ival *name;
	char raw[300];
	char **q;
	int i;

	(void)ctx;
	if (n < 4 || !iv_is(v, "LIST"))
		return;
	name = iv_next(m, iv_next(m, iv_next(m, v)));	/* (flags, delimiter) */
	if (name == NULL)
		return;
	iv_str(name, raw, sizeof raw);
	for (i = 0; i < g_nnames; i++)
		if (strcmp(g_names[i], raw) == 0)
			return;		/* (a LIST tried again) */
	if ((q = xrealloc(g_names, (size_t)(g_nnames + 1) * sizeof *q)) == NULL)
		return;
	g_names = q;
	g_names[g_nnames++] = xstrdup(raw);
}

static void choose_folder(void)
{
	char **shown;
	int i, k;

	busy("Fetching the folders...");
	for (i = 0; i < g_nnames; i++)
		xfree(g_names[i]);
	g_nnames = 0;
	if (cmd(on_list, NULL, "LIST \"\" \"*\"") < 0 || g_nnames == 0) {
		say("%s", g_m.err[0] ? g_m.err : "No folders.");
		return;
	}
	if ((shown = xmalloc((size_t)g_nnames * sizeof *shown)) == NULL)
		return;
	for (i = 0; i < g_nnames; i++) {
		char u[300];

		mime_mutf7(g_names[i], u, sizeof u);
		shown[i] = xstrdup(u);
	}
	k = menu("Folders", shown, g_nnames, 0);
	for (i = 0; i < g_nnames; i++)
		xfree(shown[i]);
	xfree(shown);
	if (k >= 0 && open_box(g_names[k]) < 0)
		say("%s", g_m.err);
	else
		say("", NULL);
}

/* the list again from the server, the same message chosen */
static void reload(void)
{
	unsigned long uid = g_n ? g_msgs[g_sel].uid : 0;
	int i;

	g_stale = 0;
	if (cmd(NULL, NULL, "NOOP") < 0 || open_box(g_box) < 0) {
		say("%s", g_m.err);
		return;
	}
	for (i = 0; i < g_n; i++)
		if (g_msgs[i].uid == uid)
			g_sel = i;
}

static void list_loop(void)
{
	while (!g_quit) {
		int rows = scr_rows - 2, k;

		if (g_stale)
			reload();
		scr_check_size();
		draw_list();
		k = scr_getkey(-1);
		say("", NULL);
		switch (k) {
		case K_UP: case 'k':
			if (g_sel > 0)
				g_sel--;
			break;
		case K_DOWN: case 'j':
			if (g_sel + 1 < g_n)
				g_sel++;
			else
				fetch_older();
			break;
		case K_PGUP:
			g_sel = g_sel > rows ? g_sel - rows : 0;
			break;
		case K_PGDN: case ' ':
			if (g_sel + rows >= g_n)
				fetch_older();
			g_sel = g_sel + rows < g_n ? g_sel + rows : g_n - 1;
			break;
		case K_HOME:
			g_sel = 0;
			break;
		case K_END:
			g_sel = g_n ? g_n - 1 : 0;
			break;
		case '\r': case K_RIGHT:
			if (g_n > 0)
				read_message();
			break;
		case 'c':
			choose_folder();
			break;
		case 'g':
			reload();
			break;
		case 'm':
			compose(W_NEW);
			break;
		case 'r': case 'a': case 'f':
			/* (the message is read first: what the reply quotes) */
			if (g_n > 0) {
				load_message(&g_msgs[g_sel]);
				compose(k == 'r' ? W_REPLY : k == 'a' ? W_REPLY_ALL : W_FORWARD);
			}
			break;
		case 'q': case K_CLOSE:
			g_quit = 1;
			break;
		}
	}
}

/* --- starting ------------------------------------------------------------ */

/* the first line of ~/.manx/mailpass into g_pass: 0 (none there: g_pass
 * ""), -1 when others can read the file */
static int read_pass(void)
{
	char path[600];
	struct stat st;
	unsigned char *d;
	size_t len, i;

	g_pass[0] = '\0';
	if (os_datapath(path, sizeof path, "mailpass") == NULL || stat(path, &st) < 0)
		return 0;
	if (st.st_mode & 077) {
		fprintf(stderr, "manxmail: others can read %s: chmod 600 it\n", path);
		return -1;
	}
	if ((d = os_read_file(path, &len)) == NULL)
		return 0;
	for (i = 0; i < len && i + 1 < sizeof g_pass && d[i] != '\n' && d[i] != '\r'; i++)
		g_pass[i] = (char)d[i];
	g_pass[i] = '\0';
	memset(d, 0, len);
	xfree(d);
	return 0;
}

static void note(const char *msg)
{
	fprintf(stderr, "manxmail: %s\n", msg);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: manxmail [FOLDER]\n"
		"In ~/.manx/config:\n"
		"  mail_imap = imap.gmail.com      the server (host or host:port; 993, TLS)\n"
		"  mail_user = name@gmail.com      the login\n"
		"  mail_from = Ann Lee <ann@gmail.com>   who it's from (mail_user)\n"
		"  mail_smtp = smtp.gmail.com      where it's sent (imap. made smtp.; 465, TLS)\n"
		"  mail_sent = Sent                a folder for a copy of each (Gmail files them)\n"
		"The password: ~/.manx/mailpass (chmod 600), else asked for.\n"
		"(Gmail: an app password; it needs 2-Step Verification.)\n");
}

int main(int argc, char **argv)
{
	char path[600];
	const char *err = NULL, *h, *colon;

	if (strrchr(argv[0], '/') && strlen(argv[0]) + 5 < sizeof g_manx) {
		strcpy(g_manx, argv[0]);
		strcpy(strrchr(g_manx, '/') + 1, "manx");
		if (access(g_manx, X_OK) < 0)
			g_manx[0] = '\0';
	}
	if (config_load(os_datapath(path, sizeof path, "config"), &err) < 0) {
		fprintf(stderr, "manxmail: %s\n", err);
		return 2;
	}
	h = config_str("mail_imap", NULL);
	if (h == NULL || config_str("mail_user", NULL) == NULL || (argc > 1 && argv[1][0] == '-')) {
		usage();
		return 2;
	}
	snprintf(g_host, sizeof g_host, "%s", h);
	if ((colon = strrchr(g_host, ':')) != NULL) {
		g_port = (unsigned)atoi(colon + 1);
		g_host[colon - g_host] = '\0';
	}
	g_tls = config_bool("mail_tls", 1);
	snprintf(g_user, sizeof g_user, "%s", config_str("mail_user", ""));
	h = config_str("mail_from", NULL);
	snprintf(g_from, sizeof g_from, "%s", h ? h : strchr(g_user, '@') ? g_user : "");
	{
		struct addr a[1];

		if (addr_parse(g_from, a, 1, NULL, 0) == 1)
			strcpy(g_from_mbox, a[0].mbox);
	}
	if ((h = config_str("mail_smtp", NULL)) != NULL)
		snprintf(g_smtp, sizeof g_smtp, "%s", h);
	else if (strncmp(g_host, "imap.", 5) == 0)
		snprintf(g_smtp, sizeof g_smtp, "smtp.%s", g_host + 5);
	if ((colon = strrchr(g_smtp, ':')) != NULL) {
		g_smtp_port = (unsigned)atoi(colon + 1);
		g_smtp[colon - g_smtp] = '\0';
	}
	snprintf(g_sent, sizeof g_sent, "%s", config_str("mail_sent", ""));
	if (argc > 1)
		snprintf(g_box, sizeof g_box, "%s", argv[1]);
	if (read_pass() < 0)
		return 2;
	signal(SIGPIPE, SIG_IGN);
	entropy_init(os_datapath(path, sizeof path, "seed"));
	dns_cache_init(os_datapath(path, sizeof path, "dns"));
	tls_init(config_str("cafile", NULL), note);
	if (doc_init(&g_doc, 0) < 0 || scr_open(config_str("charset", NULL)) < 0) {
		fprintf(stderr, "manxmail: needs %s\n", scr_needs);
		return 1;
	}
	if (!g_pass[0] && !prompt("Password: ", g_pass, sizeof g_pass, 1)) {
		scr_close();
		return 1;
	}
	if (connect_server() < 0 || open_box(g_box) < 0) {
		char t[400];

		snprintf(t, sizeof t, "%s (a key to leave)", g_m.err);
		say("%s", t);
		status("");
		scr_flush(0);
		scr_getkey(-1);
		scr_close();
		fprintf(stderr, "manxmail: %s\n", g_m.err);
		imap_close(&g_m);
		memset(g_pass, 0, sizeof g_pass);
		return 1;
	}
	say("", NULL);
	list_loop();
	scr_close();
	imap_close(&g_m);
	memset(g_pass, 0, sizeof g_pass);
	entropy_save();
	return 0;
}
