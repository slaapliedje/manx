/*
 * tokenizer.h - the HTML tokenizer: UTF-8 in (already decoded), tags and
 * text out, fed in chunks of any size. Follows the WHATWG tokenizer where
 * it matters to rendering (tags, attributes, comments, character
 * references, raw text and RCDATA elements), simplified elsewhere.
 * <script> and <style> bodies are skipped as they stream past.
 */
#ifndef MANX_TOKENIZER_H
#define MANX_TOKENIZER_H

#include <stddef.h>

#define TOK_MAX_ATTRS	24
#define TOK_NAME_MAX	32
#define TOK_ABUF	8192	/* all attribute values of one tag */
#define TOK_TEXT_BUF	1024

struct tok_tag {
	int tag;			/* enum tag */
	int end;			/* end tag */
	int self_closing;
	int nattr;
	int attr[TOK_MAX_ATTRS];	/* enum attr: only kept attributes */
	const char *value[TOK_MAX_ATTRS];
};

struct tok_sink {
	void *ctx;
	void (*tag)(void *ctx, const struct tok_tag *t);
	void (*text)(void *ctx, const char *s, size_t n);
	/* the text of a <style> element (never page text); may be NULL */
	void (*style)(void *ctx, const char *s, size_t n);
};

struct tokenizer {
	struct tok_sink sink;
	int state, raw, raw_tag;
	int reconsume;
	int prev_cr;
	/* pending text */
	char text[TOK_TEXT_BUF];
	size_t text_len;
	/* the tag being read */
	char name[TOK_NAME_MAX];
	size_t name_len;
	struct tok_tag t;
	char aname[TOK_NAME_MAX];
	size_t aname_len;
	int acur;			/* attribute being read: index, or -1 dropped */
	char abuf[TOK_ABUF];
	size_t abuf_len;
	/* comments, markup declarations, raw-text end tags */
	int dashes, bang;
	char endbuf[TOK_NAME_MAX];
	size_t endlen;
	/* character references */
	int ret_state, in_attr;
	char ref[40];
	size_t ref_len;
};

void tok_init(struct tokenizer *t, const struct tok_sink *sink);
void tok_feed(struct tokenizer *t, const char *s, size_t n);
void tok_end(struct tokenizer *t);

/* Decode a character reference name ("amp", "#x41", ...) the way the
 * tokenizer does: the code point, or 0 if unknown (for tests). */
unsigned long tok_entity(const char *name, int semicolon);

#endif /* MANX_TOKENIZER_H */
