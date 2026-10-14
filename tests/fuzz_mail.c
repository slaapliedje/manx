/*
 * fuzz_mail - manxmail's IMAP reader and MIME pieces under ASan/UBSan: a
 * server's transcripts mutated (bit flips, bytes inserted, deleted or
 * overwritten, literal counts changed, cut short) and fed in random
 * pieces through a stand-in connection; every value of every response
 * walked as manxmail walks them (iv_*, mime_parts, mime_pick, decoding).
 * Random bytes through the header, quoted-printable, base64 and
 * modified UTF-7 decoders too; an SMTP server's mutated replies through
 * logging in and sending; typed address lists and texts through
 * addr_parse and mail_build. Nothing may crash or read or write out of
 * bounds.
 *
 *   fuzz_mail ITERATIONS SEED
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "conn.h"
#include "imap.h"
#include "mime.h"
#include "smtp.h"
#include "compose.h"

static unsigned long s_rng = 1;

static unsigned long rnd(unsigned long n)
{
	s_rng = s_rng * 6364136223846793005ULL + 1442695040888963407ULL;
	return n ? (unsigned long)(s_rng >> 33) % n : 0;
}

/* --- the stand-in connection: the server's side from g_in ---------------- */

static unsigned char *g_in;
static size_t g_len, g_pos;
static int g_dummy;

struct conn *conn_open(const char *host, unsigned port, int is_tls, int early,
	char *err, size_t errlen)
{
	g_pos = 0;
	return (struct conn *)&g_dummy;
}

int conn_read(struct conn *c, void *buf, size_t len)
{
	size_t k;

	if (g_pos >= g_len)
		return rnd(4) ? 0 : -1;
	k = 1 + rnd(rnd(2) ? 16 : 3000);
	if (k > len)
		k = len;
	if (k > g_len - g_pos)
		k = g_len - g_pos;
	memcpy(buf, g_in + g_pos, k);
	g_pos += k;
	return (int)k;
}

int conn_write(struct conn *c, const void *buf, size_t len)
{
	return rnd(200) ? 0 : -1;
}

void conn_release(struct conn *c, int reusable)
{
}

/* --- what the server says -------------------------------------------------- */

static const char *seeds[] = {
	"* OK [CAPABILITY IMAP4rev1 AUTH=PLAIN SASL-IR] ready\r\n"
	"A0001 OK [CAPABILITY IMAP4rev1 IDLE] logged in\r\n"
	"* 5 EXISTS\r\n* 0 RECENT\r\n* OK [UIDVALIDITY 42] x\r\n* OK [UIDNEXT 6] y\r\n"
	"A0002 OK [READ-WRITE] done\r\n"
	"* 1 FETCH (UID 1 FLAGS (\\Seen) INTERNALDATE \"12-Oct-2026 12:30:00 -0600\" "
	"ENVELOPE (\"Mon, 12 Oct 2026 12:30:00 -0600\" \"=?UTF-8?B?R3LDvMOfZQ==?=\" "
	"((\"Alice\" NIL \"alice\" \"example.com\")) NIL NIL ((NIL NIL \"t\" \"e.com\")) NIL NIL NIL "
	"\"<id@x>\"))\r\n"
	"* 2 FETCH (UID 2 FLAGS () ENVELOPE ({9}\r\nMon, 12 O {3}\r\nabc NIL NIL NIL NIL NIL NIL NIL NIL))\r\n"
	"A0003 OK done\r\n",

	"* OK hi\r\n* CAPABILITY IMAP4rev1 AUTH=PLAIN\r\nA0001 OK\r\n"
	"+ \r\nA0002 NO [AUTHENTICATIONFAILED] Invalid credentials\r\n"
	"* 2 FETCH (UID 4 BODYSTRUCTURE (((\"text\" \"plain\" (\"charset\" \"us-ascii\") NIL NIL "
	"\"7bit\" 10 1 NIL NIL NIL)(\"text\" \"html\" (\"charset\" \"windows-1252\") NIL NIL "
	"\"quoted-printable\" 20 1 NIL NIL NIL) \"alternative\" (\"boundary\" \"a\") NIL NIL)"
	"(\"application\" \"pdf\" (\"name\" \"r.pdf\") NIL NIL \"base64\" 5000 NIL "
	"(\"attachment\" (\"filename\" \"=?utf-8?q?R=C3=A9sum=C3=A9.pdf?=\")) NIL)"
	"(\"message\" \"rfc822\" NIL NIL NIL \"7bit\" 300 (NIL \"s\" NIL NIL NIL NIL NIL NIL NIL NIL) "
	"(\"text\" \"plain\" NIL NIL NIL \"7bit\" 3 1 NIL NIL NIL) 9 NIL (\"inline\" NIL) NIL) "
	"\"mixed\" (\"boundary\" \"b\") NIL NIL))\r\n"
	"A0003 OK done\r\n",

	"* OK ready\r\n"
	"* 3 FETCH (UID 3 BODY[1] {44}\r\nQ2Fmw6kgYXUgbGFpdCwgY3LDqG1lIGJyw7tsw6llLgo= FLAGS (\\Seen))\r\n"
	"* 3 FETCH (BODY[2] {24}\r\nCaf=E9 =\r\nau lait=3D=\r\n BODY[3] NIL)\r\n"
	"A0001 OK done\r\n"
	"* LIST (\\HasNoChildren) \"/\" \"INBOX\"\r\n"
	"* LIST (\\HasNoChildren \\Sent) \"/\" \"[Gmail]/&AMk-t&AOk-\"\r\n"
	"* LIST () NIL {5}\r\nS\"x\\y\r\n"
	"A0002 OK done\r\n",

	"* PREAUTH [CAPABILITY IMAP4rev1] hi\r\n"
	"* 1 FETCH (UID 9 BODYSTRUCTURE (\"TEXT\" \"PLAIN\" (\"CHARSET\" \"UTF-16\") NIL NIL "
	"\"BASE64\" 50 2 NIL (\"ATTACHMENT\" (\"FILENAME*\" \"x\")) NIL) "
	"BODY[1] {8}\r\nAP4AaQ==)\r\n"
	"* BYE going\r\nA0001 BAD what\r\n",
};

