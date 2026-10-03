/*
 * load.h - bytes of an HTML document in, a struct doc out, as the bytes
 * arrive: charset detection, decoding to UTF-8, tokenizing, tree building.
 */
#ifndef MANX_LOAD_H
#define MANX_LOAD_H

#include <stddef.h>
#include "utf8.h"
#include "doc.h"
#include "tokenizer.h"
#include "tree.h"

#define LOAD_SNIFF	1024	/* bytes looked at for <meta charset> */

enum { CS_FROM_DEFAULT, CS_FROM_HTTP, CS_FROM_BOM, CS_FROM_META, CS_FROM_GUESS };

struct html_load {
	struct doc *d;
	struct tree tree;
	struct tokenizer tok;
	struct decoder dec;
	enum charset cs;
	int cs_from;
	int sniffing;
	unsigned char sniff[LOAD_SNIFF];
	size_t sniff_len;
	int plain;			/* text/plain: all one <pre> */
	unsigned long bytes_in;
};

/*
 * Start loading into d (already doc_init'ed). http_charset: the charset
 * parameter of Content-Type, or NULL/"". plain_text: render the body as
 * preformatted text instead of parsing it as HTML.
 */
/* Read pages' <style> sheets (1, the default), for the layout to hide
 * what they hide; 0 saves the time (on a 68030, ~3 us a byte of CSS). */
extern int html_stylesheets;

void html_load_begin(struct html_load *l, struct doc *d,
	const char *http_charset, int plain_text);
void html_load_feed(struct html_load *l, const unsigned char *s, size_t n);
void html_load_end(struct html_load *l);

/* charset detection alone (for tests): from the first bytes */
enum charset html_sniff(const unsigned char *s, size_t n, int *from,
	size_t *bom_len);

#endif /* MANX_LOAD_H */
