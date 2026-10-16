/*
 * sites.h - settings for particular sites, from a file of lines
 *
 *   HOST SETTING VALUE
 *
 * HOST is a host name, or .DOMAIN for that domain and every host under
 * it; # starts a comment line. The settings:
 *
 *   agent   the User-Agent sent to it, in place of Manx's own
 *   cookie  NAME=VALUE, put in the cookie jar for it (for the run)
 *
 * Both together carry another browser's session to a site that ties it
 * to no more than that - not Cloudflare's pass (cf_clearance), which is
 * tied to the browser's TLS handshake too. A cookie is as good as a
 * password: keep the file to yourself.
 */
#ifndef MANX_SITES_H
#define MANX_SITES_H

#define SITES_MAX	32

/* Read the file (none is fine); its cookies go into the jar, so after
 * cookie_init. */
void sites_load(const char *path);

/* The User-Agent to send to host, or NULL: Manx's own. */
const char *sites_agent(const char *host);

/* For tests: does host fall under a sites file's HOST pattern? */
int sites_match(const char *pattern, const char *host);

#endif /* MANX_SITES_H */
