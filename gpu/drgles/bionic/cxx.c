/*
 * The C++ runtime pieces the Nokia's shader compiler (libllvm-glnext.so)
 * imports from Android's libc++, written in C on libc++'s ABI.
 *
 * std::string layout, read from the compiler's own inlined code (24 bytes):
 *   byte 0 bit 0 clear: short string, size = byte0 >> 1, chars at +1 (22 max)
 *   bit 0 set: long string, +0 allocation size | 1, +8 size, +16 data
 * Allocation sizes follow libc++'s __recommend (multiples of 16), memory
 * comes from our operator new (the inlined destructor calls operator delete).
 *
 * Measured use (stage 1 bench): new/delete, 8 basic_string members, guards.
 * The stream and locale entries are never called (0 calls); they stop with
 * a message instead of guessing at libc++'s stream internals.
 */
#include "bionic.h"

extern void *malloc(size_t);
extern void free(void *);

/* ---- operator new / delete ---- */
void *b_new(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		b_fatal("operator new: out of memory");
	return p;
}

void *b_new_nothrow(size_t n, const void *tag)
{
	return malloc(n ? n : 1);
}

void b_delete(void *p)
{
	free(p);
}

char b_std_nothrow;

void b___cxa_pure_virtual(void)
{
	b_fatal("pure virtual call in the driver");
}

/* ---- static-local guards (64-bit guard, byte 0 = done) ---- */
int b_pthread_mutex_lock(void *);
int b_pthread_mutex_unlock(void *);
static uint32_t guard_lock[10] = { 1u << 14 };	/* recursive: nested static inits */

int b___cxa_guard_acquire(uint64_t *g)
{
	if (__atomic_load_n((uint8_t *)g, __ATOMIC_ACQUIRE))
		return 0;
	b_pthread_mutex_lock(guard_lock);
	if (*(uint8_t *)g) {
		b_pthread_mutex_unlock(guard_lock);
		return 0;
	}
	return 1;
}

void b___cxa_guard_release(uint64_t *g)
{
	__atomic_store_n((uint8_t *)g, 1, __ATOMIC_RELEASE);
	b_pthread_mutex_unlock(guard_lock);
}

void b___cxa_guard_abort(uint64_t *g)
{
	b_pthread_mutex_unlock(guard_lock);
}

/* ---- std::__1::basic_string<char> ---- */
struct lstr {
	union {
		struct {
			size_t cap;
			size_t size;
			char *data;
		} l;
		struct {
			unsigned char size;
			char data[23];
		} s;
	};
};
#define NPOS ((size_t)-1)
#define SHORT_CAP 22

static int is_long(const struct lstr *s) { return s->s.size & 1; }
static size_t str_size(const struct lstr *s) { return is_long(s) ? s->l.size : s->s.size >> 1; }
static char *str_data(struct lstr *s) { return is_long(s) ? s->l.data : s->s.data; }
static const char *cdata(const struct lstr *s) { return is_long(s) ? s->l.data : s->s.data; }
static size_t str_cap(const struct lstr *s) { return is_long(s) ? (s->l.cap & ~(size_t)1) - 1 : SHORT_CAP; }

static void set_size(struct lstr *s, size_t n)
{
	if (is_long(s))
		s->l.size = n;
	else
		s->s.size = (unsigned char)(n << 1);
	str_data(s)[n] = 0;
}

static size_t recommend(size_t n)
{
	return n <= SHORT_CAP ? SHORT_CAP : ((n + 16) & ~(size_t)15) - 1;
}

__attribute__((noreturn)) static void out_of_range(void)
{
	b_fatal("std::string: index out of range");
}

static void init(struct lstr *s, const char *p, size_t n)
{
	if (n <= SHORT_CAP) {
		s->s.size = (unsigned char)(n << 1);
		memcpy(s->s.data, p, n);
		s->s.data[n] = 0;
		return;
	}
	size_t cap = recommend(n);
	char *d = b_new(cap + 1);
	memcpy(d, p, n);
	d[n] = 0;
	s->l.cap = (cap + 1) | 1;
	s->l.size = n;
	s->l.data = d;
}

/* libc++ __grow_by: new buffer, keep [0,n_copy), drop n_del, leave n_add
 * room, keep the rest; does not set the size */
void b_str_grow_by(struct lstr *s, size_t old_cap, size_t delta, size_t old_sz, size_t n_copy, size_t n_del, size_t n_add)
{
	char *old = str_data(s);
	size_t want = old_cap + delta > 2 * old_cap ? old_cap + delta : 2 * old_cap;
	size_t cap = recommend(want);
	char *p = b_new(cap + 1);
	if (n_copy)
		memcpy(p, old, n_copy);
	size_t rest = old_sz - n_del - n_copy;
	if (rest)
		memcpy(p + n_copy + n_add, old + n_copy + n_del, rest);
	if (old_cap != SHORT_CAP)
		b_delete(old);
	s->l.data = p;
	s->l.cap = (cap + 1) | 1;
}

