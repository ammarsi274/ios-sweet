/*
 * printf/scanf formatting for the Nokia's driver, reading arguments from an
 * AAPCS64 va_list (struct va64). Integers and strings are formatted here;
 * one floating-point conversion at a time is handed to the host's snprintf
 * (our code calls the host in the host's own convention, so that is safe).
 */
#include "bionic.h"

long va64_int(struct va64 *va)
{
	long v;
	if (va->gr_offs < 0) {
		v = *(long *)(va->gr_top + va->gr_offs);
		va->gr_offs += 8;
	} else {
		v = *(long *)va->stack;
		va->stack += 8;
	}
	return v;
}

double va64_double(struct va64 *va)
{
	double v;
	if (va->vr_offs < 0) {
		v = *(double *)(va->vr_top + va->vr_offs);
		va->vr_offs += 16;
	} else {
		v = *(double *)va->stack;
		va->stack += 8;
	}
	return v;
}

struct out {
	char *p;
	size_t n, len;
};

static void put(struct out *o, char c)
{
	if (o->len + 1 < o->n)
		o->p[o->len] = c;
	o->len++;
}

static void puts_n(struct out *o, const char *s, size_t n)
{
	while (n--)
		put(o, *s++);
}

static void pad(struct out *o, char c, int n)
{
	while (n-- > 0)
		put(o, c);
}

int b_vformat(char *buf, size_t n, const char *f, struct va64 *va)
{
	struct out o = { buf, n, 0 };
	while (*f) {
		if (*f != '%') {
			put(&o, *f++);
			continue;
		}
		const char *spec = f++;
		int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = -1, prec = -1, lng = 0;
		for (;; f++) {
			if (*f == '-') left = 1;
			else if (*f == '+') plus = 1;
			else if (*f == ' ') space = 1;
			else if (*f == '#') alt = 1;
			else if (*f == '0') zero = 1;
			else break;
		}
		if (*f == '*') {
			width = (int)va64_int(va);
			if (width < 0)
				left = 1, width = -width;
			f++;
		} else
			for (width = *f >= '0' && *f <= '9' ? 0 : -1; *f >= '0' && *f <= '9'; f++)
				width = width * 10 + (*f - '0');
		if (*f == '.') {
			f++;
			if (*f == '*') {
				prec = (int)va64_int(va);
				f++;
			} else
				for (prec = 0; *f >= '0' && *f <= '9'; f++)
					prec = prec * 10 + (*f - '0');
		}
		/* length: hh h l ll z j t L */
		for (;; f++) {
			if (*f == 'h') lng = lng == -1 ? -2 : -1;
			else if (*f == 'l') lng = lng == 1 ? 2 : 1;
			else if (*f == 'z' || *f == 'j' || *f == 't') lng = 2;
			else if (*f == 'L') lng = 3;
			else break;
		}
		char c = *f ? *f++ : 0;
		char tmp[80];
		switch (c) {
		case '%':
			put(&o, '%');
			break;
		case 'c': {
			char ch = (char)va64_int(va);
			if (!left) pad(&o, ' ', width - 1);
			put(&o, ch);
			if (left) pad(&o, ' ', width - 1);
			break;
		}
		case 's': {
			const char *s = (const char *)va64_int(va);
			if (!s)
				s = "(null)";
			size_t l = 0;
			while (s[l] && (prec < 0 || (int)l < prec))
				l++;
			if (!left) pad(&o, ' ', width - (int)l);
			puts_n(&o, s, l);
			if (left) pad(&o, ' ', width - (int)l);
			break;
		}
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
			unsigned long v;
			int neg = 0, base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
			long raw = va64_int(va);
			if (c == 'p') {
				v = (unsigned long)raw;
				alt = 1;
			} else if (c == 'd' || c == 'i') {
				long s = lng >= 1 ? raw : lng == -1 ? (short)raw : lng == -2 ? (signed char)raw : (int)raw;
				neg = s < 0;
				v = neg ? 0UL - (unsigned long)s : (unsigned long)s;
			} else
				v = lng >= 1 ? (unsigned long)raw : lng == -1 ? (unsigned short)raw :
				    lng == -2 ? (unsigned char)raw : (unsigned int)raw;
			int l = 0;
			const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
			do {
				tmp[l++] = dig[v % base];
				v /= base;
			} while (v);
			while (l < prec)
				tmp[l++] = '0';
			if (prec == 0 && l == 1 && tmp[0] == '0' && c != 'p')
				l = 0;
			const char *pre = neg ? "-" : plus && (c == 'd' || c == 'i') ? "+" : space && (c == 'd' || c == 'i') ? " " : "";
			const char *px = alt && base == 16 && (raw || c == 'p') ? (c == 'X' ? "0X" : "0x") : alt && base == 8 ? "0" : "";
			int total = l + (int)strlen(pre) + (int)strlen(px);
			if (!left && !(zero && prec < 0)) pad(&o, ' ', width - total);
			puts_n(&o, pre, strlen(pre));
			puts_n(&o, px, strlen(px));
			if (!left && zero && prec < 0) pad(&o, '0', width - total);
			while (l)
				put(&o, tmp[--l]);
			if (left) pad(&o, ' ', width - total);
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
			/* one conversion for the host, rebuilt from the parsed fields
			 * (no length modifier: long double is not used by the driver) */
			double v = va64_double(va);
			char fmt2[40], *q = fmt2;
			*q++ = '%';
			if (left) *q++ = '-';
			if (plus) *q++ = '+';
			if (space) *q++ = ' ';
			if (alt) *q++ = '#';
			if (zero) *q++ = '0';
			*q = 0;
			if (width >= 0)
				q += snprintf(q, 12, "%d", width > 60 ? 60 : width);
			if (prec >= 0)
				q += snprintf(q, 12, ".%d", prec > 40 ? 40 : prec);
			*q++ = c;
			*q = 0;
			int l = snprintf(tmp, sizeof(tmp), fmt2, v);
			puts_n(&o, tmp, l < (int)sizeof(tmp) ? (size_t)l : sizeof(tmp) - 1);
			break;
		}
		case 'n':
			va64_int(va);	/* never written: not used by the driver, and a classic hazard */
			break;
		default:	/* unknown: print it as is */
			puts_n(&o, spec, (size_t)(f - spec));
			break;
		}
	}
	if (o.n)
		o.p[o.len < o.n ? o.len : o.n - 1] = 0;
	return (int)o.len;
}

