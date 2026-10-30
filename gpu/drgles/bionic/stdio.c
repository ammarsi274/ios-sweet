/*
 * stdio for the Nokia's driver: our own FILE (the driver only passes FILE *
 * back to these functions), the printf family over AAPCS64 va_lists, and
 * the files the driver reads, served from what was measured on the phone
 * under TWRP (drg/gpu-research/TWRP-MEASURED.md) so the bench sees exactly
 * what iOS will.
 */
#include "bionic.h"

enum { F_FD, F_MEM, F_LOG, F_STREAM };
struct bfile {
	int kind, fd, err, eof, writable;
	const char *mem;	/* F_MEM */
	size_t len, pos;
	char *buf;		/* F_STREAM: open_memstream */
	size_t cap;
	char **out_ptr;
	size_t *out_size;
};

static struct bfile f_in = { .kind = F_LOG }, f_out = { .kind = F_LOG, .fd = 1 }, f_err = { .kind = F_LOG, .fd = 2 };
struct bfile *b_stdin = &f_in, *b_stdout = &f_out, *b_stderr = &f_err;

/* measured on the Nokia 6.1 Plus under TWRP (2026-10-29) */
static const struct {
	const char *path, *text;
} virt[] = {
	{ "/sys/devices/system/cpu/present", "0-7\n" },
	{ "/sys/devices/system/cpu/possible", "0-7\n" },
	{ "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "1612800\n" },
	{ "/sys/devices/system/cpu/cpu1/cpufreq/cpuinfo_max_freq", "1612800\n" },
	{ "/sys/devices/system/cpu/cpu2/cpufreq/cpuinfo_max_freq", "1612800\n" },
	{ "/sys/devices/system/cpu/cpu3/cpufreq/cpuinfo_max_freq", "1612800\n" },
	{ "/sys/devices/system/cpu/cpu4/cpufreq/cpuinfo_max_freq", "1804800\n" },
	{ "/sys/devices/system/cpu/cpu5/cpufreq/cpuinfo_max_freq", "1804800\n" },
	{ "/sys/devices/system/cpu/cpu6/cpufreq/cpuinfo_max_freq", "1804800\n" },
	{ "/sys/devices/system/cpu/cpu7/cpufreq/cpuinfo_max_freq", "1804800\n" },
	{ "/sys/class/kgsl/kgsl-3d0/gpu_model", "Adreno512v1\n" },
};

/* configuration and debug files the driver looks for; none exist on the
 * phone (measured), so they never exist here either */
static int optional_config(const char *p)
{
	return !strncmp(p, "/data/vendor/gpu/", 17) || !strncmp(p, "/data/misc/gpu/", 15) || !strncmp(p, "./", 2);
}

struct bfile *b_fopen(const char *path, const char *mode)
{
	for (unsigned i = 0; i < sizeof(virt) / sizeof(virt[0]); i++)
		if (!strcmp(path, virt[i].path)) {
			if (mode[0] != 'r' || strchr(mode, '+')) {
				B_ERR(LE_ACCES);
				return 0;
			}
			struct bfile *f = host_alloc(sizeof(*f));
			f->kind = F_MEM;
			f->mem = virt[i].text;
			f->len = strlen(f->mem);
			return f;
		}
	if (optional_config(path) || !strncmp(path, "/sys/", 5) || !strncmp(path, "/proc/", 6)) {
		if (strncmp(path, "/sys/devices/system/cpu/cpu0/cpu_capacity", 41) && !optional_config(path))
			b_log("drgles: fopen(%s): not provided\n", path);
		B_ERR(LE_NOENT);
		return 0;
	}
	if (mode[0] != 'r' || strchr(mode, '+')) {
		b_log("drgles: fopen(%s, %s): writing refused\n", path, mode);
		B_ERR(LE_ACCES);
		return 0;
	}
	int fd = host_open(path, 0x80000 /* O_RDONLY|O_CLOEXEC */, 0);
	if (fd < 0) {
		B_ERR(-fd);
		return 0;
	}
	struct bfile *f = host_alloc(sizeof(*f));
	f->kind = F_FD;
	f->fd = fd;
	return f;
}

static void stream_sync(struct bfile *f)
{
	if (f->kind == F_STREAM) {
		*f->out_ptr = f->buf;
		*f->out_size = f->len;
	}
}

struct bfile *b_open_memstream(char **ptr, size_t *size)
{
	struct bfile *f = host_alloc(sizeof(*f));
	extern void *malloc(size_t);
	f->kind = F_STREAM;
	f->cap = 256;
	f->buf = malloc(f->cap);	/* the caller frees it with free() */
	f->buf[0] = 0;
	f->out_ptr = ptr;
	f->out_size = size;
	f->writable = 1;
	stream_sync(f);
	return f;
}

int b_fclose(struct bfile *f)
{
	if (!f || f == b_stdin || f == b_stdout || f == b_stderr)
		return 0;
	stream_sync(f);
	if (f->kind == F_FD)
		host_close(f->fd);
	host_free(f);
	return 0;
}

int b_fflush(struct bfile *f)
{
	if (f)
		stream_sync(f);
	return 0;
}

size_t b_fread(void *p, size_t size, size_t n, struct bfile *f)
{
	size_t want = size * n, got = 0;
	if (!want)
		return 0;
	if (f->kind == F_MEM) {
		got = f->len - f->pos < want ? f->len - f->pos : want;
		memcpy(p, f->mem + f->pos, got);
		f->pos += got;
	} else if (f->kind == F_FD) {
		while (got < want) {
			long r = host_read(f->fd, (char *)p + got, want - got);
			if (r <= 0) {
				if (r < 0)
					f->err = 1;
				break;
			}
			got += (size_t)r;
		}
	}
	if (got < want)
		f->eof = 1;
	return got / size;
}

size_t b___fread_chk(void *p, size_t size, size_t n, struct bfile *f, size_t bufsize)
{
	if (size && n > bufsize / size)
		b_fatal("__fread_chk: buffer overflow");
	return b_fread(p, size, n, f);
}

long b_file_write(struct bfile *f, const void *p, size_t n)
{
	if (f->kind == F_LOG) {
		char b[512];
		size_t k = n < sizeof(b) - 1 ? n : sizeof(b) - 1;
		memcpy(b, p, k);
		b[k] = 0;
		b_log("%s", b);
		return (long)n;
	}
	if (f->kind == F_STREAM) {
		extern void *realloc(void *, size_t);
		if (f->len + n + 1 > f->cap) {
			while (f->len + n + 1 > f->cap)
				f->cap *= 2;
			f->buf = realloc(f->buf, f->cap);
		}
		memcpy(f->buf + f->len, p, n);
		f->len += n;
		f->buf[f->len] = 0;
		stream_sync(f);
		return (long)n;
	}
	if (f->kind == F_FD && f->writable)
		return host_write(f->fd, p, n);
	f->err = 1;
	return -1;
}

size_t b_fwrite(const void *p, size_t size, size_t n, struct bfile *f)
{
	if (!size || !n)
		return 0;
	long r = b_file_write(f, p, size * n);
	return r < 0 ? 0 : (size_t)r / size;
}

size_t b___fwrite_chk(const void *p, size_t size, size_t n, struct bfile *f, size_t bufsize)
{
	if (size && n > bufsize / size)
		b_fatal("__fwrite_chk: buffer overflow");
	return b_fwrite(p, size, n, f);
}

int b_fputs(const char *s, struct bfile *f)
{
	return b_file_write(f, s, strlen(s)) < 0 ? -1 : 0;
}

int b_fputc(int c, struct bfile *f)
{
	char ch = (char)c;
	return b_file_write(f, &ch, 1) < 0 ? -1 : (unsigned char)c;
}

static int getc_(struct bfile *f)
{
	unsigned char c;
	return b_fread(&c, 1, 1, f) == 1 ? c : -1;
}

char *b_fgets(char *s, int n, struct bfile *f)
{
	int i = 0;
	if (n <= 0)
		return 0;
	while (i < n - 1) {
		int c = getc_(f);
		if (c < 0)
			break;
		s[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (!i)
		return 0;
	s[i] = 0;
	return s;
}

char *b___fgets_chk(char *s, int n, struct bfile *f, size_t dlen)
{
	if (n < 0 || (size_t)n > dlen)
		b_fatal("__fgets_chk: buffer overflow");
	return b_fgets(s, n, f);
}

long b_getline(char **line, size_t *cap, struct bfile *f)
{
	extern void *realloc(void *, size_t);
	size_t n = 0;
	int c;
	while ((c = getc_(f)) >= 0) {
		if (n + 2 > *cap || !*line) {
			*cap = *cap ? *cap * 2 : 128;
			*line = realloc(*line, *cap);
		}
		(*line)[n++] = (char)c;
		if (c == '\n')
			break;
	}
	if (!n)
		return -1;
	(*line)[n] = 0;
	return (long)n;
}

int b_fseeko(struct bfile *f, long off, int whence)
{
	if (f->kind == F_MEM) {
		long base = whence == 0 ? 0 : whence == 1 ? (long)f->pos : (long)f->len;
		if (base + off < 0) {
			B_ERR(LE_INVAL);
			return -1;
		}
		f->pos = (size_t)(base + off);
		f->eof = 0;
		return 0;
	}
	if (f->kind == F_FD) {
		f->eof = 0;
		return b_ret(host_lseek(f->fd, off, whence)) < 0 ? -1 : 0;
	}
	B_ERR(LE_INVAL);
	return -1;
}

long b_ftello(struct bfile *f)
{
	if (f->kind == F_MEM)
		return (long)f->pos;
	if (f->kind == F_FD)
		return b_ret(host_lseek(f->fd, 0, 1));
	if (f->kind == F_STREAM)
		return (long)f->len;
	return 0;
}

/* ---- printf family ---- */
int b_vsnprintf(char *buf, size_t n, const char *fmt, struct va64 *va)
{
	return b_vformat(buf, n, fmt, va);
}

int b___vsnprintf_chk(char *buf, size_t n, int flags, size_t dlen, const char *fmt, struct va64 *va)
{
	if (n > dlen)
		b_fatal("__vsnprintf_chk: buffer overflow");
	return b_vformat(buf, n, fmt, va);
}

int b_vsnprintf3(char *buf, size_t n, const char *fmt, struct va64 *va)
{
	return b_vformat(buf, n, fmt, va);
}

int b_vsprintf2(char *buf, const char *fmt, struct va64 *va)
{
	return b_vformat(buf, (size_t)1 << 30, fmt, va);
}

int b_vfprintf(struct bfile *f, const char *fmt, struct va64 *va)
{
	char small[512];
	struct va64 copy = *va;
	int n = b_vformat(small, sizeof(small), fmt, va);
	if (n < (int)sizeof(small))
		return b_file_write(f, small, (size_t)n) < 0 ? -1 : n;
	extern void *malloc(size_t);
	extern void free(void *);
	char *big = malloc((size_t)n + 1);
	b_vformat(big, (size_t)n + 1, fmt, &copy);
	long r = b_file_write(f, big, (size_t)n);
	free(big);
	return r < 0 ? -1 : n;
}

int b_vprintf(const char *fmt, struct va64 *va)
{
	return b_vfprintf(b_stdout, fmt, va);
}

int b_vsscanf(const char *s, const char *fmt, struct va64 *va)
{
	return b_vscan(s, fmt, va);
}

void b_perror(const char *s)
{
	b_log("%s: errno %d\n", s ? s : "", bthread_self()->err);
}
