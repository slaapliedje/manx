/*
 * test_html - the HTML engine: tables, charset handling, tokenizer and
 * tree builder against expected trees. Every case is also parsed in
 * chunks of 1, 2, 3, 7 and 64 bytes, which must give the same tree.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "utf8.h"
#include "tags.h"
#include "doc.h"
#include "load.h"

static int fails;

/* parse s in chunks of `step` (0: whole) and return the dump (malloc'd) */
static char *parse(const char *s, size_t len, size_t step, const char *cs,
	int plain)
{
	static struct html_load l;
	struct doc d;
	char *out = NULL;
	size_t outlen = 0, off = 0;
	FILE *f;

	doc_init(&d, 0);
	html_load_begin(&l, &d, cs, plain);
	while (off < len) {
		size_t n = step && len - off > step ? step : len - off;

		html_load_feed(&l, (const unsigned char *)s + off, n);
		off += n;
	}
	html_load_end(&l);
	f = open_memstream(&out, &outlen);
	doc_dump(&d, f);
	fclose(f);
	doc_free(&d);
	return out;
}

static void check(const char *what, const char *html, const char *want)
{
	static const size_t steps[] = { 0, 1, 2, 3, 7, 64 };
	size_t i, len = strlen(html);

	for (i = 0; i < sizeof steps / sizeof steps[0]; i++) {
		char *got = parse(html, len, steps[i], "utf-8", 0);

		if (strcmp(got, want) != 0) {
			printf("FAIL %s (chunks of %lu):\n--- want\n%s--- got\n%s",
				what, (unsigned long)steps[i], want, got);
			fails++;
			free(got);
			return;
		}
		free(got);
	}
}

static void check_body(const char *what, const char *html, const char *want_body)
{
	/* the usual preamble, with the body's content indented under it */
	char want[4096];

	snprintf(want, sizeof want, "<html>\n  <head>\n  <body>\n%s", want_body);
	check(what, html, want);
}

static void tables(void)
{
	int i;

	for (i = 2; i < TAG_COUNT; i++)
		if (strcmp(tag_name(i - 1), tag_name(i)) >= 0
			|| tag_lookup(tag_name(i)) != i) {
			printf("FAIL tag table order at %s\n", tag_name(i));
			fails++;
		}
	for (i = 2; i < ATTR_COUNT; i++)
		if (strcmp(attr_name(i - 1), attr_name(i)) >= 0
			|| attr_lookup(attr_name(i)) != i) {
			printf("FAIL attr table order at %s\n", attr_name(i));
			fails++;
		}
	if (tag_lookup("blink") != TAG_BLINK || tag_lookup("nosuch") != TAG_UNKNOWN
		|| attr_lookup("onclick") != ATTR_NONE) {
		printf("FAIL lookups\n");
		fails++;
	}
}