/* sscanf subset: whitespace, literals, %d %i %u %x %o %s %c %ld %lu %lld %llu %lx %f %lf %n %% and '*' */
static int isspc(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

int b_vscan(const char *s, const char *f, struct va64 *va)
{
	const char *start = s;
	int got = 0;
	while (*f) {
		if (isspc(*f)) {
			while (isspc(*s)) s++;
			f++;
			continue;
		}
		if (*f != '%') {
			if (*s != *f)
				return got;
			s++, f++;
			continue;
		}
		f++;
		int skip = 0, width = 0, lng = 0;
		if (*f == '*') skip = 1, f++;
		while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
		while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'L') lng += *f == 'l' || *f == 'z' || *f == 'L', f++;
		char c = *f++;
		if (c == '%') {
			while (isspc(*s)) s++;
			if (*s != '%') return got;
			s++;
			continue;
		}
		if (c == 'n') {
			if (!skip) *(int *)va64_int(va) = (int)(s - start);
			continue;
		}
		if (c != 'c')
			while (isspc(*s)) s++;
		if (!*s)
			return got ? got : -1;
		if (c == 's' || c == 'c') {
			char *d = skip ? 0 : (char *)va64_int(va);
			int n = 0, max = width ? width : c == 'c' ? 1 : 0x7fffffff;
			while (*s && n < max && (c == 'c' || !isspc(*s))) {
				if (d) d[n] = *s;
				n++, s++;
			}
			if (d && c == 's') d[n] = 0;
			got += !skip;
			continue;
		}
		if (c == 'f' || c == 'e' || c == 'g') {
			/* digits, sign, point, exponent */
			char num[64];
			int n = 0;
			while (n < 63 && ((*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.' || *s == 'e' || *s == 'E'))
				num[n++] = *s++;
			num[n] = 0;
			if (!n) return got;
			extern double strtod(const char *, char **);
			double v = strtod(num, 0);
			if (!skip) {
				if (lng) *(double *)va64_int(va) = v;
				else *(float *)va64_int(va) = (float)v;
				got++;
			}
			continue;
		}
		int base = c == 'x' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10, neg = 0;
		if (*s == '-' || *s == '+') neg = *s++ == '-';
		if ((base == 16 || base == 0) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2, base = 16;
		else if (base == 0) base = s[0] == '0' ? 8 : 10;
		unsigned long v = 0;
		int digits = 0;
		for (;; s++, digits++) {
			int d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : 99;
			if (d >= base || (width && digits >= width)) break;
			v = v * base + d;
		}
		if (!digits) return got;
		if (neg) v = 0UL - v;
		if (!skip) {
			void *d = (void *)va64_int(va);
			if (lng >= 1) *(unsigned long *)d = v;
			else *(unsigned int *)d = (unsigned int)v;
			got++;
		}
	}
	return got;
}
