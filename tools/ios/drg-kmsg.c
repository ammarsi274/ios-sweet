/*
 * drg-kmsg: keep the kernel's messages on the phone. A launchd daemon (root)
 * on the Nokia 6.1 Plus that copies the kernel message buffer (what dmesg
 * shows: exec lines, CODE SIGNING, CRASH, DRG drivers) to
 * /private/var/log/kernel.log, which is read back from TWRP afterwards.
 *
 * Every second it takes the whole buffer (libproc proc_kmsgbuf, root only)
 * and appends what is new since the last copy: the last 512 bytes it saw
 * are looked up in the new copy (the latest occurrence), and everything
 * after them is new. If they are gone (more than the buffer was written in
 * one second) a marker says text was lost and the whole buffer follows.
 * Each write is fsync'ed: a forced reset loses at most one second. Boot
 * with msgbuf=1048576 so the buffer still holds the start of the boot when
 * launchd starts this.
 */
#include <stddef.h>
#include <stdint.h>

int proc_kmsgbuf(void *buffer, uint32_t buffersize);
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
int open(const char *path, int flags, ...);
long write(int fd, const void *buf, size_t n);
int fsync(int fd);
unsigned int sleep(unsigned int seconds);
void *malloc(size_t n);
void *memcpy(void *, const void *, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int snprintf(char *, size_t, const char *, ...);

#define O_WRONLY	0x0001	/* Darwin values */
#define O_APPEND	0x0008
#define O_CREAT		0x0200
#define TAIL		512


/* bytes of cur after the latest copy of prev's tail, or -1 if that tail is not in cur */
long kmsg_new_from(const char *prev, size_t plen, const char *cur, size_t clen)
{
	size_t t = plen < TAIL ? plen : TAIL;
	const char *tail = prev + plen - t;
	if (t == 0)
		return 0;
	for (size_t i = clen >= t ? clen - t + 1 : 0; i-- > 0;)
		if (!memcmp(cur + i, tail, t))
			return (long)(i + t);
	return -1;
}

#ifndef KMSG_TEST
static int out;

static void put(const char *p, size_t n)
{
	while (n) {
		long w = write(out, p, n);
		if (w <= 0)
			return;
		p += w;
		n -= (size_t)w;
	}
}

int main(void)
{
	int size = 0;
	size_t len = sizeof(size);
	if (sysctlbyname("kern.msgbuf", &size, &len, 0, 0) || size <= 0)
		size = 128 * 1024;
	char *a = malloc((size_t)size + 2), *b = malloc((size_t)size + 2);
	out = open("/private/var/log/kernel.log", O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (!a || !b || out < 0)
		return 1;
	char head[96];
	int hn = snprintf(head, sizeof(head), "\n== drg-kmsg: started, kernel message buffer %d bytes ==\n", size);
	put(head, (size_t)hn);
	size_t plen = 0;
	for (;;) {
		int n = proc_kmsgbuf(b, (uint32_t)size + 2);
		size_t clen = n > 0 ? (size_t)n : 0;
		while (clen && b[clen - 1] == 0)
			clen--;
		long from = kmsg_new_from(a, plen, b, clen);
		if (from < 0) {
			static const char lost[] = "\n== drg-kmsg: some kernel text was lost here ==\n";
			put(lost, sizeof(lost) - 1);
			from = 0;
		}
		if ((size_t)from < clen) {
			put(b + from, clen - (size_t)from);
			fsync(out);
		}
		char *t = a;
		a = b;
		b = t;
		plen = clen;
		sleep(1);
	}
}
#endif
