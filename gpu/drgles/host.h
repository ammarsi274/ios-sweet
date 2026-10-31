/*
 * drgles host interface: the few services the ELF loader needs from the
 * process it runs in. One implementation per host:
 *   host_linux.c   TWRP on the Nokia (test bench), raw Linux system calls
 *   host_darwin.c  iOS, libSystem
 * Everything above this line is the same code on both.
 */
#ifndef DRGLES_HOST_H
#define DRGLES_HOST_H

#include <stddef.h>
#include <stdint.h>

#define HOST_PROT_R 1
#define HOST_PROT_W 2
#define HOST_PROT_X 4

/* Whole file into a new buffer (host_free it). NULL if missing. */
void *host_read_file(const char *path, size_t *size);
void host_free(void *p);
void *host_alloc(size_t n);	/* zeroed */

/* Anonymous private memory, page granular. NULL on failure. */
void *host_map(size_t len, int prot);
int host_protect(void *addr, size_t len, int prot);
void host_unmap(void *addr, size_t len);
void host_sync_icache(void *addr, size_t len);

void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void host_die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t host_random64(void);

/* 4 KiB on the Linux bench, 16 KiB on iOS (iPhone X userland). The Nokia
 * libraries are laid out for 4 KiB, so on iOS one host page can hold the
 * end of a code segment and the start of a data segment; the loader then
 * gives that page the union of both protections. */
size_t host_page_size(void);

/*
 * Services for the bionic layer (bionic/). Every call that can fail returns
 * a negative LINUX errno (the numbers Nokia's libraries know); the iOS host
 * translates from Darwin's. Paths and flags are Linux values too.
 */
struct host_ts {
	int64_t sec, nsec;
};
int host_clock(int linux_clock, struct host_ts *ts);	/* 0 REALTIME, 1 MONOTONIC, 7 BOOTTIME */
void host_sleep_ns(int64_t ns);

/* wait while *addr == val, at most rel_ns (-1: forever): 0, -EAGAIN, -ETIMEDOUT, -EINTR */
int host_futex_wait(volatile uint32_t *addr, uint32_t val, int64_t rel_ns);
void host_futex_wake(volatile uint32_t *addr, int count);

int host_thread_create(void *(*fn)(void *), void *arg, size_t stack, uint64_t *handle);
int host_thread_join(uint64_t handle, void **ret);
int host_thread_detach(uint64_t handle);
uint64_t host_thread_self(void);
__attribute__((noreturn)) void host_thread_exit(void *ret);
void *host_tls_get(void);	/* one pointer per thread, for the bionic layer */
void host_tls_set(void *p);
int host_gettid(void);
int host_getpid(void);

int host_open(const char *path, int linux_flags, int mode);
int host_close(int fd);
long host_read(int fd, void *buf, size_t n);
long host_write(int fd, const void *buf, size_t n);
long host_lseek(int fd, long off, int whence);
int host_fstat(int fd, void *linux_stat);	/* fills a Linux arm64 struct stat (128 bytes) */
int host_stat(const char *path, void *linux_stat);
int host_access(const char *path, int mode);
int host_poll(void *linux_pollfd, unsigned n, int timeout_ms);
int host_dup(int fd);
int host_fcntl(int fd, int linux_cmd, long arg);
long host_ioctl(int fd, unsigned long req, void *arg);	/* bench: the real device */
void *host_mmap_fd(void *addr, size_t len, int prot, int linux_flags, int fd, long off, int *err);
int host_munmap(void *addr, size_t len);
int host_msync(void *addr, size_t len, int linux_flags);
void *host_memalign(size_t align, size_t n);
uint64_t host_auxval(unsigned long type);	/* AT_HWCAP etc. of this CPU */
__attribute__((noreturn)) void host_exit(int code);

#endif
