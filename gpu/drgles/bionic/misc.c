/*
 * The rest of what the Nokia's driver imports from Android's system
 * libraries: dl*, logging, properties, environment, tracing, system
 * information, fortify checks, fences, process control.
 * Values that describe the machine are the ones measured on the Nokia
 * under TWRP (drg/gpu-research/TWRP-MEASURED.md).
 */
#include "bionic.h"
#include "../elf.h"

void *(*bionic_bench_hook)(const char *name);
int bionic_verbose = 1;

void b_log(const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	host_log("%s", b);
}

void b_fatal(const char *what)
{
	host_die("drgles: fatal: %s\n", what);
}

/* ---- dl ---- */
static char dl_err[256];
static int dl_err_set;

void *b_dlopen(const char *name, int flags)
{
	if (!name)
		return DRG_ELF_SYSTEM;
	struct drg_dso *d = drg_elf_open(name);
	if (bionic_verbose)
		b_log("drgles: dlopen(%s) -> %s\n", name, d ? "ok" : drg_elf_error());
	if (!d) {
		snprintf(dl_err, sizeof(dl_err), "%s", drg_elf_error());
		dl_err_set = 1;
	}
	return d;
}

void *b_dlsym(void *h, const char *name)
{
	void *p = h == 0 || h == (void *)-1L ? drg_elf_sym(0, name) : drg_elf_sym(h, name);
	if (!p) {
		snprintf(dl_err, sizeof(dl_err), "undefined symbol: %s", name);
		dl_err_set = 1;
	}
	return p;
}

char *b_dlerror(void)
{
	if (!dl_err_set)
		return 0;
	dl_err_set = 0;
	return dl_err;
}

int b_dlclose(void *h)
{
	return 0;	/* stays loaded */
}

int b_dladdr(const void *a, void **info)
{
	/* Dl_info: dli_fname, dli_fbase, dli_sname, dli_saddr */
	return drg_elf_addr(a, (const char **)&info[0], &info[1], (const char **)&info[2], &info[3]);
}

/* ---- log ---- */
int b_vlog_print(int prio, const char *tag, const char *fmt, struct va64 *va)
{
	char b[1024];
	b_vformat(b, sizeof(b), fmt, va);
	b_log("log %d %s: %s\n", prio, tag ? tag : "-", b);
	return 1;
}

void b_vlog_assert(const char *cond, const char *tag, const char *fmt, struct va64 *va)
{
	char b[1024] = "";
	if (fmt)
		b_vformat(b, sizeof(b), fmt, va);
	b_log("ASSERT %s %s: %s\n", cond ? cond : "-", tag ? tag : "-", b);
	b_fatal("__android_log_assert");
}

/* ---- properties and environment: none set (measured: the driver only
 * reads vendor.debug.prerotation.disable and ro.vendor.graphics.memory, both
 * at their defaults on the phone) ---- */
int b_property_get(const char *key, char *value, const char *def)
{
	size_t n = def ? strlen(def) : 0;
	if (n > 91)
		n = 91;
	memcpy(value, def ? def : "", n);
	value[n] = 0;
	return (int)n;
}

int b_property_get_bool(const char *key, int def)
{
	return def;
}

int b_property_set(const char *key, const char *value)
{
	if (bionic_verbose)
		b_log("drgles: property_set(%s, %s) ignored\n", key, value);
	return 0;
}

int b___system_property_get(const char *key, char *value)
{
	value[0] = 0;
	return 0;
}

char *b_getenv(const char *k)
{
	return 0;
}

/* ---- tracing (libcutils): always off ---- */
uint8_t b_atrace_is_ready = 1;
uint64_t b_atrace_enabled_tags;
void b_atrace_setup(void) {}
void b_atrace_begin_body(const char *n) {}
void b_atrace_end_body(void) {}
void b_atrace_async_begin_body(const char *n, int32_t c) {}
void b_atrace_async_end_body(const char *n, int32_t c) {}
void b_atrace_int_body(const char *n, int32_t v) {}
uint64_t b_atrace_get_enabled_tags(void)
{
	return 0;
}

/* ---- system information ---- */
long b_sysconf(int name)
{
	switch (name) {
	case 0x0027: /* _SC_PAGESIZE */
	case 0x0028: /* _SC_PAGE_SIZE */
		return (long)host_page_size();
	case 0x0060: /* _SC_NPROCESSORS_CONF */
	case 0x0061: /* _SC_NPROCESSORS_ONLN: cpu "0-7", measured */
		return 8;
	case 0x0006: /* _SC_CLK_TCK */
		return 100;
	}
	if (bionic_verbose)
		b_log("drgles: sysconf(0x%x) unknown\n", name);
	B_ERR(LE_INVAL);
	return -1;
}

int b_getpagesize(void)
{
	return (int)host_page_size();
}

unsigned long b_getauxval(unsigned long type)
{
	return host_auxval(type);
}

/* Linux struct sysinfo (arm64) */
struct lsysinfo {
	long uptime;
	unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
	unsigned short procs, pad;
	unsigned long totalhigh, freehigh;
	unsigned int mem_unit;
	char f[0];
};