static const char *smtp_seeds[] = {
	"220 fake ESMTP ready\r\n250-fake greets you\r\n250-8BITMIME\r\n"
	"250-AUTH LOGIN PLAIN\r\n250 SIZE 35882577\r\n235 2.7.0 Accepted\r\n"
	"250 2.1.0 OK\r\n250 2.1.5 OK\r\n250 OK\r\n354 Go ahead\r\n250 2.0.0 OK queued\r\n"
	"221 bye\r\n",

	"220 hi\r\n250-x\r\n250 AUTH=LOGIN\r\n334 VXNlcm5hbWU6\r\n334 UGFzc3dvcmQ6\r\n"
	"235 ok\r\n250 ok\r\n550 5.1.1 No such user\r\n",

	"220 hi\r\n250-x\r\n250 AUTH XOAUTH2 OAUTHBEARER\r\n",

	"220 hi\r\n250 x\r\n535-5.7.8 Username and Password not accepted.\r\n535 5.7.8 Try\r\n",
};

static void mutate(unsigned char **buf, size_t *len)
{
	unsigned char *b = *buf;
	size_t n = *len, k = 1 + rnd(8), i;

	for (i = 0; i < k; i++) {
		size_t at = n ? rnd(n) : 0;

		switch (rnd(7)) {
		case 0:
			if (n)
				b[at] ^= (unsigned char)(1 << rnd(8));
			break;
		case 1:		/* a byte in */
			b = realloc(b, n + 2);
			memmove(b + at + 1, b + at, n - at);
			b[at] = "(){}\"\\[] \r\n0123456789*NIL"[rnd(26)];
			n++;
			break;
		case 2:		/* bytes out */
			if (n) {
				size_t d = 1 + rnd(n - at < 8 ? n - at : 8);

				memmove(b + at, b + at + d, n - at - d);
				n -= d;
			}
			break;
		case 3:
			if (n)
				b[at] = (unsigned char)rnd(256);
			break;
		case 4:		/* a literal's count changed */
			for (; at < n && b[at] != '{'; at++)
				;
			if (at + 1 < n && b[at + 1] >= '0' && b[at + 1] <= '9')
				b[at + 1] = (unsigned char)('0' + rnd(10));
			break;
		case 5:		/* cut short */
			n = at;
			break;
		case 6:		/* a run of brackets: the depth limit */
			{
				size_t r = 1 + rnd(60), j;

				b = realloc(b, n + r + 1);
				memmove(b + at + r, b + at, n - at);
				for (j = 0; j < r; j++)
					b[at + j] = rnd(2) ? '(' : '[';
				n += r;
			}
			break;
		}
	}
	*buf = b;
	*len = n;
}

