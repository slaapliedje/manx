/*
 * smtp.c - sending mail (smtp.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "mime.h"
#include "smtp.h"

static int fail(struct smtp *s, const char *fmt, const char *arg)
{
	snprintf(s->err, sizeof s->err, fmt, arg ? arg : "");
	return -1;
}

static void drop(struct smtp *s)
{
	if (s->c) {
		conn_release(s->c, 0);
		s->c = NULL;
	}
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* one line into s->line, without its CRLF: 0, or -1 */
static int read_line(struct smtp *s)
{
	size_t n = 0;

	for (;;) {
		unsigned char ch;

		if (s->rpos == s->rlen) {
			int r = conn_read(s->c, s->rbuf, sizeof s->rbuf);

			if (r <= 0)
				return -1;
			s->rpos = 0;
			s->rlen = (size_t)r;
		}
		ch = s->rbuf[s->rpos++];
		if (ch == '\n')
			break;
		if (ch != '\r' && n + 1 < sizeof s->line)
			s->line[n++] = (char)ch;
	}
	s->line[n] = '\0';
	return 0;
}

/* a reply, all its lines ("250-..." go on, "250 ..." ends it): its code,
 * or -1; the first line in s->said, and with ehlo, the others in s->ext */
static int reply(struct smtp *s, int ehlo)
{
	int code = -1, first = 1;
	size_t o = 0;

	if (ehlo)
		s->ext[0] = '\0';
	for (;;) {
		const char *l = s->line;

		if (read_line(s) < 0) {
			drop(s);
			return fail(s, "the server went away", NULL);
		}
		if (strlen(l) < 3 || l[0] < '1' || l[0] > '5' || l[1] < '0' || l[1] > '9'
			|| l[2] < '0' || l[2] > '9' || (l[3] && l[3] != '-' && l[3] != ' ')) {
			drop(s);
			snprintf(s->err, sizeof s->err, "not a mail server's answer: %.200s", l);
			return -1;
		}
		code = atoi(l);
		if (first)
			snprintf(s->said, sizeof s->said, "%.199s", l);
		else if (ehlo && o + strlen(l + 4) + 2 < sizeof s->ext) {
			strcpy(s->ext + o, l + 4);
			o += strlen(l + 4);
			s->ext[o++] = '\n';
			s->ext[o] = '\0';
		}
		first = 0;
		if (l[3] != '-')
			return code;
	}
}

/* a command line (fmt with one %s); its reply must be code want (with
 * ehlo, what it says it can do kept). The line is wiped after (it may be a
 * login). */
static int cmd_x(struct smtp *s, int want, const char *fmt, const char *arg, int ehlo)
{
	char line[800];
	int k, code;

	if (s->c == NULL)
		return fail(s, "not connected", NULL);
	k = snprintf(line, sizeof line - 2, fmt, arg ? arg : "");
	if (k < 0 || (size_t)k >= sizeof line - 2)
		return fail(s, "a command too long", NULL);
	line[k++] = '\r';
	line[k++] = '\n';
	if (conn_write(s->c, line, (size_t)k) < 0) {
		memset(line, 0, sizeof line);
		drop(s);
		return fail(s, "can't write to the server", NULL);
	}
	memset(line, 0, sizeof line);
	if ((code = reply(s, ehlo)) < 0)
		return -1;
	if (code != want)
		return fail(s, "the server said: %s", s->said);
	return 0;
}

static int cmd(struct smtp *s, int want, const char *fmt, const char *arg)
{
	return cmd_x(s, want, fmt, arg, 0);
}

/* does the EHLO line "AUTH ..." list mechanism mech? */
static int has_auth(const struct smtp *s, const char *mech)
{
	const char *l = s->ext;
	size_t ml = strlen(mech);

	while (*l) {
		const char *e = strchr(l, '\n');
		size_t n = e ? (size_t)(e - l) : strlen(l);

		if (n > 5 && lower(l[0]) == 'a' && lower(l[1]) == 'u' && lower(l[2]) == 't'
			&& lower(l[3]) == 'h' && (l[4] == ' ' || l[4] == '=')) {
			const char *p = l + 5, *end = l + n;

			while (p < end) {
				const char *w = p;
				size_t i;

				while (p < end && *p != ' ')
					p++;
				if ((size_t)(p - w) == ml) {
					for (i = 0; i < ml && lower(w[i]) == lower(mech[i]); i++)
						;
					if (i == ml)
						return 1;
				}
				while (p < end && *p == ' ')
					p++;
			}
		}
		l += n + (e != NULL);
	}
	return 0;
}

int smtp_addr_ok(const char *a)
{
	const char *at = strchr(a, '@');

	if (at == NULL || at == a || at[1] == '\0' || strlen(a) > 254)
		return 0;
	for (; *a; a++)
		if ((unsigned char)*a <= ' ' || (unsigned char)*a >= 127 || *a == '<' || *a == '>')
			return 0;
	return 1;
}

