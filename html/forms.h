/*
 * forms.h - the form fields of a document and what the user has done to
 * them, and the submission they make (HTML's form submission algorithm,
 * without scripts).
 */
#ifndef MANX_FORMS_H
#define MANX_FORMS_H

#include <stddef.h>
#include "doc.h"
#include "url.h"

enum {
	FT_TEXT, FT_PASSWORD, FT_HIDDEN, FT_CHECKBOX, FT_RADIO, FT_SUBMIT,
	FT_RESET, FT_BUTTON, FT_IMAGE, FT_FILE, FT_SELECT, FT_TEXTAREA
};

struct field {
	nodeid node;
	nodeid form;			/* its form, 0: none */
	unsigned char type;
	unsigned char checked;		/* checkbox, radio */
	nodeid selected;		/* select: the chosen <option> */
	char *value;			/* text-like fields: what's in them */
};

struct forms {
	const struct doc *d;
	struct field *f;
	int n;
};

/* Collect the fields of d, with their initial values. 0, or -1 (memory). */
int forms_init(struct forms *fs, const struct doc *d);
void forms_free(struct forms *fs);

struct field *forms_field(const struct forms *fs, nodeid node);

/* What a field shows: its text, or the chosen option's text. */
const char *forms_text(const struct forms *fs, const struct field *f);

void forms_set_text(struct field *f, const char *v);

/* A checkbox toggles; a radio button is chosen (the others of its group
 * are not). */
void forms_click(struct forms *fs, struct field *f);

/* the options of a select: up to max, and which is chosen */
int forms_options(const struct forms *fs, const struct field *f,
	nodeid *opts, int max);
void forms_choose(struct field *f, nodeid option);

/* Back to the page's values. */
void forms_reset(struct forms *fs, nodeid form);

/* The next text-like field of f's form after f, or NULL. */
struct field *forms_next_text(const struct forms *fs, const struct field *f);

/* Does the form have a submit button? */
int forms_has_submit(const struct forms *fs, nodeid form);

/*
 * The submission of form by submitter (a button's node, or 0): the
 * method ("GET" or "POST"), the URL to fetch (for GET, with the data as
 * its query), and for POST the body (malloc'd, xfree it) and its type.
 * win1252: the page came in windows-1252, so the data goes back in it.
 * 0, or -1 with *why set.
 */
struct submission {
	char method[8];
	char url[URL_MAX];
	char *body;
	size_t body_len;
	char type[80];
};

int forms_submit(const struct forms *fs, nodeid form, nodeid submitter,
	const struct url *base, int win1252, struct submission *out,
	const char **why);

#endif /* MANX_FORMS_H */
