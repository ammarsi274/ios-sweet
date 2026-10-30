/*
 * drgles host for the test bench: TWRP's Linux 4.4 on the Nokia, inside a
 * process started by TWRP's bionic. System calls are made directly
 * (arm64 Linux numbers); memory for our own bookkeeping comes from the
 * process's malloc.
 */
#include "host.h"
#include "rt.h"

void *calloc(size_t, size_t);
void free(void *);
void *malloc(size_t);

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
#define NR_openat 56
#define NR_close 57
#define NR_lseek 62
#define NR_read 63
#define NR_write 64
#define NR_exit_group 94
#define NR_munmap 215
#define NR_mmap 222
#define NR_mprotect 226
#define NR_getrandom 278

size_t host_page_size(void)
{
	return 4096;
}

void *host_alloc(size_t n)
{
	return calloc(1, n);
}

void host_free(void *p)
{
	free(p);
}

void *host_read_file(const char *path, size_t *size)
{
	long fd = sys6(NR_openat, -100, (long)path, 0 /* O_RDONLY */, 0, 0, 0);
	if (fd < 0)
		return 0;
	long n = sys6(NR_lseek, fd, 0, 2 /* SEEK_END */, 0, 0, 0);
	sys6(NR_lseek, fd, 0, 0, 0, 0, 0);
	char *buf = n > 0 ? malloc((size_t)n) : 0;
	long got = 0;
	while (buf && got < n) {
		long r = sys6(NR_read, fd, (long)(buf + got), n - got, 0, 0, 0);
		if (r <= 0)
			break;
		got += r;
	}
	sys6(NR_close, fd, 0, 0, 0, 0, 0);
	if (!buf || got != n) {
		free(buf);
		return 0;
	}
	*size = (size_t)n;
	return buf;
}

static int lprot(int p)
{
	return (p & HOST_PROT_R ? 1 : 0) | (p & HOST_PROT_W ? 2 : 0) | (p & HOST_PROT_X ? 4 : 0);
}

void *host_map(size_t len, int prot)
{
	long r = sys6(NR_mmap, 0, (long)len, lprot(prot), 0x22 /* MAP_PRIVATE|MAP_ANONYMOUS */, -1, 0);
	return r < 0 && r > -4096 ? 0 : (void *)r;
}

int host_protect(void *addr, size_t len, int prot)
{
	return sys6(NR_mprotect, (long)addr, (long)len, lprot(prot), 0, 0, 0) ? -1 : 0;
}

void host_unmap(void *addr, size_t len)
{
	sys6(NR_munmap, (long)addr, (long)len, 0, 0, 0, 0);
}

/* make new code visible: clean D-cache to PoU, invalidate I-cache (line
 * sizes from CTR_EL0, which Linux lets EL0 read) */
void host_sync_icache(void *addr, size_t len)
{
	uint64_t ctr, a, end = (uint64_t)addr + len;
	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	uint64_t dline = 4UL << ((ctr >> 16) & 15), iline = 4UL << (ctr & 15);
	for (a = (uint64_t)addr & ~(dline - 1); a < end; a += dline)
		__asm__ volatile("dc cvau, %0" ::"r"(a) : "memory");
	__asm__ volatile("dsb ish" ::: "memory");
	for (a = (uint64_t)addr & ~(iline - 1); a < end; a += iline)
		__asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
	__asm__ volatile("dsb ish; isb" ::: "memory");
}

static void out(const char *fmt, va_list ap)
{
	char b[1024];
	int n = vsnprintf(b, sizeof(b), fmt, ap);
	if (n > (int)sizeof(b) - 1)
		n = sizeof(b) - 1;
	sys6(NR_write, 1, (long)b, n, 0, 0, 0);
}

void host_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	out(fmt, ap);
	va_end(ap);
}

void host_die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	out(fmt, ap);
	va_end(ap);
	sys6(NR_exit_group, 99, 0, 0, 0, 0, 0);
	__builtin_unreachable();
}

uint64_t host_random64(void)
{
	uint64_t v = 0;
	sys6(NR_getrandom, (long)&v, 8, 0, 0, 0, 0);
	return v;
}

/* ---- services for the bionic layer (Linux numbers pass straight through) ---- */
#define NR_dup 23
#define NR_fcntl 25
#define NR_ioctl 29
#define NR_faccessat 48
#define NR_ppoll 73
#define NR_newfstatat 79
#define NR_fstat 80
#define NR_futex 98
#define NR_nanosleep 101
#define NR_clock_gettime 113
#define NR_getpid 172
#define NR_gettid 178
#define NR_msync 227
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

/* TWRP's bionic for threads and the one TLS slot: this is the host */
typedef long pthread_t;
int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
void pthread_exit(void *) __attribute__((noreturn));
int pthread_attr_init(void *);
int pthread_attr_setstacksize(void *, size_t);
int pthread_key_create(unsigned *, void (*)(void *));
void *pthread_getspecific(unsigned);
int pthread_setspecific(unsigned, const void *);
int posix_memalign(void **, size_t, size_t);
unsigned long getauxval(unsigned long);

int host_clock(int clk, struct host_ts *ts)
{
	return (int)sys6(NR_clock_gettime, clk, (long)ts, 0, 0, 0, 0);
}

