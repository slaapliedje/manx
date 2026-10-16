/*
 * test_xdefer - a recorded chain put in the validator's order (each
 * certificate followed by its issuer), whatever order it was sent in.
 * The certificates are made here, as far as their subject: only the
 * names are looked at.
 */
#include <stdio.h>
#include <string.h>
#include "xdefer.h"

static int fails;
static struct xdefer g_xd;

/* a DER element: tag, length, content */
static size_t tlv(unsigned char *out, int tag, const unsigned char *c, size_t n)
{
	size_t h = 0;

	out[h++] = (unsigned char)tag;
	if (n < 128)
		out[h++] = (unsigned char)n;
	else {
		out[h++] = 0x82;
		out[h++] = (unsigned char)(n >> 8);
		out[h++] = (unsigned char)n;
	}
	memmove(out + h, c, n);
	return h + n;
}

/* Name: SEQUENCE { SET { SEQUENCE { OID cn, UTF8String } } } */
static size_t name(unsigned char *out, const char *cn)
{
	static const unsigned char oid_cn[] = { 0x06, 0x03, 0x55, 0x04, 0x03 };
	unsigned char a[300], b[300];
	size_t n;

	memcpy(a, oid_cn, sizeof oid_cn);
	n = sizeof oid_cn + tlv(a + sizeof oid_cn, 0x0c, (const unsigned char *)cn,
		strlen(cn));
	n = tlv(b, 0x30, a, n);
	n = tlv(a, 0x31, b, n);
	return tlv(out, 0x30, a, n);
}

/* a certificate from issuer to subject, onto the chain */
static void add(const char *issuer, const char *subject)
{
	static const unsigned char head[] = {
		0xa0, 0x03, 0x02, 0x01, 0x02,		/* [0] version 3 */
		0x02, 0x01, 0x01,			/* serial 1 */
		0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
		0x01, 0x01, 0x0b, 0x05, 0x00 };		/* sha256WithRSA */
	static const unsigned char validity[] = { 0x30, 0x1e,
		0x17, 0x0d, '2', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z',
		0x17, 0x0d, '3', '6', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z' };
	unsigned char tbs[1000], t[1000], c[1000];
	size_t n = 0;

	memcpy(tbs, head, sizeof head);
	n = sizeof head;
	n += name(tbs + n, issuer);
	memcpy(tbs + n, validity, sizeof validity);
	n += sizeof validity;
	n += name(tbs + n, subject);
	n = tlv(t, 0x30, tbs, n);
	n = tlv(c, 0x30, t, n);
	g_xd.cert_off[g_xd.ncert] = g_xd.len;
	g_xd.cert_len[g_xd.ncert] = n;
	memcpy(g_xd.chain + g_xd.len, c, n);
	g_xd.len += n;
	g_xd.ncert++;
}

static void start(void)
{
	memset(&g_xd, 0, sizeof g_xd);
}

/* after xdefer_order, the certificates must be the ones sent at want[] */
static void check(const char *what, const int *want, int n)
{
	size_t sent[XDEFER_CERTS_MAX];
	int i;

	for (i = 0; i < g_xd.ncert; i++)
		sent[i] = g_xd.cert_off[i];
	xdefer_order(&g_xd);
	for (i = 0; i < n; i++)
		if (g_xd.cert_off[i] != sent[want[i]]) {
			printf("FAIL %s: position %d\n", what, i);
			fails++;
			return;
		}
}

int main(void)
{
	{
		static const int want[] = { 0, 1, 2 };

		start();
		add("I1", "leaf");
		add("I2", "I1");
		add("R", "I2");
		check("in order", want, 3);
	}
	{
		/* www.gutenberg.org's: leaf, root, intermediate's issuer,
		 * intermediate */
		static const int want[] = { 0, 3, 2, 1 };

		start();
		add("NetSol CA 3", "*.gutenberg.org");
		add("AAA", "AAA");
		add("AAA", "USERTrust");
		add("USERTrust", "NetSol CA 3");
		check("gutenberg", want, 4);
	}
	{
		/* one that isn't on the path goes last */
		static const int want[] = { 0, 2, 1 };

		start();
		add("I1", "leaf");
		add("X", "Y");
		add("R", "I1");
		check("stranger", want, 3);
	}
	{
		/* issuers round in a circle: it ends */
		static const int want[] = { 0, 1, 2 };

		start();
		add("I1", "leaf");
		add("I2", "I1");
		add("I1", "I2");
		check("circle", want, 3);
	}
	{
		/* one that can't be read: all as sent */
		static const int want[] = { 0, 1, 2 };

		start();
		add("I1", "leaf");
		add("R", "X");
		add("X", "I1");
		g_xd.chain[g_xd.cert_off[1]] = 0x31;
		check("unreadable", want, 3);
	}
	printf("xdefer: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
