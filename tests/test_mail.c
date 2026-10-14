/*
 * test_mail.c - manxmail's pieces: base64, quoted-printable, RFC 2047,
 * modified UTF-7, IMAP's response values and quoting, BODYSTRUCTURE's
 * parts and the one picked to show.
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "imap.h"
#include "mime.h"

static int fails, runs;

static void same(const char *what, const char *got, const char *want)
{
	runs++;
	if (strcmp(got, want) != 0) {
		fails++;
		printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
	}
}

static void ok(const char *what, int cond)
{
	runs++;
	if (!cond) {
		fails++;
		printf("FAIL %s\n", what);
	}
}

static void codings(void)
{
	char b[200];
	unsigned char u[200];
	size_t n;

	b64_encode((const unsigned char *)"\0u\0p", 4, b);
	same("base64 PLAIN", b, "AHUAcA==");
	b64_encode((const unsigned char *)"ab", 2, b);
	same("base64 two", b, "YWI=");
	n = b64_decode("SGVs\r\nbG8=", 10, u);
	u[n] = '\0';
	same("base64 decode", (char *)u, "Hello");

	n = qp_decode("Caf=C3=A9 =\r\nau lait=3D", 23, u, 0);
	u[n] = '\0';
	same("quoted-printable", (char *)u, "Caf\xc3\xa9 au lait=");
	n = qp_decode("Caf=E9_noir", 11, u, 1);
	u[n] = '\0';
	same("Q", (char *)u, "Caf\xe9 noir");

	mime_header("=?UTF-8?B?Q2Fmw6k=?= =?ISO-8859-1?Q?au_lait=E9?=", 48, b, sizeof b);
	same("2047 words", b, "Caf\xc3\xa9" "au lait\xc3\xa9");
	mime_header("Re: =?utf-8?q?Gr=C3=BC=C3=9Fe?= from\r\n me", 41, b, sizeof b);
	same("2047 among words", b, "Re: Gr\xc3\xbc\xc3\x9f" "e from me");
	mime_header("=?bogus", 7, b, sizeof b);
	same("2047 broken", b, "=?bogus");

	mime_mutf7("&AMk-t&AOk-", b, sizeof b);
	same("mUTF-7", b, "\xc3\x89t\xc3\xa9");
	mime_mutf7("[Gmail]/Sent Mail", b, sizeof b);
	same("mUTF-7 plain", b, "[Gmail]/Sent Mail");
	mime_mutf7("a&-b", b, sizeof b);
	same("mUTF-7 &", b, "a&b");

	ok("quote", imap_quote(b, sizeof b, "a\"b\\c") == 0 && strcmp(b, "\"a\\\"b\\\\c\"") == 0);
	ok("quote no CR", imap_quote(b, sizeof b, "a\rb") < 0);
}

static struct imap m;

/* line, parsed: its first value */
static struct ival *parse(const char *line, unsigned long *n)
{
	static char buf[2000];
	long k;

	strcpy(buf, line);
	k = imap_parse(&m, buf, buf + strlen(buf), n);
	return k >= 0 ? &m.iv[k] : NULL;
}

static void values(void)
{
	unsigned long n;
	struct ival *v = parse("12 FETCH (UID 34 BODY[1] {5}hello FLAGS (\\Seen NIL))", &n), *f, *x;
	char s[64];

	ok("parse count", n == 3 && v && iv_num(v) == 12);
	f = iv_next(&m, iv_next(&m, v));
	ok("fetch list", f && f->type == IV_LIST && f->n == 6);
	ok("uid", iv_is(iv_at(&m, f, 0), "UID") && iv_num(iv_at(&m, f, 1)) == 34);
	same("section atom", iv_str(iv_at(&m, f, 2), s, sizeof s), "BODY[1]");
	x = iv_at(&m, f, 3);
	ok("literal", x && x->type == IV_STR && x->len == 5 && memcmp(x->s, "hello", 5) == 0);
	x = iv_at(&m, f, 5);
	ok("flags", x && x->n == 2 && iv_is(iv_at(&m, x, 0), "\\Seen")
		&& iv_at(&m, x, 1)->type == IV_NIL);

	v = parse("LIST (\\HasNoChildren) \"/\" \"a \\\"q\\\" b\"", &n);
	same("unescaped", iv_str(iv_next(&m, iv_next(&m, iv_next(&m, v))), s, sizeof s),
		"a \"q\" b");
	v = parse("OK [UIDVALIDITY 3857529045] UIDs valid", &n);
	x = iv_next(&m, v);
	ok("response code", x && x->type == IV_LIST && iv_is(iv_at(&m, x, 0), "UIDVALIDITY")
		&& iv_num(iv_at(&m, x, 1)) == 3857529045UL);
}

