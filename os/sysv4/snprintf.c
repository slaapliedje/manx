/*
 * snprintf.c - bounded snprintf/vsnprintf for SVR4.0, which has only the
 * unbounded forms.
 *
 * Formatted directly into the caller's buffer: the browser formats text
 * whose length comes from the network, so there is no scratch buffer to
 * overflow. Integer and string conversions only (d i u o x X c s p %), with
 * flags - 0 + space, width, precision (both may be *), and the h l z length
 * modifiers. Floating-point conversions are not supported and print '?'.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct out {
	char *buf;
	size_t n;		/* buffer size */
	size_t len;		/* characters produced so far */
};

static void put(struct out *o, char c)
{
	if (o->n > 0 && o->len < o->n - 1)
		o->buf[o->len] = c;
	o->len++;
}

static void pad(struct out *o, char c, int k)
{
	while (k-- > 0)
		put(o, c);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
	struct out o;
	char digits[24];

	o.buf = buf;
	o.n = n;
	o.len = 0;
	for (; *fmt; fmt++) {
		int left = 0, zero = 0, plus = 0, space = 0;
		int width = 0, prec = -1, lng = 0, shrt = 0;
		unsigned long u;
		const char *s;
		int k, neg, base, upper, slen;
		char sign;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-') left = 1;
			else if (*fmt == '0') zero = 1;
			else if (*fmt == '+') plus = 1;
			else if (*fmt == ' ') space = 1;
			else if (*fmt == '#') ;
			else break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) {
				left = 1;
				width = -width;
			}
			fmt++;
		} else
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
		}
		for (;; fmt++) {
			if (*fmt == 'l') lng = 1;
			else if (*fmt == 'z') lng = 1;	/* size_t is long-sized */
			else if (*fmt == 'h') shrt = 1;
			else break;
		}
		if (*fmt == '\0')
			break;

		switch (*fmt) {
		case '%':
			put(&o, '%');
			continue;
		case 'c':
			if (!left) pad(&o, ' ', width - 1);
			put(&o, (char)va_arg(ap, int));
			if (left) pad(&o, ' ', width - 1);
			continue;
		case 's':
			s = va_arg(ap, const char *);
			if (s == NULL)
				s = "(null)";
			for (slen = 0; s[slen] && (prec < 0 || slen < prec); slen++)
				;
			if (!left) pad(&o, ' ', width - slen);
			for (k = 0; k < slen; k++)
				put(&o, s[k]);
			if (left) pad(&o, ' ', width - slen);
			continue;
		case 'd': case 'i':
			if (lng) {
				long v = va_arg(ap, long);
				neg = v < 0;
				u = neg ? 0UL - (unsigned long)v : (unsigned long)v;
			} else {
				int v = va_arg(ap, int);
				if (shrt) v = (short)v;
				neg = v < 0;
				u = neg ? 0UL - (unsigned long)v : (unsigned long)v;
			}
			base = 10; upper = 0;
			break;
		case 'u': case 'o': case 'x': case 'X': case 'p':
			if (*fmt == 'p') {
				u = (unsigned long)va_arg(ap, void *);
				lng = 1;
			} else if (lng)
				u = va_arg(ap, unsigned long);
			else {
				u = va_arg(ap, unsigned int);
				if (shrt) u &= 0xFFFFUL;
			}
			neg = 0;
			base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
			upper = *fmt == 'X';
			break;
		default:	/* floating point and anything unknown */
			put(&o, '?');
			continue;
		}

		/* an integer: digits backwards into digits[] */
		k = 0;
		do {
			int d = (int)(u % base);
			digits[k++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
			u /= base;
		} while (u != 0);
		if (prec == 0 && k == 1 && digits[0] == '0')
			k = 0;
		sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
		if (*fmt == 'p') {
			sign = 0;
		}
		{
			int ndig = prec > k ? prec : k;
			int total = ndig + (sign != 0) + (*fmt == 'p' ? 2 : 0);

			if (prec >= 0)
				zero = 0;
			if (!left && !zero) pad(&o, ' ', width - total);
			if (sign) put(&o, sign);
			if (*fmt == 'p') { put(&o, '0'); put(&o, 'x'); }
			if (!left && zero) pad(&o, '0', width - total);
			pad(&o, '0', ndig - k);
			while (k > 0)
				put(&o, digits[--k]);
			if (left) pad(&o, ' ', width - total);
		}
	}
	if (n > 0)
		buf[o.len < n ? o.len : n - 1] = '\0';
	return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return len;
}