int smtp_open(struct smtp *s, const char *host, unsigned port, int tls)
{
	char err[256];
	int code;

	memset(s, 0, sizeof *s);
	if (!tls && strcmp(host, "127.0.0.1") != 0 && strcmp(host, "localhost") != 0)
		return fail(s, "SMTP without TLS only to this machine: a password "
			"would cross the network as it is", NULL);
	if (port == 587 || port == 25)
		return fail(s, "port 587 or 25 begins without TLS (STARTTLS), which this "
			"doesn't do: port 465 takes TLS from the start", NULL);
	if ((s->c = conn_open(host, port, tls, 0, err, sizeof err)) == NULL)
		return fail(s, "%s", err);
	if ((code = reply(s, 0)) < 0)
		return -1;
	if (code != 220) {
		drop(s);
		return fail(s, "the server said: %s", s->said);
	}
	/* (an address literal: this machine has no name the world knows) */
	return cmd_x(s, 250, "EHLO [127.0.0.1]", NULL, 1);
}

int smtp_auth(struct smtp *s, const char *user, const char *pass)
{
	size_t ul = strlen(user), pl = strlen(pass), n = ul + pl + 2;
	unsigned char *raw;
	char *b;
	int rc;

	if (has_auth(s, "PLAIN")) {
		if ((raw = xmalloc(n)) == NULL || (b = xmalloc(4 * (n / 3 + 1) + 1)) == NULL) {
			xfree(raw);
			return fail(s, "out of memory", NULL);
		}
		raw[0] = '\0';
		memcpy(raw + 1, user, ul);
		raw[1 + ul] = '\0';
		memcpy(raw + 2 + ul, pass, pl);
		b64_encode(raw, n, b);
		memset(raw, 0, n);
		xfree(raw);
		rc = strlen(b) < 700 ? cmd(s, 235, "AUTH PLAIN %s", b)
			: fail(s, "a user or password too long", NULL);
		memset(b, 0, strlen(b));
		xfree(b);
		return rc;
	}
	if (has_auth(s, "LOGIN")) {
		char u[400], p[400];

		if (ul > 290 || pl > 290)
			return fail(s, "a user or password too long", NULL);
		b64_encode((const unsigned char *)user, ul, u);
		b64_encode((const unsigned char *)pass, pl, p);
		rc = cmd(s, 334, "AUTH LOGIN", NULL) < 0 || cmd(s, 334, "%s", u) < 0
			|| cmd(s, 235, "%s", p) < 0 ? -1 : 0;
		memset(p, 0, sizeof p);
		return rc;
	}
	if (has_auth(s, "XOAUTH2") || has_auth(s, "OAUTHBEARER"))
		return fail(s, "the server takes only OAuth2 logins (as Outlook and Microsoft "
			"365 do), not a password: manxmail can't log in to it", NULL);
	return fail(s, "the server offers no login this client knows", NULL);
}

int smtp_send(struct smtp *s, const char *from, char **rcpt, int nrcpt,
	const char *msg, size_t len)
{
	char out[1024];
	size_t o = 0, i;
	int k, bol = 1, code;

	if (!smtp_addr_ok(from))
		return fail(s, "not an address to send from: %s", from);
	if (nrcpt <= 0)
		return fail(s, "no one to send it to", NULL);
	for (k = 0; k < nrcpt; k++)
		if (!smtp_addr_ok(rcpt[k]))
			return fail(s, "not an address: %s", rcpt[k]);
	if (cmd(s, 250, "MAIL FROM:<%s>", from) < 0)
		return -1;
	for (k = 0; k < nrcpt; k++)
		if (cmd(s, 250, "RCPT TO:<%s>", rcpt[k]) < 0) {
			char e[sizeof s->err];

			snprintf(e, sizeof e, "%s", s->err);
			snprintf(s->err, sizeof s->err, "%.40s: %.200s", rcpt[k], e);
			return -1;
		}
	if (cmd(s, 354, "DATA", NULL) < 0)
		return -1;
	/* the message, a line beginning with "." given another */
	for (i = 0; i <= len; i++) {
		if (o + 4 > sizeof out || i == len) {
			if (conn_write(s->c, out, o) < 0) {
				drop(s);
				return fail(s, "can't write to the server", NULL);
			}
			o = 0;
			if (i == len)
				break;
		}
		if (bol && msg[i] == '.')
			out[o++] = '.';
		out[o++] = msg[i];
		bol = msg[i] == '\n';
	}
	if (conn_write(s->c, bol ? ".\r\n" : "\r\n.\r\n", bol ? 3 : 5) < 0) {
		drop(s);
		return fail(s, "can't write to the server", NULL);
	}
	if ((code = reply(s, 0)) < 0)
		return -1;
	if (code != 250)
		return fail(s, "the server said: %s", s->said);
	return 0;
}

void smtp_close(struct smtp *s)
{
	if (s->c) {
		cmd(s, 221, "QUIT", NULL);
		drop(s);
	}
}
