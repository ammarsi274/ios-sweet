/*
 * libsync.so for gles-probe: the two fence calls Nokia's libgsl.so imports
 * (sync_wait, sync_merge), which TWRP does not ship. Our own code on the
 * fence interface of HMD's DRG kernel, drivers/staging/android/uapi/sync.h:
 *   SYNC_IOC_WAIT  _IOW('>', 0, __s32)                    timeout in ms
 *   SYNC_IOC_MERGE _IOWR('>', 1, struct sync_merge_data)  { s32 fd2; char name[32]; s32 fence; }
 */

extern int ioctl(int, int, void *);
extern int *__errno(void);

struct sync_merge_data {
	int fd2;
	char name[32];
	int fence;
};

#define IOC(dir, nr, size) ((unsigned)(dir) << 30 | (unsigned)(size) << 16 | '>' << 8 | (nr))
#define SYNC_IOC_WAIT IOC(1, 0, sizeof(int))
#define SYNC_IOC_MERGE IOC(3, 1, sizeof(struct sync_merge_data))

int sync_wait(int fd, int timeout)
{
	int t = timeout;
	return ioctl(fd, (int)SYNC_IOC_WAIT, &t);
}

int sync_merge(const char *name, int fd1, int fd2)
{
	struct sync_merge_data d = { .fd2 = fd2 };
	for (int i = 0; i < 31 && name && name[i]; i++)
		d.name[i] = name[i];
	if (ioctl(fd1, (int)SYNC_IOC_MERGE, &d) < 0)
		return -1;
	return d.fence;
}
