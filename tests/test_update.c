/*
 * test_update - the updater's manifests (net/update.h): versions
 * compared, signatures checked (a test key's, made with openssl), and
 * manifests read for one platform.
 */
#include <stdio.h>
#include <string.h>
#include "os.h"
#include "update.h"

static int g_pass, g_fail;

static void ok(int cond, const char *what)
{
	if (cond)
		g_pass++;
	else {
		g_fail++;
		printf("FAIL: %s\n", what);
	}
}

/* openssl ecparam -name prime256v1 -genkey; its public point */
static const unsigned char test_pub[65] = {
	0x04, 0x6a, 0x31, 0x26, 0x13, 0x13, 0xc4, 0x5f, 0xc9, 0xad, 0xec, 0xb0,
	0x8b, 0x5f, 0xcd, 0xbc, 0x04, 0xe6, 0xb6, 0x04, 0x71, 0xc8, 0xf7, 0xd0,
	0xe2, 0x82, 0xae, 0xec, 0x3d, 0xf9, 0x8d, 0xf4, 0x0b, 0x37, 0x5a, 0xf8,
	0xbf, 0xa4, 0xf6, 0xb8, 0x7b, 0x87, 0xf3, 0x21, 0x79, 0xf9, 0xd6, 0x69,
	0xe1, 0xed, 0xaf, 0xcd, 0xb5, 0x88, 0x80, 0xba, 0xf0, 0x30, 0x67, 0xb1,
	0x7c, 0x5e, 0x80, 0x73, 0x6f
};

static const char manifest[] =
	"manx-update 1\n"
	"version 1.2.3\n"
	"date 2026-10-22\n"
	"file irix-mips manx 5 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e"
	"73043362938b9824 https://example.com/m.gz\n"
	"file sysv4-m68k manx 6 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa742"
	"5e73043362938b9824 https://example.com/s.gz\n"
	"note Line one.\n";

/* openssl dgst -sha256 -sign of manifest */
static const unsigned char manifest_sig[70] = {
	0x30, 0x44, 0x02, 0x21, 0x00, 0x95, 0xcc, 0xfe, 0xc8, 0x41, 0x9c, 0xf7,
	0x71, 0xc1, 0x49, 0x0f, 0xa1, 0xb2, 0xc5, 0x34, 0xf8, 0x36, 0x09, 0xb0,
	0xe5, 0xd9, 0x76, 0x22, 0x67, 0x69, 0xcf, 0x40, 0x53, 0x45, 0xf2, 0xdf,
	0xf8, 0x02, 0x1f, 0x04, 0xc1, 0xb1, 0xcd, 0x98, 0x17, 0xfc, 0xe0, 0xb6,
	0xfe, 0x30, 0x82, 0x8d, 0x50, 0x43, 0xdf, 0xce, 0xe5, 0x42, 0x6f, 0x50,
	0xe3, 0x13, 0xe0, 0xdc, 0x9c, 0xee, 0xa5, 0x8e, 0xd4, 0x5e
};

static void versions(void)
{
	ok(update_vercmp("0.9.0", "0.9.0") == 0, "same");
	ok(update_vercmp("0.10", "0.9.5") > 0, "0.10 > 0.9.5");
	ok(update_vercmp("0.9", "0.9.0") == 0, "0.9 = 0.9.0");
	ok(update_vercmp("0.9.0", "0.9.1") < 0, "0.9.0 < 0.9.1");
	ok(update_vercmp("1.0", "0.99.99") > 0, "1.0 > 0.99.99");
	ok(update_vercmp("1.0-rc1", "1.0") == 0, "suffix ignored");
}

static void signatures(void)
{
	char bad[sizeof manifest];
	unsigned char sig[sizeof manifest_sig];

	ok(update_verify(manifest, strlen(manifest), manifest_sig,
		sizeof manifest_sig, test_pub), "good signature");
	memcpy(bad, manifest, sizeof manifest);
	bad[24] = '4';		/* version 1.2.4 */
	ok(!update_verify(bad, strlen(bad), manifest_sig, sizeof manifest_sig,
		test_pub), "changed text");
	memcpy(sig, manifest_sig, sizeof sig);
	sig[10] ^= 1;
	ok(!update_verify(manifest, strlen(manifest), sig, sizeof sig,
		test_pub), "changed signature");
	ok(!update_verify(manifest, strlen(manifest), manifest_sig,
		sizeof manifest_sig, NULL), "not the release key's");
	ok(!update_verify(manifest, strlen(manifest), manifest_sig, 0,
		test_pub), "no signature");
}

static int parse(const char *text, struct update_info *u)
{
	char err[200];

	return update_parse(text, strlen(text), "irix-mips", u, err, sizeof err);
}

static void manifests(void)
{
	static struct update_info u;
	char err[200];

	ok(update_parse(manifest, strlen(manifest), "irix-mips", &u, err,
		sizeof err) == 0, "parses");
	ok(strcmp(u.version, "1.2.3") == 0, "version");
	ok(strcmp(u.date, "2026-10-22") == 0, "date");
	ok(strcmp(u.notes, "Line one.\n") == 0, "notes");
	ok(u.nfiles == 1, "only this platform's files");
	ok(strcmp(u.files[0].name, "manx") == 0 && u.files[0].size == 5
		&& strcmp(u.files[0].url, "https://example.com/m.gz") == 0,
		"file line");
	ok(u.files[0].sha256[0] == 0x2c && u.files[0].sha256[31] == 0x24,
		"checksum");
	ok(update_parse(manifest, strlen(manifest), "linux-x86_64", &u, err,
		sizeof err) == 0 && u.nfiles == 0, "no files for this one");
	ok(parse("manx-update 2\nversion 9\n", &u) < 0, "a later format refused");
	ok(parse("manx-update 1\r\nversion 2.0\r\n", &u) == 0
		&& strcmp(u.version, "2.0") == 0, "CRLF");
	ok(parse("manx-update 1\nfile irix-mips ../manx 5 2cf24dba5fb0a"
		"30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824 u\n"
		"version 2\n", &u) < 0,
		"a path in a name refused");
	ok(parse("manx-update 1\nfile irix-mips manx 5 2cf u\nversion 2\n", &u)
		< 0, "short checksum");
	ok(parse("manx-update 1\n", &u) < 0, "no version");
	ok(parse("manx-update 1\nversion 2\n", &u) == 0, "(the good line)");
}

int main(void)
{
	versions();
	signatures();
	manifests();
	printf("test_update: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail != 0;
}
