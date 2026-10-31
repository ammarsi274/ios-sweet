/*
 * drgles host for iOS: libSystem underneath, Linux values above (host.h).
 * Nokia's libraries pass Linux flag bits and expect Linux errno numbers;
 * everything is translated here. Declarations are our own (no SDK headers,
 * as host_linux.c): the signatures and constants are Darwin's (xnu's
 * bsd/sys headers, libpthread, libplatform).
 */
#include "host.h"
#include "rt.h"

/* ---- libSystem ---- */
typedef long ssize_t;
typedef int64_t off_t;
typedef struct _opaque_pthread_t *pthread_t;
typedef struct { long sig; char opaque[56]; } pthread_attr_t;
typedef unsigned long pthread_key_t;
struct darwin_timespec { long tv_sec, tv_nsec; };
struct darwin_stat {		/* struct stat, 64-bit inodes (arm64) */
	int32_t st_dev;
	uint16_t st_mode, st_nlink;
	uint64_t st_ino;
	uint32_t st_uid, st_gid;
	int32_t st_rdev;
	struct darwin_timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
	int64_t st_size, st_blocks;
	int32_t st_blksize;
	uint32_t st_flags, st_gen;
	int32_t st_lspare;
	int64_t st_qspare[2];
};

void *calloc(size_t, size_t);
void *malloc(size_t);
void free(void *);
int posix_memalign(void **, size_t, size_t);
int *__error(void);
int open(const char *, int, ...);
int close(int);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
off_t lseek(int, off_t, int);
int fstat(int, struct darwin_stat *);
int stat(const char *, struct darwin_stat *);
int access(const char *, int);
int poll(void *, unsigned int, int);
int dup(int);
int fcntl(int, int, ...);
int ioctl(int, unsigned long, ...);
void *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int mprotect(void *, size_t, int);
int msync(void *, size_t, int);
int getpagesize(void);
int clock_gettime(int, struct darwin_timespec *);
int nanosleep(const struct darwin_timespec *, struct darwin_timespec *);
int getpid(void);
void arc4random_buf(void *, size_t);
void sys_icache_invalidate(void *, size_t);
void exit(int) __attribute__((noreturn));
int pthread_attr_init(pthread_attr_t *);
int pthread_attr_destroy(pthread_attr_t *);
int pthread_attr_setstacksize(pthread_attr_t *, size_t);
int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
int pthread_detach(pthread_t);
pthread_t pthread_self(void);
void pthread_exit(void *) __attribute__((noreturn));
int pthread_key_create(pthread_key_t *, void (*)(void *));
void *pthread_getspecific(pthread_key_t);
int pthread_setspecific(pthread_key_t, const void *);
int pthread_threadid_np(pthread_t, uint64_t *);
/* libsystem_kernel: compare-and-wait on a 32-bit word (what libpthread's mutexes use) */
int __ulock_wait2(uint32_t operation, void *addr, uint64_t value, uint64_t timeout_ns, uint64_t value2);
int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

#define UL_COMPARE_AND_WAIT	1
#define ULF_WAKE_ALL		0x00000100
#define ULF_NO_ERRNO		0x01000000

/* Darwin values (bsd/sys/fcntl.h, mman.h, errno.h, time.h) */
#define D_O_NONBLOCK	0x00000004
#define D_O_APPEND	0x00000008
#define D_O_NOFOLLOW	0x00000100
#define D_O_CREAT	0x00000200
#define D_O_TRUNC	0x00000400
#define D_O_EXCL	0x00000800
#define D_O_DIRECTORY	0x00100000
#define D_O_CLOEXEC	0x01000000
#define D_MAP_SHARED	0x0001
#define D_MAP_PRIVATE	0x0002
#define D_MAP_FIXED	0x0010
#define D_MAP_NORESERVE	0x0040
#define D_MAP_JIT	0x0800
#define D_MAP_ANON	0x1000
#define D_MS_ASYNC	0x0001
#define D_MS_INVALIDATE	0x0002
#define D_MS_SYNC	0x0010
#define D_F_GETLK	7
#define D_F_SETLK	8
#define D_F_SETLKW	9
#define D_F_DUPFD_CLOEXEC 67

/* Linux values (arm64 asm-generic) */
#define L_O_CREAT	0100
#define L_O_EXCL	0200
#define L_O_TRUNC	01000
#define L_O_APPEND	02000
#define L_O_NONBLOCK	04000
#define L_O_DIRECTORY	040000
#define L_O_NOFOLLOW	0100000
#define L_O_CLOEXEC	02000000
#define L_MAP_SHARED	0x01
#define L_MAP_PRIVATE	0x02
#define L_MAP_FIXED	0x10
#define L_MAP_ANONYMOUS	0x20
#define L_MAP_NORESERVE	0x4000
#define L_MS_ASYNC	1
#define L_MS_INVALIDATE	2
#define L_MS_SYNC	4
#define L_F_GETFL	3
#define L_F_SETFL	4
#define L_F_GETLK	5
#define L_F_SETLK	6
#define L_F_SETLKW	7
#define L_F_DUPFD_CLOEXEC 1030

