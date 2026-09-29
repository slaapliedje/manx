/*
 * tree.h - build a document from tokens: the tolerant core of HTML5 tree
 * construction (implied html/head/body, implied end tags, scopes, void
 * elements), without the parts that cost more than they give on a 68030
 * (the adoption agency algorithm, foster parenting, template contents).
 */
#ifndef MANX_TREE_H
#define MANX_TREE_H

#include "doc.h"
#include "tokenizer.h"

#define TREE_MAX_DEPTH	160
#define TAG_COUNT_MAX	256		/* tags are unsigned char */

struct tree {
	struct doc *d;
	nodeid html, head, body;
	int head_done;			/* past </head> or body content */
	nodeid stack[TREE_MAX_DEPTH];
	unsigned char stag[TREE_MAX_DEPTH];
	int depth;
	unsigned short nopen[TAG_COUNT_MAX];	/* open elements per tag */
	int overflow;			/* elements past TREE_MAX_DEPTH */
	int skip_tag, skip_nest;	/* inside a dropped subtree */
	int pre_newline;		/* just opened <pre>/<textarea>/<listing> */
	int line_start;			/* last text ended a line or with a space */
	int in_pre;
	char base_href[512];		/* the first <base href> */
	char meta_charset[40];		/* <meta charset> / http-equiv */
};

void tree_init(struct tree *b, struct doc *d);

/* the tokenizer's sink functions */
void tree_tag(void *ctx, const struct tok_tag *t);
void tree_text(void *ctx, const char *s, size_t n);

/* End of input: nothing more to close explicitly. */
void tree_end(struct tree *b);

#endif /* MANX_TREE_H */