static void charsets(void)
{
	struct decoder d;
	char out[64];
	size_t n;
	int from;
	size_t bom;

	/* UTF-8 split in every place, and broken sequences */
	{
		const unsigned char in[] = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
		size_t cut, total;

		for (cut = 0; cut <= sizeof in - 1; cut++) {
			decoder_init(&d, CS_UTF8);
			total = decoder_run(&d, in, cut, out);
			total += decoder_run(&d, in + cut, sizeof in - 1 - cut, out + total);
			total += decoder_end(&d, out + total);
			if (total != sizeof in - 1 || memcmp(out, in, total) != 0) {
				printf("FAIL utf-8 split at %lu\n", (unsigned long)cut);
				fails++;
			}
		}
	}
	/* random bytes: split anywhere, the output must not change */
	{
		unsigned long seed = 12345;
		int round;

		for (round = 0; round < 300; round++) {
			unsigned char in[48];
			char whole[256], split[256];
			size_t i, cut, wn, sn;

			for (i = 0; i < sizeof in; i++) {
				seed = seed * 1103515245UL + 12345UL;
				/* mostly high bytes: many broken sequences */
				in[i] = (unsigned char)(seed >> 16);
				if ((seed >> 8) % 4 == 0)
					in[i] = (unsigned char)(0x80 | (in[i] & 0x3F));
			}
			decoder_init(&d, CS_UTF8);
			wn = decoder_run(&d, in, sizeof in, whole);
			wn += decoder_end(&d, whole + wn);
			for (cut = 1; cut < sizeof in; cut++) {
				decoder_init(&d, CS_UTF8);
				sn = decoder_run(&d, in, cut, split);
				sn += decoder_run(&d, in + cut, sizeof in - cut, split + sn);
				sn += decoder_end(&d, split + sn);
				if (sn != wn || memcmp(whole, split, wn) != 0) {
					printf("FAIL decoder split-invariance (round %d, cut %lu)\n",
						round, (unsigned long)cut);
					fails++;
					round = 300;
					break;
				}
			}
		}
	}
	decoder_init(&d, CS_UTF8);
	n = decoder_run(&d, (const unsigned char *)"a\xFFz\xC3(\xE2\x82", 7, out);
	n += decoder_end(&d, out + n);
	out[n] = '\0';
	if (strcmp(out, "a\xEF\xBF\xBDz\xEF\xBF\xBD(\xEF\xBF\xBD") != 0) {
		printf("FAIL invalid utf-8 handling: %s\n", out);
		fails++;
	}
	decoder_init(&d, CS_WIN1252);
	n = decoder_run(&d, (const unsigned char *)"\x93q\x94 \xE9\x80", 6, out);
	out[n] = '\0';
	if (strcmp(out, "\xE2\x80\x9Cq\xE2\x80\x9D \xC3\xA9\xE2\x82\xAC") != 0) {
		printf("FAIL windows-1252: %s\n", out);
		fails++;
	}
	if (charset_from_label(" ISO-8859-1") != CS_WIN1252
		|| charset_from_label("UTF-8") != CS_UTF8
		|| charset_from_label("koi8-r") != CS_UNKNOWN) {
		printf("FAIL charset labels\n");
		fails++;
	}
	if (html_sniff((const unsigned char *)"<meta charset=\"iso-8859-1\">", 27,
		&from, &bom) != CS_WIN1252 || from != CS_FROM_META
		|| html_sniff((const unsigned char *)"<META http-equiv=Content-Type "
		"content=\"text/html; charset=UTF-8\">", 64, &from, &bom) != CS_UTF8
		|| html_sniff((const unsigned char *)"\xEF\xBB\xBFx", 4, &from, &bom)
			!= CS_UTF8 || bom != 3
		|| html_sniff((const unsigned char *)"caf\xE9", 4, &from, &bom) != CS_WIN1252
		|| html_sniff((const unsigned char *)"caf\xC3\xA9", 5, &from, &bom) != CS_UTF8) {
		printf("FAIL sniffing\n");
		fails++;
	}
	/* transliteration */
	{
		char t[8];
		int k;

		k = translit(0x2014, 0, t);
		t[k] = '\0';
		if (strcmp(t, "--") || translit(0xE9, 1, t) != 1 || (unsigned char)t[0] != 0xE9
			|| translit(0xE9, 0, t) != 1 || t[0] != 'e' || translit(0x200B, 0, t) != 0
			|| translit(0x4E2D, 0, t) != 1 || t[0] != '?'
			|| translit(0x0142, 0, t) != 1 || t[0] != 'l') {
			printf("FAIL transliteration\n");
			fails++;
		}
	}
}