int b_sysinfo(struct lsysinfo *s)
{
	struct host_ts t;
	memset(s, 0, sizeof(*s));
	host_clock(7, &t);
	s->uptime = t.sec;
	s->totalram = 3822936UL * 1024;	/* MemTotal under TWRP */
	s->freeram = s->totalram / 2;
	s->procs = 1;
	s->mem_unit = 1;
	return 0;
}

int b_uname(char *u)
{
	static const char *f[6] = { "Linux", "localhost", "4.4.78-perf+", "#1 SMP PREEMPT", "aarch64", "localdomain" };
	memset(u, 0, 65 * 6);
	for (int i = 0; i < 6; i++)
		memcpy(u + 65 * i, f[i], strlen(f[i]));
	return 0;
}

int b_getrusage(int who, void *ru)
{
	memset(ru, 0, 144);
	return 0;
}

struct mallinfo_ {
	size_t f[10];
};
struct mallinfo_ b_mallinfo(void)
{
	struct mallinfo_ m = { { 0 } };
	return m;
}

/* ---- errors ---- */
static const char *errname(int e)
{
	switch (e) {
	case 0: return "Success";
	case LE_PERM: return "Operation not permitted";
	case LE_NOENT: return "No such file or directory";
	case LE_INTR: return "Interrupted system call";
	case LE_IO: return "I/O error";
	case LE_BADF: return "Bad file descriptor";
	case LE_AGAIN: return "Try again";
	case LE_NOMEM: return "Out of memory";
	case LE_ACCES: return "Permission denied";
	case LE_FAULT: return "Bad address";
	case LE_BUSY: return "Device or resource busy";
	case LE_EXIST: return "File exists";
	case LE_NODEV: return "No such device";
	case LE_INVAL: return "Invalid argument";
	case LE_NOTTY: return "Not a typewriter";
	case LE_RANGE: return "Math result not representable";
	case LE_DEADLK: return "Resource deadlock would occur";
	case LE_NOSYS: return "Function not implemented";
	case LE_TIMEDOUT: return "Connection timed out";
	}
	return "Unknown error";
}

char *b_strerror(int e)
{
	return (char *)errname(e);
}

char *b___gnu_strerror_r(int e, char *buf, size_t n)
{
	snprintf(buf, n, "%s", errname(e));
	return buf;
}

/* ---- files the driver may try to create: refused (logged) ---- */
static int refuse(const char *what, const char *path)
{
	if (bionic_verbose)
		b_log("drgles: %s(%s) refused\n", what, path ? path : "");
	B_ERR(LE_ACCES);
	return -1;
}
int b_mkdir(const char *p, int m) { return refuse("mkdir", p); }
int b_rmdir(const char *p) { return refuse("rmdir", p); }
int b_unlink(const char *p) { return refuse("unlink", p); }
int b_chmod(const char *p, int m) { return refuse("chmod", p); }
int b_mkstemp(char *p) { return refuse("mkstemp", p); }
int b_system(const char *c) { return refuse("system", c); }

char *b___getcwd_chk(char *buf, size_t len, size_t actual)
{
	if (len > actual)
		b_fatal("__getcwd_chk: buffer overflow");
	if (len < 2) {
		B_ERR(LE_RANGE);
		return 0;
	}
	buf[0] = '/', buf[1] = 0;
	return buf;
}

int b___umask_chk(int mode)
{
	return 022;
}

int b_isatty(int fd)
{
	B_ERR(LE_NOTTY);
	return 0;
}

char *b_getlogin(void)
{
	return 0;
}

/* ---- time ---- */
int b_clock_gettime(int clk, struct host_ts *ts)
{
	return (int)b_ret(host_clock(clk, ts));
}

int b_gettimeofday(long *tv, void *tz)
{
	struct host_ts t;
	host_clock(0, &t);
	if (tv) {
		tv[0] = t.sec;
		tv[1] = t.nsec / 1000;
	}
	return 0;
}

int b_usleep(unsigned us)
{
	host_sleep_ns((int64_t)us * 1000);
	return 0;
}

/* ---- process ---- */
void b_abort(void)
{
	b_fatal("abort() called by the driver");
}

void b_exit(int code)
{
	b_log("drgles: exit(%d) called by the driver\n", code);
	host_exit(code);
}

int b_raise(int sig)
{
	char m[48];
	snprintf(m, sizeof(m), "raise(%d) called by the driver", sig);
	b_fatal(m);
}

void b___stack_chk_fail(void)
{
	b_fatal("stack protector: stack corruption in the driver");
}

const char *b___progname = "drgles";

int b_vprctl(int option, struct va64 *va)
{
	return 0;	/* thread naming etc.: nothing to do on either host */
}

int b___cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
	return 0;	/* the driver stays loaded for the life of the process */
}

void b___cxa_finalize(void *dso) {}

int b___register_atfork(void (*a)(void), void (*b)(void), void (*c)(void), void *dso)
{
	return 0;
}

