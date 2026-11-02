/*
 * drg-growfs: grow the iOS root volume to the userdata partition, minus the
 * shim's 64 MiB scratch area at its end (shim/emmc.c SCRATCH_SECTORS).
 *
 * The HFS+ volume flashed to userdata is 16 GiB (4,194,304 blocks of 4 KiB);
 * userdata is 49,399 MiB. XNU's HFS grows a mounted volume itself:
 * fsctl(HFSIOC_RESIZE_VOLUME) on the root -> hfs_extendfs, which checks the
 * device's DKIOCGETBLOCKSIZE / DKIOCGETBLOCKCOUNT (drg_emmc.c reports the
 * whole partition), that the new size is larger, a multiple of the sector
 * size and within the device. Run as root by launchd at every boot; it does
 * nothing once the volume has its size (a 16 GiB image flashed again is
 * simply grown again). Output goes to /private/var/log/growfs.log.
 *
 * Structs and ioctl numbers from xnu-8796 (bsd/sys/mount.h
 * __DARWIN_STRUCT_STATFS64, bsd/sys/disk.h, the HFS hfs_fsctl.h); built
 * without an SDK by tools/ios/build.sh.
 */
#include <stddef.h>
#include <stdint.h>

struct drg_statfs {			/* __DARWIN_STRUCT_STATFS64 (arm64: the only statfs) */
	uint32_t f_bsize;
	int32_t f_iosize;
	uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
	int32_t f_fsid[2];
	uint32_t f_owner, f_type, f_flags, f_fssubtype;
	char f_fstypename[16];
	char f_mntonname[1024];
	char f_mntfromname[1024];
	uint32_t f_flags_ext;
	uint32_t f_reserved[7];
};

int statfs(const char *path, struct drg_statfs *buf);
int fsctl(const char *path, unsigned long request, void *data, unsigned int options);
int open(const char *path, int flags, ...);
int close(int fd);
int ioctl(int fd, unsigned long request, ...);
int printf(const char *fmt, ...);
int *__error(void);

#define IOC_OUT			0x40000000UL
#define IOC_IN			0x80000000UL
#define IOC(dir, g, n, len)	((dir) | ((unsigned long)(len) & 0x1fff) << 16 | (g) << 8 | (n))
#define DKIOCGETBLOCKSIZE	IOC(IOC_OUT, 'd', 24, sizeof(uint32_t))
#define DKIOCGETBLOCKCOUNT	IOC(IOC_OUT, 'd', 25, sizeof(uint64_t))
#define HFSIOC_RESIZE_VOLUME	IOC(IOC_IN, 'h', 2, sizeof(uint64_t))

#define SCRATCH_BYTES		(64ULL << 20)

static uint64_t volume_bytes(struct drg_statfs *sf)
{
	if (statfs("/", sf) != 0)
		return 0;
	return sf->f_blocks * sf->f_bsize;
}

int main(void)
{
	struct drg_statfs sf;
	uint64_t cur = volume_bytes(&sf);
	if (cur == 0) {
		printf("growfs: statfs / failed: errno %d\n", *__error());
		return 1;
	}
	printf("growfs: / is %s on %s, %llu bytes (%llu blocks of %u)\n", sf.f_fstypename, sf.f_mntfromname,
	       (unsigned long long)cur, (unsigned long long)sf.f_blocks, sf.f_bsize);

	/* the raw device of the root: /dev/emmcroot0 -> /dev/remmcroot0 */
	char raw[1100] = "/dev/r";
	const char *from = sf.f_mntfromname;
	if (from[0] != '/' || from[1] != 'd' || from[2] != 'e' || from[3] != 'v' || from[4] != '/') {
		printf("growfs: unexpected root device %s\n", from);
		return 1;
	}
	unsigned n = 6;
	for (const char *p = from + 5; *p && n < sizeof(raw) - 1; p++)
		raw[n++] = *p;
	raw[n] = 0;
	int fd = open(raw, 0 /* O_RDONLY */);
	uint32_t ssize = 0;
	uint64_t scount = 0;
	if (fd < 0 || ioctl(fd, DKIOCGETBLOCKSIZE, &ssize) != 0 || ioctl(fd, DKIOCGETBLOCKCOUNT, &scount) != 0) {
		printf("growfs: %s: open/ioctl failed: errno %d\n", raw, *__error());
		if (fd >= 0)
			close(fd);
		return 1;
	}
	close(fd);
	uint64_t dev = scount * ssize;
	printf("growfs: %s: %llu sectors of %u = %llu bytes\n", raw, (unsigned long long)scount, ssize,
	       (unsigned long long)dev);
	if (ssize == 0 || sf.f_bsize == 0 || dev <= SCRATCH_BYTES) {
		printf("growfs: device too small\n");
		return 1;
	}

	/* the partition minus the scratch area, in whole volume blocks (also whole sectors) */
	uint64_t target = (dev - SCRATCH_BYTES) / sf.f_bsize * sf.f_bsize;
	if (target <= cur) {
		printf("growfs: nothing to do (target %llu <= current %llu)\n", (unsigned long long)target,
		       (unsigned long long)cur);
		return 0;
	}
	printf("growfs: growing to %llu bytes\n", (unsigned long long)target);
	if (fsctl("/", HFSIOC_RESIZE_VOLUME, &target, 0) != 0) {
		printf("growfs: HFSIOC_RESIZE_VOLUME failed: errno %d\n", *__error());
		return 1;
	}
	printf("growfs: done, / is now %llu bytes, %llu free\n", (unsigned long long)volume_bytes(&sf),
	       (unsigned long long)(sf.f_bavail * sf.f_bsize));
	return 0;
}
