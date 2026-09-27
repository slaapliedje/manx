/*
 * test_snprintf - os/sysv4/snprintf.c against the host C library, built
 * with its functions renamed (see the Makefile's test target).
 */
#include <stdio.h>
#include <string.h>
#include <limits.h>

int ub_snprintf(char *buf, size_t n, const char *fmt, ...);

static int fails;

#define T(...) do { \
	char a[256], b[256]; \
	int ra = snprintf(a, sizeof a, __VA_ARGS__); \
	int rb = ub_snprintf(b, sizeof b, __VA_ARGS__); \
	if (ra != rb || strcmp(a, b) != 0) { \
		printf("FAIL %s: libc [%s] %d, ours [%s] %d\n", #__VA_ARGS__, a, ra, b, rb); \
		fails++; \
	} \
} while (0)

int main(void)
{
	char small[8];
	int r;

	T("plain");
	T("%d %i %u", 42, -42, 42u);
	T("%5d|%-5d|%05d|%+d|% d", 42, 42, 42, 42, 42);
	T("%d %d", INT_MIN, INT_MAX);
	T("%ld %lu %lx", LONG_MIN, ULONG_MAX, 0xdeadbeefUL);
	T("%x %X %o %#x", 255u, 255u, 8u, 0u);
	T("%.3d|%.0d|%8.3d|%-8.3d|", 7, 0, 7, 7);
	T("%s|%10s|%-10s|%.2s|%*s|%-*s|%.*s", "abc", "abc", "abc", "abc", 6, "x", 6, "x", 2, "abcdef");
	T("%c%c%3c%-3c|", 'a', 'b', 'c', 'd');
	T("%hd %hu", 70000, 70000);
	T("%zu", (size_t)12345);
	T("100%%");
	/* truncation: the return value is the full length */
	r = ub_snprintf(small, sizeof small, "%s", "0123456789");
	if (r != 10 || strcmp(small, "0123456") != 0) {
		printf("FAIL truncation: [%s] %d\n", small, r);
		fails++;
	}
	r = ub_snprintf(NULL, 0, "%d", 12345);
	if (r != 5) {
		printf("FAIL size query: %d\n", r);
		fails++;
	}
	printf("snprintf: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