/* --- what manxmail does with it ------------------------------------------- */

static unsigned long g_sink, g_resps, g_parts;

static void walk(struct imap *m, struct ival *v, int depth)
{
	char s[256];
	unsigned long i;

	for (; v; v = iv_next(m, v)) {
		iv_str(v, s, 1 + rnd(sizeof s));
		g_sink += iv_num(v) + (unsigned long)iv_is(v, "NIL") + strlen(s);
		if (v->type == IV_STR && v->len) {
			char out[300];

			mime_header(v->s, v->len, out, 1 + rnd(sizeof out));
			mime_mutf7(s, out, 1 + rnd(sizeof out));
		}
		if (v->type == IV_LIST && depth < 40) {
			struct mpart p[12];
			int k = mime_parts(m, v, p, 1 + (int)rnd(12)), j;

			g_parts += (unsigned long)k;
			for (j = 0; j < k; j++)
				g_sink += strlen(p[j].section) + strlen(p[j].name) + p[j].attachment;
			if ((j = mime_pick(p, k, (int)rnd(2))) >= 0 && v->n)
				g_sink += (unsigned long)j;
			for (i = 0; i < v->n + 2; i++)
				g_sink += iv_at(m, v, i) != NULL;
			walk(m, iv_at(m, v, 0), depth + 1);
		}
	}
}

static void on_resp(void *ctx, struct imap *m, struct ival *v, unsigned long n)
{
	struct mpart *p = ctx;
	struct ival *x;

	g_resps++;
	walk(m, v, 0);
	/* a BODY[...] as manxmail takes it: decoded, into UTF-8 */
	for (x = v; x; x = iv_next(m, x))
		if (x->type == IV_LIST) {
			struct ival *k;

			for (k = iv_at(m, x, 0); k; k = iv_next(m, k))
				if (k->type == IV_STR) {
					size_t len;
					unsigned char *d = mime_decode(p, k->s, k->len, &len);
					char *u = d ? mime_utf8(p->charset, d, len) : NULL;

					g_sink += u ? strlen(u) : 0;
					xfree(d);
					xfree(u);
				}
		}
}

static const char *charsets[] = {
	"utf-8", "iso-8859-1", "windows-1252", "utf-16", "utf-16be", "shift_jis",
	"euc-kr", "gb18030", "big5", "iso-2022-jp", "koi8-r", "bogus", ""
};

static void session(const char *seed)
{
	static struct imap m;
	struct mpart p;
	unsigned char *b;
	size_t n = strlen(seed);
	int i;

	b = malloc(n + 1);
	memcpy(b, seed, n);
	if (rnd(8))
		mutate(&b, &n);
	g_in = b;
	g_len = n;

	memset(&p, 0, sizeof p);
	strcpy(p.encoding, rnd(3) == 0 ? "base64" : rnd(2) ? "quoted-printable" : "7bit");
	strcpy(p.charset, charsets[rnd(sizeof charsets / sizeof *charsets)]);
	if (imap_open(&m, "127.0.0.1", 1143, 0) == 0) {
		if (rnd(2))
			imap_auth(&m, "user", "pass");
		for (i = 0; i < 4 && m.c; i++)
			imap_cmd(&m, on_resp, &p, "FETCH 1:* (UID FLAGS)");
	}
	imap_close(&m);
	free(b);
}

static void smtp_session(void)
{
	static struct smtp s;
	const char *seed = smtp_seeds[rnd(sizeof smtp_seeds / sizeof *smtp_seeds)];
	unsigned char *b;
	size_t n = strlen(seed);
	char *rcpt[3] = { "a@b.example", "c@d.example", "e@f.example" };
	char msg[300];
	size_t i, ml = rnd(sizeof msg);

	b = malloc(n + 1);
	memcpy(b, seed, n);
	if (rnd(8))
		mutate(&b, &n);
	g_in = b;
	g_len = n;
	for (i = 0; i < ml; i++)
		msg[i] = ".\r\nab"[rnd(5)];
	if (smtp_open(&s, "127.0.0.1", 1144, 0) == 0 && smtp_auth(&s, "user", "pass") == 0)
		smtp_send(&s, "me@x.example", rcpt, 1 + (int)rnd(3), msg, ml);
	g_sink += strlen(s.err) + strlen(s.said);
	smtp_close(&s);
	free(b);
}

