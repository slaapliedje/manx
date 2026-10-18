/*
 * frames.h - a page of frames as one page. Each frame's document is read
 * and written out again as HTML: its links absolute (and, if they had no
 * target, aimed at their own frame), its body in a table cell; rows of a
 * frameset stacked, columns side by side, framesets in framesets nested.
 * The style sheets of all the frames go in the one page's head.
 */
#ifndef MANX_FRAMES_H
#define MANX_FRAMES_H

#include <stddef.h>
#include "doc.h"
#include "url.h"

#define FRAMES_MAX	16		/* frames read for one page */
#define FRAMES_DEPTH	4		/* framesets in frames in ... */

/* a frame's document, as frames_get delivers it */
struct frame_bytes {
	unsigned char *data;		/* xmalloc'd; frames_compose frees it */
	size_t len;
	char url[URL_MAX];		/* where it came from, after redirects */
	char charset[32];		/* the HTTP charset, or "" */
	char error[200];		/* why there is none */
};

/* get url's document into out: 0, or -1 with out->error set */
typedef int (*frames_get)(void *ctx, const char *url, struct frame_bytes *out);

/*
 * The page d (whose tree.frameset is fs, from page_url and relative to
 * base) as one page of HTML (xmalloc'd, NUL-terminated; NULL when out of
 * memory). Which document each frame shows: its src, unless chosen has a
 * line "name\turl" for it. *state (xmalloc'd) gets those lines for every
 * named frame, as shown.
 */
char *frames_compose(const struct doc *d, nodeid fs, const struct url *base,
	const char *chosen, frames_get get, void *ctx, char **state);

/* chosen with frame name's document changed to url (xmalloc'd) */
char *frames_choose(const char *chosen, const char *name, const char *url);

/* is name one of state's frames? */
int frames_has(const char *state, const char *name);

#endif /* MANX_FRAMES_H */