/* Darwin errno -> Linux errno. 1-34 are the same numbers except 11 (Darwin EDEADLK). */
static int lerr(int e)
{
	if (e >= 1 && e <= 34 && e != 11)
		return e;
	switch (e) {
	case 11: return 35;	/* EDEADLK */
	case 35: return 11;	/* EAGAIN */
	case 36: return 115;	/* EINPROGRESS */
	case 37: return 114;	/* EALREADY */
	case 38: return 88;	/* ENOTSOCK */
	case 45: return 95;	/* ENOTSUP */
	case 48: return 98;	/* EADDRINUSE */
	case 54: return 104;	/* ECONNRESET */
	case 55: return 105;	/* ENOBUFS */
	case 57: return 107;	/* ENOTCONN */
	case 60: return 110;	/* ETIMEDOUT */
	case 61: return 111;	/* ECONNREFUSED */
	case 62: return 40;	/* ELOOP */
	case 63: return 36;	/* ENAMETOOLONG */
	case 66: return 39;	/* ENOTEMPTY */
	case 78: return 38;	/* ENOSYS */
	case 84: return 75;	/* EOVERFLOW */
	case 89: return 125;	/* ECANCELED */
	default: return 5;	/* EIO */
	}
}

static long ret(long r)
{
	return r < 0 ? -lerr(*__error()) : r;
}

static int dflags(int lf)	/* open / F_SETFL flags */
{
	int f = lf & 3;
	f |= lf & L_O_CREAT ? D_O_CREAT : 0;
	f |= lf & L_O_EXCL ? D_O_EXCL : 0;
	f |= lf & L_O_TRUNC ? D_O_TRUNC : 0;
	f |= lf & L_O_APPEND ? D_O_APPEND : 0;
	f |= lf & L_O_NONBLOCK ? D_O_NONBLOCK : 0;
	f |= lf & L_O_DIRECTORY ? D_O_DIRECTORY : 0;
	f |= lf & L_O_NOFOLLOW ? D_O_NOFOLLOW : 0;
	f |= lf & L_O_CLOEXEC ? D_O_CLOEXEC : 0;
	return f;
}

static int lflags(int df)	/* F_GETFL result */
{
	int f = df & 3;
	f |= df & D_O_APPEND ? L_O_APPEND : 0;
	f |= df & D_O_NONBLOCK ? L_O_NONBLOCK : 0;
	return f;
}

static int dmap(int lf)
{
	int f = 0;
	f |= lf & L_MAP_SHARED ? D_MAP_SHARED : 0;
	f |= lf & L_MAP_PRIVATE ? D_MAP_PRIVATE : 0;
	f |= lf & L_MAP_FIXED ? D_MAP_FIXED : 0;
	f |= lf & L_MAP_ANONYMOUS ? D_MAP_ANON : 0;
	f |= lf & L_MAP_NORESERVE ? D_MAP_NORESERVE : 0;
	return f;
}

/* ---- loader services ---- */

size_t host_page_size(void)
{
	return (size_t)getpagesize();	/* 16 KiB */
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
	int fd = open(path, 0);
	if (fd < 0)
		return 0;
	off_t n = lseek(fd, 0, 2);
	lseek(fd, 0, 0);
	char *buf = n > 0 ? malloc((size_t)n) : 0;
	off_t got = 0;
	while (buf && got < n) {
		ssize_t r = read(fd, buf + got, (size_t)(n - got));
		if (r <= 0)
			break;
		got += r;
	}
	close(fd);
	if (!buf || got != n) {
		free(buf);
		return 0;
	}
	*size = (size_t)n;
	return buf;
}

void *host_map(size_t len, int prot)
{
	void *p = mmap(0, len, prot, D_MAP_PRIVATE | D_MAP_ANON, -1, 0);
	if (p == (void *)-1 && (prot & HOST_PROT_W) && (prot & HOST_PROT_X))	/* code and data in one 16 KiB page */
		p = mmap(0, len, prot, D_MAP_PRIVATE | D_MAP_ANON | D_MAP_JIT, -1, 0);
	return p == (void *)-1 ? 0 : p;
}

int host_protect(void *addr, size_t len, int prot)
{
	return mprotect(addr, len, prot) ? -1 : 0;
}

