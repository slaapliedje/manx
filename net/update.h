/*
 * update.h - Manx updating itself from its releases.
 *
 * Each release carries a manifest, manx-update.txt, and its signature,
 * manx-update.sig: ECDSA P-256 over the manifest's SHA-256, by the
 * release key, whose public half is built in (update_key.h). The
 * signature is what's trusted, not the connection: a 68030's certificate
 * store may be years old, and its checks come after the request.
 *
 *   manx-update 1
 *   version 0.9.1
 *   date 2026-10-22
 *   file irix-mips manx 412345 <sha256 hex> https://.../manx-irix-mips.gz
 *   note what's new, a line each (any number)
 *
 * A file line is one program of one platform: its name, its size and
 * SHA-256 as installed, and where it is, gzipped. Installing writes each
 * of this platform's programs found beside the running one (manx, xmanx,
 * ...) next to it, checks it, then keeps the old as NAME.old and renames
 * the new into place. A package's copy (inst, tdnf: PACKAGED= at build
 * time) is never replaced: Manx says how to update it instead.
 */
#ifndef MANX_UPDATE_H
#define MANX_UPDATE_H

#include <stddef.h>
#include "url.h"

/* where the newest release's manifest is (setting update_url) */
#define UPDATE_URL "https://github.com/slaapliedje/manx/releases/latest/download/manx-update.txt"
#define UPDATE_PAGE "https://github.com/slaapliedje/manx/releases/latest"

#define UPDATE_FILES	8
#define UPDATE_NOTES	1500

struct update_file {
	char name[32];
	long size;
	unsigned char sha256[32];
	char url[URL_MAX];
};

struct update_info {
	char version[32];
	char date[32];
	char notes[UPDATE_NOTES];	/* the note lines, '\n' after each */
	int nfiles;			/* this platform's */
	struct update_file files[UPDATE_FILES];
};

/* This build: its version ("0.9.0"), platform ("irix-mips"), and the
 * package system it was built for ("" when none). */
const char *update_version(void);
const char *update_platform(void);
const char *update_packaged(void);

/* <0, 0, >0 as version a is older than, the same as, newer than b
 * (dotted numbers: 0.10 > 0.9). */
int update_vercmp(const char *a, const char *b);

/* 1 when sig (DER) is pub's (an uncompressed P-256 point, 65 bytes)
 * signature of text; the release key's when pub is NULL. */
int update_verify(const void *text, size_t len, const void *sig,
	size_t siglen, const unsigned char *pub);

/* Read a manifest, keeping platform's files. 0, or -1 with err set. */
int update_parse(const char *text, size_t len, const char *platform,
	struct update_info *u, char *err, size_t errn);

/* Fetch and check the newest manifest (url: NULL for the setting, else
 * UPDATE_URL). 1: newer than this build; 0: not; -1: err says why.
 * status (may be NULL) hears the progress. */
int update_check(const char *url, struct update_info *u, char *err,
	size_t errn, void (*status)(void *ctx, const char *msg), void *ctx);

/* Where the running program is, from argv[0] (and $PATH), links
 * followed: call once at start. */
void update_init(const char *argv0);
const char *update_self(void);

/* Whether this copy can replace itself: 1, or 0 with why set (a
 * package's copy, a directory the user can't write). */
int update_can_install(char *why, size_t n);

/* Download, check and install u's files for the running program's
 * directory. 0 with msg saying what was done, or -1 with msg saying
 * why not (nothing replaced). */
int update_install(const struct update_info *u, char *msg, size_t n,
	void (*status)(void *ctx, const char *msg), void *ctx);

/*
 * The daily check, in a child process so nothing waits on it (setting
 * update_check = off: never; mogrix's copy only with update_check = on).
 * update_bg_start forks it when the last one is a day old; update_bg_poll
 * returns 1, once, when it has finished and found a newer version. update_known gives the newest version a check
 * found (from $MANX_HOME/update), "" when none newer is known.
 */
void update_bg_start(void);
int update_bg_poll(void);
const char *update_known(void);

#endif /* MANX_UPDATE_H */