/* replace the contents by [prefix of old][src n][suffix of old], growing */
static void splice(struct lstr *s, size_t pos, size_t del, const char *src, size_t n)
{
	size_t sz = str_size(s), cap = str_cap(s), nsz = sz - del + n;
	char *cur = str_data(s);
	if (n && src >= cur && src < cur + sz + 1) {
		/* the source is inside this string (s.assign(s.c_str() + k), ...):
		 * take a copy first, the move below may overwrite it */
		char *tmp = b_new(n);
		memcpy(tmp, src, n);
		splice(s, pos, del, tmp, n);
		b_delete(tmp);
		return;
	}
	if (nsz <= cap) {
		memmove(cur + pos + n, cur + pos + del, sz - pos - del);
		memcpy(cur + pos, src, n);
		set_size(s, nsz);
		return;
	}
	size_t ncap = recommend(nsz > 2 * cap ? nsz : 2 * cap);
	char *p = b_new(ncap + 1), *old = str_data(s);
	memcpy(p, old, pos);
	memcpy(p + pos, src, n);
	memcpy(p + pos + n, old + pos + del, sz - pos - del);
	if (is_long(s))
		b_delete(old);
	s->l.data = p;
	s->l.cap = (ncap + 1) | 1;
	s->l.size = nsz;
	p[nsz] = 0;
}

void b_str_ctor_copy(struct lstr *d, const struct lstr *s)
{
	if (!is_long(s))
		*d = *s;
	else
		init(d, s->l.data, s->l.size);
}

void b_str_ctor_sub(struct lstr *d, const struct lstr *s, size_t pos, size_t n, const void *alloc)
{
	size_t sz = str_size(s);
	if (pos > sz)
		out_of_range();
	if (n > sz - pos)
		n = sz - pos;
	init(d, cdata(s) + pos, n);
}

void b_str_dtor(struct lstr *s)
{
	if (is_long(s))
		b_delete(s->l.data);
}

struct lstr *b_str_assign_n(struct lstr *s, const char *p, size_t n)
{
	splice(s, 0, str_size(s), p, n);
	return s;
}

struct lstr *b_str_assign(struct lstr *s, const char *p)
{
	return b_str_assign_n(s, p, strlen(p));
}

struct lstr *b_str_operator_eq(struct lstr *d, const struct lstr *s)
{
	if (d != s)
		b_str_assign_n(d, cdata(s), str_size(s));
	return d;
}

struct lstr *b_str_append_n(struct lstr *s, const char *p, size_t n)
{
	if (n)
		splice(s, str_size(s), 0, p, n);
	return s;
}

struct lstr *b_str_append(struct lstr *s, const char *p)
{
	return b_str_append_n(s, p, strlen(p));
}

struct lstr *b_str_insert(struct lstr *s, size_t pos, const char *p)
{
	if (pos > str_size(s))
		out_of_range();
	splice(s, pos, 0, p, strlen(p));
	return s;
}

char *b_str_insert_char(struct lstr *s, const char *it, char c)
{
	size_t ip = (size_t)(it - str_data(s));
	splice(s, ip, 0, &c, 1);
	return str_data(s) + ip;
}

struct lstr *b_str_erase(struct lstr *s, size_t pos, size_t n)
{
	size_t sz = str_size(s);
	if (pos > sz)
		out_of_range();
	if (n > sz - pos)
		n = sz - pos;
	if (n)
		splice(s, pos, n, "", 0);
	return s;
}

void b_str_reserve(struct lstr *s, size_t want);

void b_str_resize(struct lstr *s, size_t n, char c)
{
	size_t sz = str_size(s);
	if (n > str_cap(s))
		b_str_reserve(s, n);
	if (n > sz)
		memset(str_data(s) + sz, c, n - sz);
	set_size(s, n);
}

void b_str_reserve(struct lstr *s, size_t want)
{
	size_t sz = str_size(s), cap = str_cap(s);
	if (want < sz)
		want = sz;
	want = recommend(want);
	if (want == cap)
		return;
	if (want == SHORT_CAP) {	/* long -> short */
		char *old = s->l.data;
		s->s.size = (unsigned char)(sz << 1);
		memcpy(s->s.data, old, sz + 1);
		b_delete(old);
		return;
	}
	char *p = b_new(want + 1), *old = str_data(s);
	memcpy(p, old, sz + 1);
	if (is_long(s))
		b_delete(old);
	s->l.data = p;
	s->l.cap = (want + 1) | 1;
	s->l.size = sz;
}

void b_str_push_back(struct lstr *s, char c)
{
	size_t sz = str_size(s), cap = str_cap(s);
	if (sz == cap) {
		b_str_grow_by(s, cap, 1, sz, sz, 0, 0);
		s->l.size = sz;	/* now long */
	}
	char *p = str_data(s);
	p[sz] = c;
	if (is_long(s))
		s->l.size = sz + 1;
	else
		s->s.size = (unsigned char)((sz + 1) << 1);
	p[sz + 1] = 0;
}