static void parts(void)
{
	struct mpart p[16];
	unsigned long n;
	struct ival *v;
	int k;

	v = parse("((\"text\" \"plain\" (\"charset\" \"utf-8\") NIL NIL \"quoted-printable\" 120 4 NIL NIL NIL)"
		"(\"text\" \"html\" (\"charset\" \"utf-8\") NIL NIL \"base64\" 400 6 NIL NIL NIL) "
		"\"alternative\" (\"boundary\" \"x\") NIL NIL)", &n);
	k = mime_parts(&m, v, p, 16);
	ok("alternative: two", k == 2 && strcmp(p[0].section, "1") == 0
		&& strcmp(p[1].section, "2") == 0 && strcmp(p[1].encoding, "base64") == 0);
	ok("pick plain", mime_pick(p, k, 0) == 0);
	ok("pick html", mime_pick(p, k, 1) == 1);

	v = parse("(((\"text\" \"plain\" (\"charset\" \"us-ascii\") NIL NIL \"7bit\" 10 1 NIL NIL NIL)"
		"(\"text\" \"html\" (\"charset\" \"us-ascii\") NIL NIL \"7bit\" 20 1 NIL NIL NIL) "
		"\"alternative\" (\"boundary\" \"a\") NIL NIL)"
		"(\"application\" \"pdf\" (\"name\" \"r.pdf\") NIL NIL \"base64\" 5000 NIL "
		"(\"attachment\" (\"filename\" \"=?utf-8?q?R=C3=A9sum=C3=A9.pdf?=\")) NIL) "
		"\"mixed\" (\"boundary\" \"b\") NIL NIL)", &n);
	k = mime_parts(&m, v, p, 16);
	ok("mixed: three", k == 3 && strcmp(p[0].section, "1.1") == 0
		&& strcmp(p[1].section, "1.2") == 0 && strcmp(p[2].section, "2") == 0);
	ok("attachment", k == 3 && p[2].attachment && !p[0].attachment);
	same("attachment's name", k == 3 ? p[2].name : "", "R\xc3\xa9sum\xc3\xa9.pdf");
	ok("pick in mixed", mime_pick(p, k, 0) == 0);

	v = parse("(\"TEXT\" \"PLAIN\" (\"CHARSET\" \"iso-8859-1\") NIL NIL \"QUOTED-PRINTABLE\" 50 2 NIL NIL NIL)", &n);
	k = mime_parts(&m, v, p, 16);
	ok("single part", k == 1 && strcmp(p[0].section, "1") == 0
		&& strcmp(p[0].type, "text") == 0 && strcmp(p[0].charset, "iso-8859-1") == 0
		&& strcmp(p[0].encoding, "quoted-printable") == 0);
	{
		size_t len;
		unsigned char *d = mime_decode(&p[0], "Caf=E9", 6, &len);
		char *u = d ? mime_utf8(p[0].charset, d, len) : NULL;

		same("a part in UTF-8", u ? u : "", "Caf\xc3\xa9");
		xfree(d);
		xfree(u);
	}
}

int main(void)
{
	codings();
	values();
	parts();
	printf("mail: %d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