/* a message as mail_build makes it: headers only of its own (no line
 * typed into one starts another), CRLF lines and nothing else, none
 * longer than SMTP takes */
static void check_msg(const char *m, size_t len)
{
	static const char *const known[] = { "Date:", "From:", "To:", "Cc:", "Subject:",
		"Message-ID:", "In-Reply-To:", "References:", "MIME-Version:", "Content-Type:",
		"Content-Transfer-Encoding:", "User-Agent:" };
	const char *p = m, *e = m + len;
	int head = 1;

	while (p < e) {
		const char *l = p;
		size_t n, k;

		while (p < e && *p != '\r' && *p != '\n')
			p++;
		n = (size_t)(p - l);
		if (p + 1 >= e || p[0] != '\r' || p[1] != '\n' || n > 998) {
			fprintf(stderr, "fuzz_mail: a line not ended by CRLF, or too long (%lu)\n",
				(unsigned long)n);
			abort();
		}
		p += 2;
		if (!head)
			continue;
		if (n == 0) {
			head = 0;
			continue;
		}
		if (l[0] == ' ' || l[0] == '\t')
			continue;
		for (k = 0; k < sizeof known / sizeof *known; k++)
			if (strncmp(l, known[k], strlen(known[k])) == 0)
				break;
		if (k == sizeof known / sizeof *known) {
			fprintf(stderr, "fuzz_mail: a header mail_build didn't mean: %.*s\n", (int)n, l);
			abort();
		}
	}
}

/* typed text: addresses, a message put together from it */
static void typed(void)
{
	char t[3][300], bad[64];
	struct addr a[8];
	struct outmsg o;
	size_t len, i, k;
	char *msg;

	for (k = 0; k < 3; k++) {
		size_t n = rnd(sizeof t[k]);

		for (i = 0; i < n; i++)
			t[k][i] = rnd(3) ? "\"<>,@\\ abc.\n\t\r=?"[rnd(16)] : (char)(1 + rnd(255));
		t[k][n] = '\0';
	}
	g_sink += (unsigned long)addr_parse(t[0], a, 1 + (int)rnd(8), bad, 1 + rnd(sizeof bad));
	memset(&o, 0, sizeof o);
	o.from = rnd(2) ? "Me <me@x.example>" : t[1];
	o.to = rnd(2) ? t[0] : "a@b.example, \"C, D\" <c@d.example>";
	o.cc = rnd(2) ? NULL : t[1];
	o.subject = t[1];
	o.date = "Wed, 14 Oct 2026 11:30:00 -0600";
	o.msgid = "<x@y>";
	o.in_reply_to = rnd(2) ? "<p@q>" : NULL;
	o.references = rnd(2) ? NULL : t[0];
	o.body = t[2];
	if ((msg = mail_build(&o, &len)) != NULL) {
		check_msg(msg, len);
		g_sink += len;
		xfree(msg);
	}
}

static void bytes(void)
{
	char in[600], out[700];
	unsigned char raw[700];
	size_t n = rnd(sizeof in - 1), i;
	char *u;

	for (i = 0; i < n; i++)
		in[i] = rnd(3) ? "=?_Qq-&AB+/,09abcz \r\n"[rnd(21)] : (char)rnd(256);
	in[n] = '\0';
	mime_header(in, n, out, 1 + rnd(sizeof out));
	mime_mutf7(in, out, 1 + rnd(sizeof out));
	g_sink += qp_decode(in, n, raw, (int)rnd(2));
	g_sink += b64_decode(in, n, raw);
	b64_encode((const unsigned char *)in, n * 3 / 4 < 500 ? n * 3 / 4 : 500, out);
	u = mime_utf8(charsets[rnd(sizeof charsets / sizeof *charsets)], (unsigned char *)in, n);
	g_sink += u ? strlen(u) : 0;
	xfree(u);
	{
		char q[64];

		g_sink += (unsigned long)imap_quote(q, 1 + rnd(sizeof q), in);
	}
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 10) : 10000, i;

	s_rng = argc > 2 ? strtoul(argv[2], NULL, 10) : 1;
	for (i = 0; i < iters; i++) {
		session(seeds[rnd(sizeof seeds / sizeof *seeds)]);
		smtp_session();
		typed();
		bytes();
	}
	printf("fuzz_mail: %lu sessions, %lu responses, %lu parts: nothing broke\n",
		iters, g_resps, g_parts);
	return 0;
}