void host_unmap(void *addr, size_t len)
{
	munmap(addr, len);
}

void host_sync_icache(void *addr, size_t len)
{
	sys_icache_invalidate(addr, len);	/* cleans the D-cache to PoU first */
}

static void out(int fd, const char *fmt, va_list ap)
{
	char b[1024];
	int n = vsnprintf(b, sizeof(b), fmt, ap);
	if (n > (int)sizeof(b) - 1)
		n = sizeof(b) - 1;
	if (n > 0)
		write(fd, b, (size_t)n);
}

void host_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	out(1, fmt, ap);
	va_end(ap);
}

void host_die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	out(2, fmt, ap);
	va_end(ap);
	exit(99);
}

uint64_t host_random64(void)
{
	uint64_t v = 0;
	arc4random_buf(&v, sizeof(v));
	return v;
}

/* ---- services for the bionic layer ---- */

int host_clock(int clk, struct host_ts *ts)
{
	int d;
	switch (clk) {
	case 0: d = 0; break;	/* REALTIME */
	case 1:			/* MONOTONIC: stops in sleep on Linux */
	case 4: d = 8; break;	/* MONOTONIC_RAW -> CLOCK_UPTIME_RAW */
	case 7: d = 6; break;	/* BOOTTIME -> CLOCK_MONOTONIC (counts sleep) */
	case 2: d = 12; break;	/* PROCESS_CPUTIME_ID */
	case 3: d = 16; break;	/* THREAD_CPUTIME_ID */
	default: return -22;
	}
	struct darwin_timespec t;
	if (clock_gettime(d, &t))
		return -lerr(*__error());
	ts->sec = t.tv_sec;
	ts->nsec = t.tv_nsec;
	return 0;
}

void host_sleep_ns(int64_t ns)
{
	struct darwin_timespec t = { (long)(ns / 1000000000), (long)(ns % 1000000000) };
	nanosleep(&t, 0);
}

int host_futex_wait(volatile uint32_t *addr, uint32_t val, int64_t rel_ns)
{
	if (*addr != val)
		return -11;	/* EAGAIN, as Linux */
	if (rel_ns == 0)
		return -110;
	int r = __ulock_wait2(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, (void *)addr, val,
			      rel_ns < 0 ? 0 : (uint64_t)rel_ns, 0);
	if (r >= 0)
		return 0;
	return -lerr(-r);	/* ETIMEDOUT, EINTR, EFAULT */
}

void host_futex_wake(volatile uint32_t *addr, int count)
{
	__ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO | (count > 1 ? ULF_WAKE_ALL : 0), (void *)addr, 0);
}

int host_thread_create(void *(*fn)(void *), void *arg, size_t stack, uint64_t *handle)
{
	pthread_attr_t attr;
	pthread_t t;
	pthread_attr_init(&attr);
	if (stack) {
		size_t pg = host_page_size();
		pthread_attr_setstacksize(&attr, (stack + pg - 1) & ~(pg - 1));
	}
	int r = pthread_create(&t, &attr, fn, arg);
	pthread_attr_destroy(&attr);
	if (r)
		return -lerr(r);
	*handle = (uint64_t)(uintptr_t)t;
	return 0;
}

int host_thread_join(uint64_t h, void **retv)
{
	int r = pthread_join((pthread_t)(uintptr_t)h, retv);
	return r ? -lerr(r) : 0;
}

int host_thread_detach(uint64_t h)
{
	int r = pthread_detach((pthread_t)(uintptr_t)h);
	return r ? -lerr(r) : 0;
}

uint64_t host_thread_self(void)
{
	return (uint64_t)(uintptr_t)pthread_self();
}

void host_thread_exit(void *retv)
{
	pthread_exit(retv);
}

static pthread_key_t tls_key;
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
	uint64_t tid = 0;
	pthread_threadid_np(0, &tid);
	return (int)tid;
}

int host_getpid(void)
{
	return getpid();
}

int host_open(const char *path, int flags, int mode)
{
	return (int)ret(open(path, dflags(flags), mode));
}

int host_close(int fd)
{
	return (int)ret(close(fd));
}

long host_read(int fd, void *buf, size_t n)
{
	return ret(read(fd, buf, n));
}

long host_write(int fd, const void *buf, size_t n)
{
	return ret(write(fd, buf, n));
}

long host_lseek(int fd, long off, int whence)
{
	return ret(lseek(fd, off, whence));	/* SEEK_SET/CUR/END: same numbers */
}