size_t b_str_copy(const struct lstr *s, char *d, size_t n, size_t pos)
{
	size_t sz = str_size(s);
	if (pos > sz)
		out_of_range();
	if (n > sz - pos)
		n = sz - pos;
	memcpy(d, cdata(s) + pos, n);
	return n;
}

size_t b_str_find_char(const struct lstr *s, char c, size_t pos)
{
	size_t sz = str_size(s);
	const char *p = cdata(s);
	for (size_t i = pos; i < sz; i++)
		if (p[i] == c)
			return i;
	return NPOS;
}

size_t b_str_rfind_char(const struct lstr *s, char c, size_t pos)
{
	size_t sz = str_size(s);
	const char *p = cdata(s);
	if (!sz)
		return NPOS;
	size_t i = pos < sz ? pos + 1 : sz;
	while (i--)
		if (p[i] == c)
			return i;
	return NPOS;
}

int b_str_compare_n(const struct lstr *s, size_t pos, size_t n1, const char *p, size_t n2)
{
	size_t sz = str_size(s);
	if (pos > sz)
		out_of_range();
	size_t rlen = n1 < sz - pos ? n1 : sz - pos;
	int r = memcmp(cdata(s) + pos, p, rlen < n2 ? rlen : n2);
	if (!r)
		r = rlen < n2 ? -1 : rlen > n2 ? 1 : 0;
	return r;
}

int b_str_compare(const struct lstr *s, size_t pos, size_t n1, const char *p)
{
	return b_str_compare_n(s, pos, n1, p, strlen(p));
}

struct lstr b_str_plus(const char *l, const struct lstr *r)
{
	struct lstr out;
	size_t ln = strlen(l), rn = str_size(r);
	init(&out, l, ln);
	b_str_reserve(&out, ln + rn);
	b_str_append_n(&out, cdata(r), rn);
	return out;
}

/* ---- std::__shared_weak_count: vptr, shared owners, weak owners.
 * vtable: [0] D1, [1] D0, [2] __on_zero_shared, [3] __get_deleter,
 * [4] __on_zero_shared_weak (Itanium order of libc++'s declarations) ---- */
struct swc {
	void (**vt)(void *);
	long shared, weak;
};

const void *b_swc_get_deleter(const struct swc *c, const void *ti)
{
	return 0;
}

void b_swc_release_weak(struct swc *c)
{
	if (__atomic_load_n(&c->weak, __ATOMIC_ACQUIRE) == 0 || __atomic_sub_fetch(&c->weak, 1, __ATOMIC_ACQ_REL) == -1)
		c->vt[4](c);
}

void b_swc_dtor(struct swc *c) {}

/* ---- errors ---- */
void b_throw_length_error(const void *x)
{
	b_fatal("std::length_error in the driver");
}

void b_throw_out_of_range(const void *x)
{
	b_fatal("std::out_of_range in the driver");
}

/* ---- std::__sort for <, on u32, long and unsigned long ---- */
#define SORT(name, T)                                                       \
	void name(T *a, T *b, void *cmp)                                    \
	{                                                                   \
		size_t n = (size_t)(b - a);                                 \
		/* heapsort: no recursion, no allocation */                 \
		for (size_t i = n / 2; i-- > 0;) {                          \
			size_t r = i;                                       \
			for (size_t c; (c = 2 * r + 1) < n; r = c) {        \
				if (c + 1 < n && a[c] < a[c + 1]) c++;      \
				if (!(a[r] < a[c])) break;                  \
				T t = a[r]; a[r] = a[c]; a[c] = t;          \
			}                                                   \
		}                                                           \
		for (size_t e = n; e-- > 1;) {                              \
			T t = a[0]; a[0] = a[e]; a[e] = t;                  \
			size_t r = 0;                                       \
			for (size_t c; (c = 2 * r + 1) < e; r = c) {        \
				if (c + 1 < e && a[c] < a[c + 1]) c++;      \
				if (!(a[r] < a[c])) break;                  \
				T u = a[r]; a[r] = a[c]; a[c] = u;          \
			}                                                   \
		}                                                           \
	}
SORT(b_sort_u32, uint32_t)
SORT(b_sort_long, long)
SORT(b_sort_ulong, unsigned long)

/* ---- streams and locales: never called (measured); data objects are
 * zero blocks so references resolve ---- */
uint64_t b_cxx_zero_object[64];

#define STOP(fn, what) \
	void fn(void) { b_fatal("libc++ " what " used by the driver (not provided)"); }
STOP(b_cxx_stream_stop, "iostream")
STOP(b_cxx_locale_stop, "locale")

/* std::__next_prime (unordered containers): smallest prime >= n */
size_t b_next_prime(size_t n)
{
	if (n <= 2)
		return 2;
	for (n |= 1;; n += 2) {
		int prime = 1;
		for (size_t d = 3; d <= n / d; d += 2)
			if (n % d == 0) {
				prime = 0;
				break;
			}
		if (prime)
			return n;
	}
}
