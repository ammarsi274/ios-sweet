/*
 * Files, devices and memory for the Nokia's driver. Linux flag values and
 * errno numbers in, host calls out. Devices: /dev/kgsl-3d0 goes to the KGSL
 * layer; the 2D cores (/dev/kgsl-2d0/2d1) do not exist on this phone
 * (measured: ENOENT under TWRP). /dev/ion is opened by the driver but its
 * use is still being measured: on the bench it is passed through and every
 * ioctl on it is counted.
 */
#include "bionic.h"
#include "../kgsl.h"

#define LO_CREAT 0100
#define LO_TMPFILE 020200000

static int ion_fd = -1;
static unsigned long ion_req[16];
static unsigned ion_count[16];

int b_open(const char *path, int flags, int mode)
{
	long r;
	if (!strcmp(path, "/dev/kgsl-3d0"))
		r = kgsl_open(flags);
	else if (!strcmp(path, "/dev/ion")) {
		r = host_open(path, flags, mode);
		if (r >= 0)
			ion_fd = (int)r;
		b_log("drgles: open(/dev/ion) -> %ld\n", r);
	} else if (!strncmp(path, "/dev/", 5))
		r = -LE_NOENT;
	else
		r = host_open(path, flags, mode);
	return (int)b_ret(r);
}

int b_vopen(const char *path, int flags, struct va64 *va)
{
	int mode = (flags & LO_CREAT) || (flags & LO_TMPFILE) == LO_TMPFILE ? (int)va64_int(va) : 0;
	return b_open(path, flags, mode);
}

int b___open_2(const char *path, int flags)
{
	return b_open(path, flags, 0);
}

int b_close(int fd)
{
	if (kgsl_close(fd))
		return 0;
	if (fd == ion_fd)
		ion_fd = -1;
	return (int)b_ret(host_close(fd));
}

long b_read(int fd, void *buf, size_t n)
{
	return b_ret(host_read(fd, buf, n));
}

long b___read_chk(int fd, void *buf, size_t n, size_t bufsize)
{
	if (n > bufsize)
		b_fatal("__read_chk: buffer overflow");
	return b_read(fd, buf, n);
}

long b_write(int fd, const void *buf, size_t n)
{
	if (fd == 1 || fd == 2) {
		extern struct bfile *b_stderr;
		return b_file_write(b_stderr, buf, n);
	}
	return b_ret(host_write(fd, buf, n));
}

long b___write_chk(int fd, const void *buf, size_t n, size_t bufsize)
{
	if (n > bufsize)
		b_fatal("__write_chk: buffer overflow");
	return b_write(fd, buf, n);
}

long b_lseek(int fd, long off, int whence)
{
	return b_ret(host_lseek(fd, off, whence));
}

int b_fstat(int fd, void *st)
{
	return (int)b_ret(host_fstat(fd, st));
}

int b_stat(const char *path, void *st)
{
	return (int)b_ret(host_stat(path, st));
}

int b_access(const char *path, int mode)
{
	return (int)b_ret(host_access(path, mode));
}

int b_poll(void *fds, unsigned n, int timeout)
{
	return (int)b_ret(host_poll(fds, n, timeout));
}

int b_dup(int fd)
{
	return (int)b_ret(host_dup(fd));
}

int b_vfcntl(int fd, int cmd, struct va64 *va)
{
	long arg = va64_int(va);
	return (int)b_ret(host_fcntl(fd, cmd, arg));
}

long b_ioctl_fixed(int fd, unsigned req, void *arg)
{
	if (kgsl_is_fd(fd))
		return b_ret(kgsl_ioctl(fd, req, arg));
	if (fd == ion_fd && ion_fd >= 0) {
		int i;
		for (i = 0; i < 16 && ion_req[i] && ion_req[i] != req; i++)
			;
		if (i < 16) {
			ion_req[i] = req;
			ion_count[i]++;
		}
	}
	return b_ret(host_ioctl(fd, req, arg));
}

int b_vioctl(int fd, int req, struct va64 *va)
{
	void *arg = (void *)va64_int(va);
	return (int)b_ioctl_fixed(fd, (unsigned)req, arg);
}

void b_sys_report(void)
{
	for (int i = 0; i < 16 && ion_req[i]; i++)
		b_log("ion ioctl %08lx x%u\n", ion_req[i], ion_count[i]);
}

void *b_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
	int err = 0;
	void *p;
	if (fd >= 0 && kgsl_is_fd(fd))
		p = kgsl_mmap(addr, len, prot, flags, fd, off, &err);
	else
		p = host_mmap_fd(addr, len, prot, flags, fd, off, &err);
	if (p == (void *)-1)
		B_ERR(err);
	return p;
}

int b_munmap(void *addr, size_t len)
{
	if (kgsl_munmap(addr, len))
		return 0;
	return (int)b_ret(host_munmap(addr, len));
}

int b_msync(void *addr, size_t len, int flags)
{
	return (int)b_ret(host_msync(addr, len, flags));
}
