/*
 * tokenizer.c - see tokenizer.h. One byte at a time through a state
 * machine; the state survives between chunks, so input may break
 * anywhere (tests/test_html.c checks that chunking never changes the
 * result).
 */
#include <string.h>
#include "utf8.h"
#include "tags.h"
#include "tokenizer.h"

struct entity {
	const char *name;
	unsigned short cp;
	unsigned char legacy;		/* also valid without ';' */
};
#include "entities_tab.h"

enum {
	S_DATA, S_TAG_OPEN, S_END_TAG_OPEN, S_TAG_NAME, S_BEFORE_ATTR_NAME,
	S_ATTR_NAME, S_AFTER_ATTR_NAME, S_BEFORE_ATTR_VALUE, S_ATTR_VALUE_DQ,
	S_ATTR_VALUE_SQ, S_ATTR_VALUE_UQ, S_AFTER_ATTR_VALUE_Q, S_SELF_CLOSING,
	S_MARKUP, S_MARKUP_DASH, S_COMMENT, S_BOGUS_COMMENT,
	S_RAW, S_RAW_LT, S_RAW_END, S_PLAINTEXT,
	S_CHARREF, S_CHARREF_NUM, S_CHARREF_NAMED
};

enum { RAW_NONE, RAW_SKIP, RAW_KEEP, RAW_RCDATA };

/* windows-1252 meanings of &#128; .. &#159; (HTML5) */
static const unsigned short c1_fix[32] = {
	0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
	0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178
};

static int is_ws(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\f';
}