void host_sleep_ns(int64_t ns)
{
	struct host_ts t = { ns / 1000000000, ns % 1000000000 };
	sys6(NR_nanosleep, (long)&t, 0, 0, 0, 0, 0);
}

int host_futex_wait(volatile uint32_t *addr, uint32_t val, int64_t rel_ns)
{
	struct host_ts t = { rel_ns / 1000000000, rel_ns % 1000000000 };
	return (int)sys6(NR_futex, (long)addr, FUTEX_WAIT_PRIVATE, val, rel_ns < 0 ? 0 : (long)&t, 0, 0);
}

void host_futex_wake(volatile uint32_t *addr, int count)
{
	sys6(NR_futex, (long)addr, FUTEX_WAKE_PRIVATE, count, 0, 0, 0);
}

int host_thread_create(void *(*fn)(void *), void *arg, size_t stack, uint64_t *handle)
{
	long attr[8] = { 0 };	/* bionic pthread_attr_t is 56 bytes */
	pthread_t t;
	pthread_attr_init(attr);
	if (stack)
		pthread_attr_setstacksize(attr, stack);
	int r = pthread_create(&t, attr, fn, arg);
	if (r)
		return -r;
	*handle = (uint64_t)t;
	return 0;
}

int host_thread_join(uint64_t h, void **ret)
{
	return -pthread_join((pthread_t)h, ret);
}

int host_thread_detach(uint64_t h)
{
	return -pthread_detach((pthread_t)h);
}

uint64_t host_thread_self(void)
{
	return (uint64_t)pthread_self();
}

void host_thread_exit(void *ret)
{
	pthread_exit(ret);
}

static unsigned tls_key;
static int tls_ready;

void *host_tls_get(void)
{
	return tls_ready ? pthread_getspecific(tls_key) : 0;
}

void host_tls_set(void *p)
{
	if (!tls_ready) {
		pthread_key_create(&tls_key, 0);
		tls_ready = 1;
	}
	pthread_setspecific(tls_key, p);
}

int host_gettid(void)
{
	return (int)sys6(NR_gettid, 0, 0, 0, 0, 0, 0);
}

int host_getpid(void)
{
	return (int)sys6(NR_getpid, 0, 0, 0, 0, 0, 0);
}

int host_open(const char *path, int flags, int mode)
{
	return (int)sys6(NR_openat, -100, (long)path, flags, mode, 0, 0);
}

int host_close(int fd)
{
	return (int)sys6(NR_close, fd, 0, 0, 0, 0, 0);
}

long host_read(int fd, void *buf, size_t n)
{
	return sys6(NR_read, fd, (long)buf, (long)n, 0, 0, 0);
}

long host_write(int fd, const void *buf, size_t n)
{
	return sys6(NR_write, fd, (long)buf, (long)n, 0, 0, 0);
}

long host_lseek(int fd, long off, int whence)
{
	return sys6(NR_lseek, fd, off, whence, 0, 0, 0);
}

int host_fstat(int fd, void *st)
{
	return (int)sys6(NR_fstat, fd, (long)st, 0, 0, 0, 0);
}

int host_stat(const char *path, void *st)
{
	return (int)sys6(NR_newfstatat, -100, (long)path, (long)st, 0, 0, 0);
}

int host_access(const char *path, int mode)
{
	return (int)sys6(NR_faccessat, -100, (long)path, mode, 0, 0, 0);
}

int host_poll(void *fds, unsigned n, int timeout_ms)
{
	struct host_ts t = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
	return (int)sys6(NR_ppoll, (long)fds, n, timeout_ms < 0 ? 0 : (long)&t, 0, 8, 0);
}

int host_dup(int fd)
{
	return (int)sys6(NR_dup, fd, 0, 0, 0, 0, 0);
}

int host_fcntl(int fd, int cmd, long arg)
{
	return (int)sys6(NR_fcntl, fd, cmd, arg, 0, 0, 0);
}

long host_ioctl(int fd, unsigned long req, void *arg)
{
	return sys6(NR_ioctl, fd, (long)req, (long)arg, 0, 0, 0);
}

void *host_mmap_fd(void *addr, size_t len, int prot, int flags, int fd, long off, int *err)
{
	long r = sys6(NR_mmap, (long)addr, (long)len, prot, flags, fd, off);
	if (r < 0 && r > -4096) {
		*err = (int)-r;
		return (void *)-1;
	}
	*err = 0;
	return (void *)r;
}

int host_munmap(void *addr, size_t len)
{
	return (int)sys6(NR_munmap, (long)addr, (long)len, 0, 0, 0, 0);
}

int host_msync(void *addr, size_t len, int flags)
{
	return (int)sys6(NR_msync, (long)addr, (long)len, flags, 0, 0, 0);
}

void *host_memalign(size_t align, size_t n)
{
	void *p = 0;
	if (align < sizeof(void *))
		align = sizeof(void *);
	return posix_memalign(&p, align, n) ? 0 : p;
}

uint64_t host_auxval(unsigned long type)
{
	return getauxval(type);
}

void host_exit(int code)
{
	sys6(NR_exit_group, code, 0, 0, 0, 0, 0);
	__builtin_unreachable();
}
