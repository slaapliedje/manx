/*
 * doc.h - a parsed document, compact for small machines: nodes are 16
 * bytes and refer to each other by 16-bit index; text and attributes live
 * in two pools. Everything is freed at once when the page is left.
 */
#ifndef MANX_DOC_H
#define MANX_DOC_H

#include <stddef.h>

typedef unsigned short nodeid;		/* 0 = none */

enum { NODE_ELEM = 1, NODE_TEXT = 2, NODE_ROOT = 3 };

struct node {
	unsigned char type;
	unsigned char tag;		/* enum tag, for NODE_ELEM */
	nodeid parent, first, last, next;
	unsigned short pad;
	unsigned long data;		/* text or attribute pool offset */
};

#define DOC_MAX_NODES	65535U

struct css_sheet;

struct doc {
	struct node *nodes;		/* nodes[0] unused, nodes[1] the root */
	unsigned long nnodes, cap_nodes;
	char *text;			/* NUL-terminated UTF-8 runs */
	unsigned long text_len, text_cap;
	unsigned char *attr;		/* (id, value NUL)... 0 per element */
	unsigned long attr_len, attr_cap;
	size_t byte_cap;		/* all three together */
	int truncated;			/* hit a limit: the rest was dropped */
	nodeid last_text;		/* the text node that ends the text pool */
	nodeid res_parent;		/* doc_text_reserve's parent */
	int res_extend;			/* ... extending last_text */
	struct css_sheet *sheet;	/* its <style> rules (style/css.h), or NULL */
};

/* An empty document with its root node; byte_cap bounds the memory it
 * may take (0: DOC_DEFAULT_CAP). 0 or -1 (out of memory). */
#define DOC_DEFAULT_CAP	(1200UL * 1024)
int doc_init(struct doc *d, size_t byte_cap);
void doc_free(struct doc *d);

/* Add an element as parent's last child; 0 when the document is full.
 * Attributes are added right after, with doc_attr_add, then closed with
 * doc_attr_end. */
nodeid doc_add_elem(struct doc *d, nodeid parent, int tag);
void doc_attr_add(struct doc *d, int attr, const char *value);
void doc_attr_end(struct doc *d);

/* Append text to parent: joined to its last child if that is text and
 * still ends the pool. */
void doc_add_text(struct doc *d, nodeid parent, const char *s, size_t n);

/*
 * Write text straight into the pool (the parser's hot path): reserve room
 * for up to max bytes as parent's text, write them at the pointer, then
 * commit how many were written. NULL when the document is full.
 */
char *doc_text_reserve(struct doc *d, nodeid parent, size_t max);
void doc_text_commit(struct doc *d, size_t written);

/* reading */
#define DOC_NODE(d, id)	(&(d)->nodes[id])
const char *doc_text(const struct doc *d, nodeid id);
const char *doc_attr(const struct doc *d, nodeid id, int attr);
size_t doc_bytes(const struct doc *d);

/* The text of the first <title>, white space collapsed (static buffer). */
const char *doc_title(const struct doc *d);

/* An indented dump of the tree (to a FILE *), for tests and uparse. */
void doc_dump(const struct doc *d, void *file);

#endif /* MANX_DOC_H */