int main(void)
{
	tables();
	charsets();

	check_body("basic", "<p>Hello <b>world</b>",
		"    <p>\n      \"Hello \"\n      <b>\n        \"world\"\n");
	check_body("implied </p>", "<p>one<p>two<div>three</div>",
		"    <p>\n      \"one\"\n    <p>\n      \"two\"\n    <div>\n      \"three\"\n");
	check_body("lists", "<ul><li>a<li>b</ul>after",
		"    <ul>\n      <li>\n        \"a\"\n      <li>\n        \"b\"\n    \"after\"\n");
	check_body("entities",
		"a&amp;b &lt; &copy &notin; &#x41;&#66; &bogus; &amp &#0; &#128;",
		"    \"a&b < \xC2\xA9 \xE2\x88\x89 AB &bogus; & \xEF\xBF\xBD \xE2\x82\xAC\"\n");
	check_body("legacy entity prefix", "&notit; &copyright",
		"    \"\xC2\xACit; \xC2\xA9right\"\n");
	check_body("script skipped",
		"<p>x<script>if (a<b) document.write('</p>')</script>y",
		"    <p>\n      \"x\"\n      <script>\n      \"y\"\n");
	check_body("attributes",
		"<a href=\"/x?a=1&amp;b=2&c\" class=foo title='t \"q\"' onclick=\"evil()\" "
		"HREF=dup>link</a>",
		"    <a href=\"/x?a=1&b=2&c\" class=\"foo\" title=\"t \"q\"\">\n      \"link\"\n");
	check_body("attribute entity rules", "<a href=\"?x=1&copy=2&lt;\">",
		"    <a href=\"?x=1&copy=2<\">\n");
	check_body("comments", "a<!-- c --> b<!---->c<!-->d<!--->e<!-- x --!>f<!DOCTYPE x>g",
		"    \"a bcdefg\"\n");
	check_body("table",
		"<table><tr><td>1<td>2<tr><td>3</table>",
		"    <table>\n      <tr>\n        <td>\n          \"1\"\n        <td>\n"
		"          \"2\"\n      <tr>\n        <td>\n          \"3\"\n");
	check_body("cell without row", "<table><td>x</table>y",
		"    <table>\n      <tr>\n        <td>\n          \"x\"\n    \"y\"\n");
	check("head", "<title>T  &amp; x</title><meta charset=utf-8><p>b",
		"<html>\n  <head>\n    <title>\n      \"T  & x\"\n    <meta charset=\"utf-8\">\n"
		"  <body>\n    <p>\n      \"b\"\n");
	check_body("pre", "<pre>\n  a\n b</pre>c",
		"    <pre>\n      \"  a\\n b\"\n    \"c\"\n");
	check_body("svg dropped", "x<svg><text>hide</text><svg></svg></svg>y<math><mi>z</mi></math>",
		"    \"x\"\n    <svg>\n    \"y\"\n    <math>\n");
	check_body("stray end tags", "<div>a</span>b</div></div>c",
		"    <div>\n      \"ab\"\n    \"c\"\n");
	check_body("nested links", "<a href=1>one<a href=2>two</a>",
		"    <a href=\"1\">\n      \"one\"\n    <a href=\"2\">\n      \"two\"\n");
	check_body("white space",
		"<p> a  <b> b </b>  c </p> <div>\n d\n</div>",
		"    <p>\n      \"a \"\n      <b>\n        \"b \"\n      \"c \"\n    <div>\n      \"d \"\n");
	check_body("br end tag", "a</br>b", "    \"a\"\n    <br>\n    \"b\"\n");
	check_body("textarea", "<textarea>\n<b>&lt;x</textarea>",
		"    <textarea>\n      \"<b><x\"\n");
	check_body("options", "<select><option>a<option>b</select>",
		"    <select>\n      <option>\n        \"a\"\n      <option>\n        \"b\"\n");
	check_body("unclosed tag at eof", "x<a href=\"y", "    \"x\"\n");
	check_body("lt in text", "1 < 2 <3 &", "    \"1 < 2 <3 &\"\n");
	check_body("image and void", "<img src=a.gif alt=A><image src=b><br/>x",
		"    <img src=\"a.gif\" alt=\"A\">\n    <img src=\"b\">\n    <br>\n    \"x\"\n");
	check_body("headings", "<h1>a<h2>b</h2>",
		"    <h1>\n      \"a\"\n    <h2>\n      \"b\"\n");
	check_body("noscript shown", "<noscript><p>no js</p></noscript>",
		"    <noscript>\n      <p>\n        \"no js\"\n");
	check_body("CRLF", "a\r\nb\rc", "    \"a b c\"\n");
	check("plain text", "", "<html>\n  <head>\n  <body>\n");
	{
		char *got = parse("a <b>\n&amp;", 11, 3, "", 1);
		const char *want = "<html>\n  <head>\n  <body>\n    <plaintext>\n"
			"      \"a <b>\\n&amp;\"\n";

		if (strcmp(got, want)) {
			printf("FAIL text/plain:\n%s", got);
			fails++;
		}
		free(got);
	}
	/* the cap: a document too big for it is cut short, not a crash */
	{
		static struct html_load l;
		struct doc d;
		int i;

		doc_init(&d, 8192);
		html_load_begin(&l, &d, "utf-8", 0);
		for (i = 0; i < 2000; i++)
			html_load_feed(&l, (const unsigned char *)"<p>paragraph text</p>", 21);
		html_load_end(&l);
		if (!d.truncated || doc_bytes(&d) > 8192) {
			printf("FAIL cap: truncated %d, %lu bytes\n", d.truncated,
				(unsigned long)doc_bytes(&d));
			fails++;
		}
		doc_free(&d);
	}
	/* depth: 1000 nested divs */
	{
		static char deep[8000];
		char *got;
		size_t i;

		for (i = 0; i < 1000; i++)
			memcpy(deep + i * 5, "<div>", 5);
		strcpy(deep + 5000, "x");
		got = parse(deep, strlen(deep), 0, "utf-8", 0);
		if (!strstr(got, "\"x\"")) {
			printf("FAIL deep nesting lost the text\n");
			fails++;
		}
		free(got);
	}
	printf("html: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
