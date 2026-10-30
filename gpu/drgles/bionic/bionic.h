/*
 * drgles bionic layer: the Android C library calls the Nokia's Adreno
 * driver makes, implemented on the host interface (host.h) so the same
 * code runs on the TWRP bench and on iOS. Our own code.
 *
 * Rules for this layer:
 * - Objects the driver allocates (pthread_mutex_t, sem_t, jmp_buf, FILE *,
 *   std::string, ...) use bionic's/libc++'s sizes and the parts of their
 *   layout the driver's inline code or static initialisers depend on.
 * - errno values, flags and struct layouts the driver sees are Linux's.
 * - Variadic functions and va_list follow AAPCS64 (Linux), not Darwin's
 *   arm64 convention: see vararg.S and struct va64.
 */
#ifndef DRGLES_BIONIC_H
#define DRGLES_BIONIC_H

#include "../host.h"
#include "../rt.h"

/* Linux errno values */
#define LE_PERM 1
#define LE_NOENT 2
#define LE_INTR 4
#define LE_IO 5
#define LE_BADF 9
#define LE_AGAIN 11
#define LE_NOMEM 12
#define LE_ACCES 13
#define LE_FAULT 14
#define LE_BUSY 16
#define LE_EXIST 17
#define LE_NODEV 19
#define LE_INVAL 22
#define LE_NOTTY 25
#define LE_RANGE 34
#define LE_DEADLK 35
#define LE_NOSYS 38
#define LE_TIMEDOUT 110

/* AAPCS64 va_list, what the driver passes to vsnprintf & co. */
struct va64 {
	char *stack;
	char *gr_top;
	char *vr_top;
	int gr_offs;
	int vr_offs;
};
long va64_int(struct va64 *va);
double va64_double(struct va64 *va);

/* per-thread state of threads that run driver code */
#define BKEYS 128
struct bthread {
	int err;	/* errno, Linux numbers */
	void *keys[BKEYS];
	void *(*fn)(void *);
	void *arg;
};
struct bthread *bthread_self(void);
#define B_ERR(e) (bthread_self()->err = (e))
/* -errno from the host -> -1 and errno; anything else returned as is */
long b_ret(long r);

/* format engine (printf.c) */
int b_vformat(char *out, size_t n, const char *fmt, struct va64 *va);
int b_vscan(const char *s, const char *fmt, struct va64 *va);

/* files (stdio.c) */
struct bfile;
long b_file_write(struct bfile *f, const void *p, size_t n);

/* devices (sys.c) */
int b_open(const char *path, int flags, int mode);

/* exports: name -> address, NULL if not ours */
void *bionic_lookup(const char *name);

/* a hook for things only the bench provides (gralloc through TWRP) */
extern void *(*bionic_bench_hook)(const char *name);

void b_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void b_fatal(const char *what);

#endif