/* Linux arm64 struct stat: 128 bytes */
static void lstat_of(const struct darwin_stat *d, void *out)
{
	uint8_t *s = out;
	memset(s, 0, 128);
	uint64_t dev = (uint32_t)d->st_dev, ino = d->st_ino, rdev = (uint32_t)d->st_rdev;
	uint32_t mode = d->st_mode, nlink = d->st_nlink;	/* S_IF* and permission bits: same values */
	int64_t size = d->st_size, blocks = d->st_blocks;
	int32_t blksize = d->st_blksize;
	memcpy(s + 0, &dev, 8);
	memcpy(s + 8, &ino, 8);
	memcpy(s + 16, &mode, 4);
	memcpy(s + 20, &nlink, 4);
	memcpy(s + 24, &d->st_uid, 4);
	memcpy(s + 28, &d->st_gid, 4);
	memcpy(s + 32, &rdev, 8);
	memcpy(s + 48, &size, 8);
	memcpy(s + 56, &blksize, 4);
	memcpy(s + 64, &blocks, 8);
	int64_t t[6] = { d->st_atimespec.tv_sec, d->st_atimespec.tv_nsec, d->st_mtimespec.tv_sec,
			 d->st_mtimespec.tv_nsec, d->st_ctimespec.tv_sec, d->st_ctimespec.tv_nsec };
	memcpy(s + 72, t, sizeof(t));
}

int host_fstat(int fd, void *st)
{
	struct darwin_stat d;
	if (fstat(fd, &d))
		return -lerr(*__error());
	lstat_of(&d, st);
	return 0;
}

int host_stat(const char *path, void *st)
{
	struct darwin_stat d;
	if (stat(path, &d))
		return -lerr(*__error());
	lstat_of(&d, st);
	return 0;
}

int host_access(const char *path, int mode)
{
	return (int)ret(access(path, mode));	/* F_OK/X/W/R: same bits */
}

int host_poll(void *fds, unsigned n, int timeout_ms)
{
	return (int)ret(poll(fds, n, timeout_ms));	/* struct pollfd and POLL* bits: same layout and values */
}

int host_dup(int fd)
{
	return (int)ret(dup(fd));
}

int host_fcntl(int fd, int cmd, long arg)
{
	switch (cmd) {
	case 0: case 1: case 2:		/* F_DUPFD, F_GETFD, F_SETFD (FD_CLOEXEC = 1 on both) */
		return (int)ret(fcntl(fd, cmd, arg));
	case L_F_GETFL: {
		int r = fcntl(fd, 3);
		return r < 0 ? -lerr(*__error()) : lflags(r);
	}
	case L_F_SETFL:
		return (int)ret(fcntl(fd, 4, dflags((int)arg)));
	case L_F_DUPFD_CLOEXEC:
		return (int)ret(fcntl(fd, D_F_DUPFD_CLOEXEC, arg));
	case L_F_GETLK: case L_F_SETLK: case L_F_SETLKW:
		return -22;	/* struct flock differs; not used by the measured driver */
	default:
		return -22;
	}
}

long host_ioctl(int fd, unsigned long req, void *arg)
{
	return ret(ioctl(fd, req, arg));
}

void *host_mmap_fd(void *addr, size_t len, int prot, int flags, int fd, long off, int *err)
{
	void *p = mmap(addr, len, prot, dmap(flags), fd, off);
	if (p == (void *)-1) {
		*err = lerr(*__error());
		return (void *)-1;
	}
	*err = 0;
	return p;
}

int host_munmap(void *addr, size_t len)
{
	return (int)ret(munmap(addr, len));
}

int host_msync(void *addr, size_t len, int flags)
{
	int f = (flags & L_MS_ASYNC ? D_MS_ASYNC : 0) | (flags & L_MS_INVALIDATE ? D_MS_INVALIDATE : 0) |
		(flags & L_MS_SYNC ? D_MS_SYNC : 0);
	return (int)ret(msync(addr, len, f));
}

void *host_memalign(size_t align, size_t n)
{
	void *p = 0;
	if (align < sizeof(void *))
		align = sizeof(void *);
	return posix_memalign(&p, align, n) ? 0 : p;
}

/*
 * The Nokia's Linux reported "fp asimd evtstrm aes pmull sha1 sha2 crc32"
 * (device-info/cpu-mem.txt): HWCAP bits 0-7. XNU does not run the timer
 * event stream, so EVTSTRM (bit 2) is not claimed.
 */
uint64_t host_auxval(unsigned long type)
{
	switch (type) {
	case 6: return host_page_size();	/* AT_PAGESZ */
	case 16: return 0xfb;			/* AT_HWCAP */
	case 17: return 100;			/* AT_CLKTCK */
	case 26: return 0;			/* AT_HWCAP2 */
	default: return 0;
	}
}

void host_exit(int code)
{
	exit(code);
}