/* ---- fortify ---- */
size_t b___strlen_chk(const char *s, size_t n)
{
	size_t l = strlen(s);
	if (l >= n)
		b_fatal("__strlen_chk: buffer overflow");
	return l;
}

extern size_t strlcpy(char *, const char *, size_t);
extern size_t strlcat(char *, const char *, size_t);

size_t b___strlcpy_chk(char *d, const char *s, size_t n, size_t dlen)
{
	if (n > dlen)
		b_fatal("__strlcpy_chk: buffer overflow");
	return strlcpy(d, s, n);
}

size_t b___strlcat_chk(char *d, const char *s, size_t n, size_t dlen)
{
	if (n > dlen)
		b_fatal("__strlcat_chk: buffer overflow");
	return strlcat(d, s, n);
}

void *b___memcpy_chk(void *d, const void *s, size_t n, size_t dlen)
{
	if (n > dlen)
		b_fatal("__memcpy_chk: buffer overflow");
	return memcpy(d, s, n);
}

void *b___memset_chk(void *d, int c, size_t n, size_t dlen)
{
	if (n > dlen)
		b_fatal("__memset_chk: buffer overflow");
	return memset(d, c, n);
}

char *b___strrchr_chk(const char *s, int c, size_t n)
{
	return strrchr(s, c);
}

void *b_memalign(size_t align, size_t n)
{
	return host_memalign(align, n);
}

/* ---- fences (HMD's drivers/staging/android/uapi/sync.h) ---- */
long b_ioctl_fixed(int fd, unsigned req, void *arg);

int b_sync_wait(int fd, int timeout)
{
	int t = timeout;
	return (int)b_ioctl_fixed(fd, 0x40043e00 /* SYNC_IOC_WAIT */, &t);
}

int b_sync_merge(const char *name, int fd1, int fd2)
{
	struct {
		int fd2;
		char name[32];
		int fence;
	} d = { .fd2 = fd2 };
	for (int i = 0; i < 31 && name && name[i]; i++)
		d.name[i] = name[i];
	if (b_ioctl_fixed(fd1, 0xc0283e01 /* SYNC_IOC_MERGE */, &d) < 0)
		return -1;
	return d.fence;
}

/* ---- native_handle (libcutils): int version, numFds, numInts, data[] ---- */
extern void *malloc(size_t);
extern void free(void *);

int *b_native_handle_create(int nfds, int nints)
{
	if (nfds < 0 || nints < 0 || nfds > 1024 || nints > 1024)
		return 0;
	int *h = malloc(sizeof(int) * (size_t)(3 + nfds + nints));
	h[0] = (int)(sizeof(int) * 3);
	h[1] = nfds;
	h[2] = nints;
	return h;
}

int b_native_handle_delete(int *h)
{
	free(h);
	return 0;
}

int b_native_handle_close(const int *h)
{
	for (int i = 0; i < h[1]; i++)
		host_close(h[3 + i]);
	return 0;
}

int *b_native_handle_clone(const int *h)
{
	int *c = b_native_handle_create(h[1], h[2]);
	if (!c)
		return 0;
	for (int i = 0; i < h[1]; i++)
		c[3 + i] = host_dup(h[3 + i]);
	memcpy(c + 3 + h[1], h + 3 + h[1], sizeof(int) * (size_t)h[2]);
	return c;
}

/* ---- gralloc: the bench lends TWRP's libhardware for now ---- */
/* gralloc: ours (gralloc.c); the bench can lend Nokia's real module for
 * measurements through bionic_bench_hook */
int b_gralloc_get_module(const char *id, const void **out);

int b_hw_get_module(const char *id, const void **module)
{
	int (*f)(const char *, const void **) = bionic_bench_hook ? bionic_bench_hook("hw_get_module") : 0;
	return f ? f(id, module) : b_gralloc_get_module(id, module);
}

/* basename/dirname: the result lives in a per-thread buffer, like bionic's */
static char *pathbuf(const char *src, size_t n)
{
	struct bthread *t = bthread_self();
	if (!t->keys[BKEYS - 1])
		t->keys[BKEYS - 1] = host_alloc(4096);
	char *b = t->keys[BKEYS - 1];
	if (n > 4095)
		n = 4095;
	memcpy(b, src, n);
	b[n] = 0;
	return b;
}

char *b_basename(const char *p)
{
	if (!p || !*p)
		return pathbuf(".", 1);
	size_t e = strlen(p);
	while (e > 1 && p[e - 1] == '/')
		e--;
	size_t s = e;
	while (s && p[s - 1] != '/')
		s--;
	return e == s ? pathbuf("/", 1) : pathbuf(p + s, e - s);
}

char *b_dirname(const char *p)
{
	if (!p || !*p)
		return pathbuf(".", 1);
	size_t e = strlen(p);
	while (e > 1 && p[e - 1] == '/')
		e--;
	while (e && p[e - 1] != '/')
		e--;
	if (!e)
		return pathbuf(".", 1);
	while (e > 1 && p[e - 1] == '/')
		e--;
	return pathbuf(p, e);
}
