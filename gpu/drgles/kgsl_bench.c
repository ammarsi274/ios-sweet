/*
 * KGSL layer for the bench: the real /dev/kgsl-3d0 of TWRP's kernel.
 * Every ioctl is checked against what Nokia's driver was measured to use;
 * anything else is reported (and still passed on, so it can be measured).
 */
#include "kgsl.h"
#include "host.h"
#include "rt.h"

#define NFD 8
static int fds[NFD];
static unsigned long seen_req[64];
static unsigned seen_count[64];
static int nseen;

/* (type 0x09) nr -> name, from HMD's msm_kgsl.h */
static const struct {
	unsigned nr;
	const char *name;
} known[] = {
	{ 0x02, "DEVICE_GETPROPERTY" }, { 0x13, "DRAWCTXT_CREATE" }, { 0x14, "DRAWCTXT_DESTROY" },
	{ 0x40, "SYNCSOURCE_CREATE" }, { 0x41, "SYNCSOURCE_DESTROY" }, { 0x45, "GPUOBJ_ALLOC" },
	{ 0x46, "GPUOBJ_FREE" }, { 0x4a, "GPU_COMMAND" },
};

int kgsl_open(int flags)
{
	int fd = host_open("/dev/kgsl-3d0", flags, 0);
	if (fd >= 0)
		for (int i = 0; i < NFD; i++)
			if (!fds[i]) {
				fds[i] = fd + 1;
				break;
			}
	return fd;
}

int kgsl_is_fd(int fd)
{
	for (int i = 0; i < NFD; i++)
		if (fds[i] == fd + 1)
			return 1;
	return 0;
}

int kgsl_close(int fd)
{
	for (int i = 0; i < NFD; i++)
		if (fds[i] == fd + 1) {
			fds[i] = 0;
			host_close(fd);
			return 1;
		}
	return 0;
}

struct waitts { uint32_t context_id, timestamp, timeout; };
#define IOC_WAITTS_CTXTID	0x400c0907UL	/* _IOW(0x09, 0x7, 12 bytes) */

/* kgsl_cap.c: wait for a captured submit (KGSL's own WAITTIMESTAMP_CTXTID) */
long kgsl_cap_wait(int fd, uint32_t ctx, uint32_t ts, uint32_t timeout_ms)
{
	struct waitts w = { ctx, ts, timeout_ms };
	return host_ioctl(fd, IOC_WAITTS_CTXTID, &w);
}

long kgsl_ioctl(int fd, unsigned long req, void *arg)
{
	int i;
	for (i = 0; i < nseen && seen_req[i] != req; i++)
		;
	if (i == nseen && nseen < 64)
		seen_req[nseen++] = req;
	if (i < 64)
		seen_count[i]++;
	kgsl_cap_before(fd, req, arg);
	long rc = host_ioctl(fd, req, arg);
	kgsl_cap_after(fd, req, arg, rc);
	return rc;
}

void *kgsl_mmap(void *addr, size_t len, int prot, int flags, int fd, long off, int *err)
{
	void *p = host_mmap_fd(addr, len, prot, flags, fd, off, err);
	kgsl_cap_mmap(p, len, prot, flags, off);
	return p;
}

int kgsl_munmap(void *addr, size_t len)
{
	return 0;	/* the bench maps through the kernel: plain munmap is right */
}

void kgsl_report(void)
{
	for (int i = 0; i < nseen; i++) {
		unsigned long r = seen_req[i];
		const char *name = "UNEXPECTED";
		for (unsigned k = 0; k < sizeof(known) / sizeof(known[0]); k++)
			if (((r >> 8) & 0xff) == 0x09 && (r & 0xff) == known[k].nr)
				name = known[k].name;
		host_log("kgsl ioctl %08lx %-20s x%u\n", r, name, seen_count[i]);
	}
}