static int is_alpha(int c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_alnum(int c)
{
	return is_alpha(c) || (c >= '0' && c <= '9');
}

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

void tok_init(struct tokenizer *t, const struct tok_sink *sink)
{
	memset(t, 0, sizeof *t);
	t->sink = *sink;
	t->state = S_DATA;
	t->acur = -1;
}

/* --- output ----------------------------------------------------------- */

static void flush_text(struct tokenizer *t)
{
	if (t->text_len && t->sink.text)
		t->sink.text(t->sink.ctx, t->text, t->text_len);
	t->text_len = 0;
}

static void text_put(struct tokenizer *t, const char *s, size_t n)
{
	if (t->raw == RAW_SKIP && (t->state == S_RAW || t->state == S_RAW_LT
		|| t->state == S_RAW_END)) {
		/* script and style: never page text; a style sheet may be
		 * wanted */
		if (t->raw_tag == TAG_STYLE && t->sink.style)
			t->sink.style(t->sink.ctx, s, n);
		return;
	}
	while (n) {
		size_t k = sizeof t->text - t->text_len;

		if (k == 0) {
			flush_text(t);
			continue;
		}
		if (k > n)
			k = n;
		memcpy(t->text + t->text_len, s, k);
		t->text_len += k;
		s += k;
		n -= k;
	}
}

static void text_char(struct tokenizer *t, int c)
{
	char ch = (char)c;

	text_put(t, &ch, 1);
}

/* attribute value bytes */
static void aval_put(struct tokenizer *t, const char *s, size_t n)
{
	if (t->acur < 0)
		return;
	if (t->abuf_len + n + 1 > sizeof t->abuf)
		n = sizeof t->abuf - 1 - t->abuf_len;	/* truncated */
	memcpy(t->abuf + t->abuf_len, s, n);
	t->abuf_len += n;
}

/* a character reference's result: into the value or the text */
static void ref_out(struct tokenizer *t, const char *s, size_t n)
{
	if (t->in_attr)
		aval_put(t, s, n);
	else
		text_put(t, s, n);
}

/* --- tags ------------------------------------------------------------- */

static void tag_begin(struct tokenizer *t, int end)
{
	flush_text(t);
	t->name_len = 0;
	/* not a memset of the whole record: the attribute arrays are only
	 * read up to nattr (and every store costs on the TT) */
	t->t.tag = TAG_UNKNOWN;
	t->t.self_closing = 0;
	t->t.nattr = 0;
	t->t.end = end;
	t->abuf_len = 0;
	t->acur = -1;
}

static void name_char(struct tokenizer *t, int c)
{
	if (t->name_len < sizeof t->name - 1)
		t->name[t->name_len++] = (char)lower(c);
	else
		t->name_len = sizeof t->name;	/* too long: unknown */
}

static void attr_begin(struct tokenizer *t)
{
	t->aname_len = 0;
	t->acur = -1;
}

static void attr_name_char(struct tokenizer *t, int c)
{
	if (t->aname_len < sizeof t->aname - 1)
		t->aname[t->aname_len++] = (char)lower(c);
	else
		t->aname_len = sizeof t->aname;
}

/* the name is complete: keep it (with an empty value so far) or not */
static void attr_named(struct tokenizer *t)
{
	int id = ATTR_NONE, i;

	t->acur = -1;
	if (t->aname_len < sizeof t->aname) {
		t->aname[t->aname_len] = '\0';
		id = attr_lookup(t->aname);
	}
	if (id == ATTR_NONE || t->t.end || t->t.nattr == TOK_MAX_ATTRS)
		return;
	for (i = 0; i < t->t.nattr; i++)
		if (t->t.attr[i] == id)
			return;		/* a duplicate: the first one wins */
	if (t->abuf_len + 1 >= sizeof t->abuf)
		return;
	t->acur = t->t.nattr++;
	t->t.attr[t->acur] = id;
	t->t.value[t->acur] = t->abuf + t->abuf_len;
}

static void attr_done(struct tokenizer *t)
{
	if (t->acur >= 0)
		t->abuf[t->abuf_len++] = '\0';
	t->acur = -1;
}

static void tag_emit(struct tokenizer *t)
{
	unsigned f;

	if (t->name_len < sizeof t->name) {
		t->name[t->name_len] = '\0';
		t->t.tag = tag_lookup(t->name);
	} else
		t->t.tag = TAG_UNKNOWN;
	if (t->t.tag == TAG_IMAGE)
		t->t.tag = TAG_IMG;		/* HTML5 does this too */
	if (t->sink.tag)
		t->sink.tag(t->sink.ctx, &t->t);
	t->state = S_DATA;
	if (t->t.end)
		return;
	f = tag_flags(t->t.tag);
	if (t->t.tag == TAG_PLAINTEXT)
		t->state = S_PLAINTEXT;
	else if (f & (TF_SKIPTEXT | TF_RAWTEXT | TF_RCDATA)) {
		t->raw = f & TF_SKIPTEXT ? RAW_SKIP : f & TF_RAWTEXT ? RAW_KEEP
			: RAW_RCDATA;
		t->raw_tag = t->t.tag;
		t->state = S_RAW;
	}
}

/* --- character references ---------------------------------------------- */

static const struct entity *entity_find(const char *name, size_t n)
{
	int lo = 0, hi = N_ENTITIES;

	while (lo < hi) {
		int mid = (lo + hi) / 2;
		int c = strncmp(name, entities[mid].name, n);

		if (c == 0 && entities[mid].name[n] != '\0')
			c = -1;
		if (c == 0)
			return &entities[mid];
		if (c > 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

unsigned long tok_entity(const char *name, int semicolon)
{
	const struct entity *e = entity_find(name, strlen(name));

	if (e == NULL || (!semicolon && !e->legacy))
		return 0;
	return e->cp;
}

static void charref_begin(struct tokenizer *t, int ret, int in_attr)
{
	t->ret_state = ret;
	t->in_attr = in_attr;
	t->ref_len = 0;
	t->state = S_CHARREF;
}

static void emit_cp(struct tokenizer *t, unsigned long cp)
{
	char buf[4];

	ref_out(t, buf, (size_t)utf8_put(buf, cp));
}

/* numeric reference complete (ref holds "#123" or "#x1F") */
static void numeric_done(struct tokenizer *t, int semicolon)
{
	unsigned long v = 0;
	size_t i = 1;
	int hex = 0, digits = 0;

	if (t->ref_len > 1 && (t->ref[1] == 'x' || t->ref[1] == 'X')) {
		hex = 1;
		i = 2;
	}
	for (; i < t->ref_len; i++) {
		int c = t->ref[i], d = c >= '0' && c <= '9' ? c - '0'
			: lower(c) - 'a' + 10;

		if (v < 0x110000UL)
			v = v * (hex ? 16 : 10) + (unsigned long)d;
		digits++;
	}
	(void)semicolon;
	if (digits == 0) {
		/* "&#" or "&#x" without digits: literal */
		ref_out(t, "&", 1);
		ref_out(t, t->ref, t->ref_len);
		return;
	}
	if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
		v = UCS_REPLACEMENT;
	else if (v >= 0x80 && v <= 0x9F)
		v = c1_fix[v - 0x80];
	emit_cp(t, v);
}

/* named reference, terminated by c (';' consumed by the caller) */
static void named_done(struct tokenizer *t, int semicolon, int next)
{
	const struct entity *e;
	size_t n;

	if (semicolon && (e = entity_find(t->ref, t->ref_len)) != NULL) {
		emit_cp(t, e->cp);
		return;
	}
	/* no ';': the longest legacy name that is a prefix */
	for (n = t->ref_len; n > 0; n--) {
		e = entity_find(t->ref, n);
		if (e && e->legacy) {
			/* in attributes, "&amp=" / "&notit" stay literal */
			if (t->in_attr && (n < t->ref_len || next == '='))
				break;
			emit_cp(t, e->cp);
			ref_out(t, t->ref + n, t->ref_len - n);
			if (semicolon)
				ref_out(t, ";", 1);
			return;
		}
	}
	ref_out(t, "&", 1);
	ref_out(t, t->ref, t->ref_len);
	if (semicolon)
		ref_out(t, ";", 1);
}

/* --- the state machine ------------------------------------------------ */

static void raw_end_mismatch(struct tokenizer *t)
{
	text_put(t, "</", 2);
	text_put(t, t->endbuf, t->endlen);
	t->state = S_RAW;
}

static void step(struct tokenizer *t, int c)
{
	switch (t->state) {
	case S_DATA:
		if (c == '<')
			t->state = S_TAG_OPEN;
		else if (c == '&')
			charref_begin(t, S_DATA, 0);
		else
			text_char(t, c);
		break;

	case S_TAG_OPEN:
		if (c == '!')
			t->state = S_MARKUP;
		else if (c == '/')
			t->state = S_END_TAG_OPEN;
		else if (is_alpha(c)) {
			tag_begin(t, 0);
			name_char(t, c);
			t->state = S_TAG_NAME;
		} else if (c == '?') {
			flush_text(t);
			t->state = S_BOGUS_COMMENT;
		} else {
			text_char(t, '<');
			t->state = S_DATA;
			t->reconsume = 1;
		}
		break;

	case S_END_TAG_OPEN:
		if (is_alpha(c)) {
			tag_begin(t, 1);
			name_char(t, c);
			t->state = S_TAG_NAME;
		} else if (c == '>')
			t->state = S_DATA;
		else {
			flush_text(t);
			t->state = S_BOGUS_COMMENT;
		}
		break;

	case S_TAG_NAME:
		if (is_ws(c))
			t->state = S_BEFORE_ATTR_NAME;
		else if (c == '/')
			t->state = S_SELF_CLOSING;
		else if (c == '>')
			tag_emit(t);
		else
			name_char(t, c);
		break;

	case S_BEFORE_ATTR_NAME:
		if (is_ws(c))
			break;
		if (c == '/')
			t->state = S_SELF_CLOSING;
		else if (c == '>')
			tag_emit(t);
		else {
			attr_begin(t);
			attr_name_char(t, c);
			t->state = S_ATTR_NAME;
		}
		break;

	case S_ATTR_NAME:
		if (is_ws(c)) {
			attr_named(t);
			t->state = S_AFTER_ATTR_NAME;
		} else if (c == '/') {
			attr_named(t);
			attr_done(t);
			t->state = S_SELF_CLOSING;
		} else if (c == '=') {
			attr_named(t);
			t->state = S_BEFORE_ATTR_VALUE;
		} else if (c == '>') {
			attr_named(t);
			attr_done(t);
			tag_emit(t);
		} else
			attr_name_char(t, c);
		break;

	case S_AFTER_ATTR_NAME:
		if (is_ws(c))
			break;
		if (c == '/') {
			attr_done(t);
			t->state = S_SELF_CLOSING;
		} else if (c == '=')
			t->state = S_BEFORE_ATTR_VALUE;
		else if (c == '>') {
			attr_done(t);
			tag_emit(t);
		} else {
			attr_done(t);
			attr_begin(t);
			attr_name_char(t, c);
			t->state = S_ATTR_NAME;
		}
		break;

	case S_BEFORE_ATTR_VALUE:
		if (is_ws(c))
			break;
		if (c == '"')
			t->state = S_ATTR_VALUE_DQ;
		else if (c == '\'')
			t->state = S_ATTR_VALUE_SQ;
		else if (c == '>') {
			attr_done(t);
			tag_emit(t);
		} else {
			t->state = S_ATTR_VALUE_UQ;
			t->reconsume = 1;
		}
		break;

	case S_ATTR_VALUE_DQ:
	case S_ATTR_VALUE_SQ:
		if (c == (t->state == S_ATTR_VALUE_DQ ? '"' : '\'')) {
			attr_done(t);
			t->state = S_AFTER_ATTR_VALUE_Q;
		} else if (c == '&')
			charref_begin(t, t->state, 1);
		else {
			char ch = (char)c;

			aval_put(t, &ch, 1);
		}
		break;

	case S_ATTR_VALUE_UQ:
		if (is_ws(c)) {
			attr_done(t);
			t->state = S_BEFORE_ATTR_NAME;
		} else if (c == '&')
			charref_begin(t, S_ATTR_VALUE_UQ, 1);
		else if (c == '>') {
			attr_done(t);
			tag_emit(t);
		} else {
			char ch = (char)c;

			aval_put(t, &ch, 1);
		}
		break;

	case S_AFTER_ATTR_VALUE_Q:
		if (is_ws(c))
			t->state = S_BEFORE_ATTR_NAME;
		else if (c == '/')
			t->state = S_SELF_CLOSING;
		else if (c == '>')
			tag_emit(t);
		else {
			t->state = S_BEFORE_ATTR_NAME;
			t->reconsume = 1;
		}
		break;

	case S_SELF_CLOSING:
		if (c == '>') {
			t->t.self_closing = 1;
			tag_emit(t);
		} else {
			t->state = S_BEFORE_ATTR_NAME;
			t->reconsume = 1;
		}
		break;

	case S_MARKUP:
		flush_text(t);
		if (c == '-')
			t->state = S_MARKUP_DASH;
		else {
			/* <!DOCTYPE ...>, <![CDATA[...]]>, <!foo>: ignored */
			t->state = S_BOGUS_COMMENT;
			t->reconsume = 1;
		}
		break;

	case S_MARKUP_DASH:
		if (c == '-') {
			t->state = S_COMMENT;
			t->dashes = 0;
			t->bang = 0;
			t->endlen = 0;		/* characters of comment seen */
		} else {
			t->state = S_BOGUS_COMMENT;
			t->reconsume = 1;
		}
		break;

	case S_COMMENT:
		/* ends at "-->", "--!>", and the odd "<!-->" / "<!--->" */
		if (c == '>' && (t->dashes >= 2 || (t->bang && t->dashes == 0 && t->endlen > 3)
			|| (size_t)t->dashes == t->endlen)) {
			t->state = S_DATA;
			break;
		}
		if (c == '-') {
			if (t->bang)
				t->bang = 0;
			t->dashes++;
		} else if (c == '!' && t->dashes >= 2) {
			t->bang = 1;
			t->dashes = 0;
		} else {
			t->dashes = 0;
			t->bang = 0;
		}
		if (t->endlen < 8)
			t->endlen++;
		break;

	case S_BOGUS_COMMENT:
		if (c == '>')
			t->state = S_DATA;
		break;

	case S_RAW:
		if (c == '<')
			t->state = S_RAW_LT;
		else if (c == '&' && t->raw == RAW_RCDATA)
			charref_begin(t, S_RAW, 0);
		else
			text_char(t, c);
		break;

	case S_RAW_LT:
		if (c == '/') {
			t->state = S_RAW_END;
			t->endlen = 0;
		} else {
			text_char(t, '<');
			t->state = S_RAW;
			t->reconsume = 1;
		}
		break;

	case S_RAW_END:
		if (is_alpha(c) && t->endlen < sizeof t->endbuf) {
			t->endbuf[t->endlen++] = (char)c;
			break;
		}
		{
			const char *want = tag_name(t->raw_tag);
			size_t i, wl = strlen(want);
			int match = t->endlen == wl
				&& (is_ws(c) || c == '/' || c == '>');

			for (i = 0; match && i < wl; i++)
				if (lower(t->endbuf[i]) != want[i])
					match = 0;
			if (!match) {
				raw_end_mismatch(t);
				t->reconsume = 1;
				break;
			}
		}
		/* the end tag: read the rest of it like any tag */
		t->raw = RAW_NONE;
		tag_begin(t, 1);
		memcpy(t->name, tag_name(t->raw_tag), strlen(tag_name(t->raw_tag)));
		t->name_len = strlen(tag_name(t->raw_tag));
		t->state = S_TAG_NAME;
		t->reconsume = 1;
		break;

	case S_PLAINTEXT:
		text_char(t, c);
		break;

	case S_CHARREF:
		if (c == '#') {
			t->ref[t->ref_len++] = '#';
			t->state = S_CHARREF_NUM;
		} else if (is_alnum(c)) {
			t->state = S_CHARREF_NAMED;
			t->reconsume = 1;
		} else {
			ref_out(t, "&", 1);
			t->state = t->ret_state;
			t->reconsume = 1;
		}
		break;

	case S_CHARREF_NUM:
		if (t->ref_len == 1 && (c == 'x' || c == 'X')) {
			t->ref[t->ref_len++] = (char)c;
			break;
		}
		{
			int hex = t->ref_len > 1 && (t->ref[1] == 'x' || t->ref[1] == 'X');
			int ok = hex ? (c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'f')
				: c >= '0' && c <= '9';

			if (ok && t->ref_len < sizeof t->ref - 1) {
				t->ref[t->ref_len++] = (char)c;
				break;
			}
			if (ok)
				break;		/* absurdly long: digits dropped */
			numeric_done(t, c == ';');
			t->state = t->ret_state;
			if (c != ';')
				t->reconsume = 1;
		}
		break;

	case S_CHARREF_NAMED:
		if (is_alnum(c) && t->ref_len < 32) {
			t->ref[t->ref_len++] = (char)c;
			break;
		}
		if (is_alnum(c)) {
			/* longer than any entity: literal */
			ref_out(t, "&", 1);
			ref_out(t, t->ref, t->ref_len);
			t->state = t->ret_state;
			t->reconsume = 1;
			break;
		}
		named_done(t, c == ';', c);
		t->state = t->ret_state;
		if (c != ';')
			t->reconsume = 1;
		break;
	}
}

/*
 * Fast paths. Most bytes are ordinary: text, script bodies being skipped,
 * attribute values, comments. Per state, a table marks the bytes that
 * need the state machine; runs of the others are handled in one go (on
 * a 68030 the byte-at-a-time path parsed ~16 KB/s, slower than the
 * network).
 */
enum { ST_DATA, ST_RAW, ST_RCDATA, ST_DQ, ST_SQ, ST_COMMENT, ST_BOGUS,
	ST_PLAIN, ST_TAGNAME, ST_ATTRNAME, ST_UQ, ST_SPACE, ST_COUNT };
static unsigned char s_stop[ST_COUNT][256];

static void stop_tables(void)
{
	static int done;
	static const char *const stops[ST_SPACE] = {
		"<&\r", "<\r", "<&\r", "\"&\r", "'&\r", "->!\r", ">\r", "\r",
		" \t\n\f/>\r", " \t\n\f/=>\r", " \t\n\f&>\r"
	};
	int k;

	if (done)
		return;
	done = 1;
	for (k = 0; k < ST_SPACE; k++) {
		const char *p;

		for (p = stops[k]; *p; p++)
			s_stop[k][(unsigned char)*p] = 1;
		/* NUL never comes (the decoder replaced it); stop anyway */
		s_stop[k][0] = 1;
	}
	/* white space runs: everything else stops */
	for (k = 0; k < 256; k++)
		s_stop[ST_SPACE][k] = !(k == ' ' || k == '\t' || k == '\n' || k == '\f');
}

/* the table for the current state, or NULL: no fast path here */
static const unsigned char *fast_table(const struct tokenizer *t)
{
	switch (t->state) {
	case S_DATA: return s_stop[ST_DATA];
	case S_RAW: return s_stop[t->raw == RAW_RCDATA ? ST_RCDATA : ST_RAW];
	case S_ATTR_VALUE_DQ: return s_stop[ST_DQ];
	case S_ATTR_VALUE_SQ: return s_stop[ST_SQ];
	case S_COMMENT: return s_stop[ST_COMMENT];
	case S_BOGUS_COMMENT: return s_stop[ST_BOGUS];
	case S_PLAINTEXT: return s_stop[ST_PLAIN];
	case S_TAG_NAME: return s_stop[ST_TAGNAME];
	case S_ATTR_NAME: return s_stop[ST_ATTRNAME];
	case S_ATTR_VALUE_UQ: return s_stop[ST_UQ];
	case S_BEFORE_ATTR_NAME:
	case S_AFTER_ATTR_NAME:
	case S_BEFORE_ATTR_VALUE:
		return s_stop[ST_SPACE];
	}
	return NULL;
}

/* a run of ordinary bytes in the current state */
static void fast_run(struct tokenizer *t, const char *s, size_t n)
{
	switch (t->state) {
	case S_DATA:
	case S_PLAINTEXT:
		text_put(t, s, n);
		break;
	case S_RAW:
		if (t->raw != RAW_SKIP || (t->raw_tag == TAG_STYLE && t->sink.style))
			text_put(t, s, n);
		break;
	case S_ATTR_VALUE_DQ:
	case S_ATTR_VALUE_SQ:
	case S_ATTR_VALUE_UQ:
		aval_put(t, s, n);
		break;
	case S_TAG_NAME: {
		size_t i;

		for (i = 0; i < n; i++)
			name_char(t, s[i]);
		break;
	}
	case S_ATTR_NAME: {
		size_t i;

		for (i = 0; i < n; i++)
			attr_name_char(t, s[i]);
		break;
	}
	case S_BEFORE_ATTR_NAME:
	case S_AFTER_ATTR_NAME:
	case S_BEFORE_ATTR_VALUE:
		break;			/* white space: nothing to do */
	case S_COMMENT:
		/* as step() would: no dashes, no bang, count the length */
		t->dashes = 0;
		t->bang = 0;
		t->endlen = t->endlen + n > 8 ? 8 : t->endlen + n;
		break;
	}
}

void tok_feed(struct tokenizer *t, const char *s, size_t n)
{
	size_t i = 0;

	stop_tables();
	while (i < n) {
		int c;

		if (!t->prev_cr) {
			const unsigned char *stop = fast_table(t);

			if (stop && !stop[(unsigned char)s[i]]) {
				size_t j = i + 1;

				while (j < n && !stop[(unsigned char)s[j]])
					j++;
				fast_run(t, s + i, j - i);
				i = j;
				continue;
			}
		}
		c = (unsigned char)s[i++];

		/* CR LF and lone CR become LF */
		if (c == '\r') {
			t->prev_cr = 1;
			c = '\n';
		} else if (c == '\n' && t->prev_cr) {
			t->prev_cr = 0;
			continue;
		} else
			t->prev_cr = 0;
		do {
			t->reconsume = 0;
			step(t, c);
		} while (t->reconsume);
	}
}

void tok_end(struct tokenizer *t)
{
	switch (t->state) {
	case S_TAG_OPEN:
		text_char(t, '<');
		break;
	case S_END_TAG_OPEN:
		text_put(t, "</", 2);
		break;
	case S_RAW_LT:
		text_char(t, '<');
		break;
	case S_RAW_END:
		raw_end_mismatch(t);
		break;
	case S_CHARREF:
		ref_out(t, "&", 1);
		break;
	case S_CHARREF_NUM:
		numeric_done(t, 0);
		break;
	case S_CHARREF_NAMED:
		named_done(t, 0, -1);
		break;
	}
	/* a tag cut off by the end of input is dropped (as HTML5 does) */
	flush_text(t);
	t->state = S_DATA;
}
