/*
 * test_forms - form fields and their submissions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "doc.h"
#include "load.h"
#include "tags.h"
#include "forms.h"

static int fails, runs;
static struct doc d;
static struct forms fs;
static struct url base;

static void load(const char *html, const char *cs)
{
	static struct html_load l;

	if (fs.f)
		forms_free(&fs);
	doc_free(&d);
	doc_init(&d, 0);
	html_load_begin(&l, &d, cs, 0);
	html_load_feed(&l, (const unsigned char *)html, strlen(html));
	html_load_end(&l);
	forms_init(&fs, &d);
	url_parse("http://h.test/dir/page?old=1", &base);
}

/* the n-th field of type t */
static struct field *nth(int t, int n)
{
	int i;

	for (i = 0; i < fs.n; i++)
		if (fs.f[i].type == t && n-- == 0)
			return &fs.f[i];
	return NULL;
}

static void submit(const char *what, struct field *button, int win1252,
	const char *method, const char *url, const char *body)
{
	struct submission s;
	const char *why = "";
	nodeid form = button ? button->form : fs.f[0].form;
	int rc = forms_submit(&fs, form, button ? button->node : 0, &base,
		win1252, &s, &why);

	runs++;
	if (rc < 0 || strcmp(s.method, method) || strcmp(s.url, url)
		|| strcmp(s.body ? s.body : "", body)) {
		fails++;
		printf("FAIL %s: rc %d (%s)\n  %s %s\n  body \"%s\"\n", what, rc,
			why, s.method, s.url, s.body ? s.body : "");
	}
	xfree(s.body);
}

int main(void)
{
	struct field *f;

	load("<form action=/search><input name=q value='old'>"
		"<input type=submit name=go value=Go></form>", "utf-8");
	forms_set_text(nth(FT_TEXT, 0), "68030 & friends");
	submit("get", nth(FT_SUBMIT, 0), 0, "GET",
		"http://h.test/search?q=68030+%26+friends&go=Go", "");

	load("<form method=post action=login.cgi><input type=hidden name=t "
		"value=x><input name=u><input type=password name=p>"
		"<input type=checkbox name=keep><input type=checkbox name=c2 "
		"value=yes checked><input type=radio name=r value=a checked>"
		"<input type=radio name=r value=b><select name=s><option>One"
		"<option value=2>Two</select><textarea name=t2>line1\nline2"
		"</textarea><input name=dis disabled value=no>"
		"<input type=submit value=Log></form>", "utf-8");
	forms_set_text(nth(FT_TEXT, 0), "me");
	forms_set_text(nth(FT_PASSWORD, 0), "s3cr\xc3\xa9t");
	forms_click(&fs, nth(FT_CHECKBOX, 0));
	forms_click(&fs, nth(FT_RADIO, 1));
	{
		nodeid o[8];

		f = nth(FT_SELECT, 0);
		forms_options(&fs, f, o, 8);
		forms_choose(f, o[1]);
	}
	submit("post", nth(FT_SUBMIT, 0), 0, "POST", "http://h.test/dir/login.cgi",
		"t=x&u=me&p=s3cr%C3%A9t&keep=on&c2=yes&r=b&s=2&t2=line1%0D%0Aline2");
	runs++;
	if (nth(FT_RADIO, 0)->checked) {
		fails++;
		printf("FAIL radio group: the first is still checked\n");
	}
	forms_reset(&fs, nth(FT_TEXT, 0)->form);
	submit("reset", nth(FT_SUBMIT, 0), 0, "POST",
		"http://h.test/dir/login.cgi",
		"t=x&u=&p=&c2=yes&r=a&s=One&t2=line1%0D%0Aline2");

	/* a windows-1252 page gets its data back in windows-1252 */
	load("<form><input name=q></form>", "windows-1252");
	forms_set_text(nth(FT_TEXT, 0), "caf\xc3\xa9 \xe2\x82\xac \xe4\xb8\xad");
	submit("win1252", NULL, 1, "GET",
		"http://h.test/dir/page?q=caf%E9+%80+%26%2320013%3B", "");

	/* a form cut short by a table: its inputs still belong to it */
	load("<table><form action=/t><tr><td><input name=a value=1>"
		"</td></tr></form></table>", "utf-8");
	submit("form in table", NULL, 0, "GET", "http://h.test/t?a=1", "");

	/* multipart */
	load("<form method=post enctype=multipart/form-data action=/up>"
		"<input name=a value=1><input name='b\"c' value=2></form>", "utf-8");
	{
		struct submission s;
		const char *why;

		runs++;
		if (forms_submit(&fs, fs.f[0].form, 0, &base, 0, &s, &why) < 0
			|| strncmp(s.type, "multipart/form-data; boundary=", 30)
			|| !strstr(s.body, "name=\"a\"\r\n\r\n1\r\n")
			|| !strstr(s.body, "name=\"b%22c\"\r\n\r\n2\r\n")) {
			fails++;
			printf("FAIL multipart:\n%s\n", s.body ? s.body : "");
		}
		xfree(s.body);
	}
	/* javascript: actions are refused */
	load("<form action='javascript:go()'><input name=a></form>", "utf-8");
	{
		struct submission s;
		const char *why = NULL;

		runs++;
		if (forms_submit(&fs, fs.f[0].form, 0, &base, 0, &s, &why) == 0) {
			fails++;
			printf("FAIL javascript action accepted\n");
		}
	}
	/* image buttons send a click point */
	load("<form action=/i><input type=image name=pic alt=Go></form>", "utf-8");
	submit("image", nth(FT_IMAGE, 0), 0, "GET", "http://h.test/i?pic.x=0&pic.y=0", "");
	printf("forms: %d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
