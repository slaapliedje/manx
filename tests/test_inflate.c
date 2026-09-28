/*
 * test_inflate - gzip and zlib streams of real pages (made by
 * tests/gen_deflate.py at build time) decoded whole and in random
 * pieces; corrupt and truncated streams must fail cleanly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os.h"
#include "inflate.h"

static unsigned char *outbuf;
static size_t outlen, outcap;

static int sink(void *ctx, const unsigned char *d, size_t n)
{
	(void)ctx;
	if (outlen + n > outcap) {
		outcap = (outlen + n) * 2;
		outbuf = realloc(outbuf, outcap);
	}
	memcpy(outbuf + outlen, d, n);
	outlen += n;
	return 0;
}

static unsigned char *slurp(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	long len;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	rewind(f);
	b = malloc((size_t)len + 1);
	*n = fread(b, 1, (size_t)len, f);
	fclose(f);
	return b;
}

/* decode s[0..n) fed in pieces of up to maxpiece (0: whole) */
static int decode(int fmt, const unsigned char *s, size_t n, size_t maxpiece)
{
	struct inflate *z = inflate_new(fmt, sink, NULL);
	size_t off = 0;
	int rc = INF_OK;

	outlen = 0;
	while (off < n && rc == INF_OK) {
		size_t k = maxpiece ? 1 + (size_t)rand() % maxpiece : n - off;

		if (k > n - off)
			k = n - off;
		rc = inflate_feed(z, s + off, k);
		off += k;
	}
	if (rc == INF_OK)
		rc = inflate_finish(z);
	inflate_free(z);
	return rc;
}

int main(int argc, char **argv)
{
	int fails = 0, runs = 0, i, a;

	srand(1);
	for (a = 1; a + 2 < argc; a += 3) {
		size_t nplain, ngz, nz;
		unsigned char *plain = slurp(argv[a], &nplain);
		unsigned char *gz = slurp(argv[a + 1], &ngz);
		unsigned char *zl = slurp(argv[a + 2], &nz);
		static const size_t pieces[] = { 0, 1, 7, 100, 4096 };

		if (!plain || !gz || !zl) {
			printf("FAIL reading %s\n", argv[a]);
			return 1;
		}
		for (i = 0; i < 5; i++) {
			int rc;

			runs += 2;
			rc = decode(INF_GZIP, gz, ngz, pieces[i]);
			if (rc != INF_END || outlen != nplain
				|| memcmp(outbuf, plain, nplain)) {
				fails++;
				printf("FAIL gzip %s piece %lu: rc %d, %lu of %lu bytes\n",
					argv[a], (unsigned long)pieces[i], rc,
					(unsigned long)outlen, (unsigned long)nplain);
			}
			rc = decode(INF_DEFLATE, zl, nz, pieces[i]);
			if (rc != INF_END || outlen != nplain
				|| memcmp(outbuf, plain, nplain)) {
				fails++;
				printf("FAIL zlib %s piece %lu: rc %d\n", argv[a],
					(unsigned long)pieces[i], rc);
			}
		}
		/* truncated anywhere: never INF_END */
		for (i = 0; i < 20; i++) {
			size_t cut = (size_t)rand() % ngz;

			runs++;
			if (decode(INF_GZIP, gz, cut, 100) == INF_END) {
				fails++;
				printf("FAIL truncated at %lu decoded\n",
					(unsigned long)cut);
			}
		}
		/* corrupted: must end (any result) without crashing; a flip
		 * in the data is caught by the CRC */
		for (i = 0; i < 200; i++) {
			unsigned char *c = malloc(ngz);
			size_t at = 10 + (size_t)rand() % (ngz - 18);

			memcpy(c, gz, ngz);
			c[at] ^= (unsigned char)(1 + rand() % 255);
			runs++;
			if (decode(INF_GZIP, c, ngz, 500) == INF_END
				&& (outlen != nplain || memcmp(outbuf, plain, nplain))) {
				fails++;
				printf("FAIL corrupt byte %lu accepted\n",
					(unsigned long)at);
			}
			free(c);
		}
		free(plain);
		free(gz);
		free(zl);
	}
	printf("inflate: %d/%d passed\n", runs - fails, runs);
	return fails != 0;
}
