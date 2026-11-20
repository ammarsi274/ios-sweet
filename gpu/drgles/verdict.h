/*
 * The GPU self-test's cached verdict (root r25): bench -selftest-boot runs
 * the self-test once per root image and writes the verdict with a key, and
 * libdrgeagl in backboardd uses the GPU only if that file's key matches the
 * running bench, library and kernel and no GPU fault was recorded since.
 *
 *   /private/var/db/drg/gpu-selftest   (root 0644, in root 0755 /private/var/db/drg)
 *     drg-gpu-selftest 1
 *     key 0123456789abcdef
 *     verdict mode 16            or     verdict none
 *   /private/var/tmp/drgeagl-fault     libdrgeagl (backboardd runs as mobile) after a GPU fault;
 *                                      the next boot's self-test reruns and removes it
 *
 * Key: FNV-1a 64 over the bench's and the library's bytes, then the kernel's
 * version string (sysctl kern.version: a new kernel = a new DrgGPU). Not a
 * security check, a change detector.
 */
#ifndef DRGLES_VERDICT_H
#define DRGLES_VERDICT_H

#include <stddef.h>
#include <stdint.h>

#define DRG_VERDICT_DIR		"/private/var/db/drg"
#define DRG_VERDICT_FILE	"/private/var/db/drg/gpu-selftest"
#define DRG_FAULT_FILE		"/private/var/tmp/drgeagl-fault"
#define DRG_BENCH_PATH		"/usr/local/bin/drgles-bench"
#define DRG_EAGL_PATH		"/usr/local/lib/libdrgeagl.dylib"

/* FNV-1a 64 of every file (in order, NULL-terminated list), then `extra`; *missing = files not read */
uint64_t drg_verdict_key(const char *const *files, const char *extra, int *missing);
/* iOS: the key of DRG_BENCH_PATH, DRG_EAGL_PATH and sysctl kern.version */
uint64_t drg_verdict_key_now(int *missing);
/* the file's text for key and mode (-1 = none); its length, or -1 if buf is too small */
int drg_verdict_format(char *buf, size_t n, uint64_t key, int mode);
/* 0 and *key, *mode (-1 = none) if buf is a well-formed verdict file, else -1 */
int drg_verdict_parse(const char *buf, uint64_t *key, int *mode);

/* what to do with a cached verdict (the decision both programs make) */
enum drg_verdict_use {
	DRG_VERDICT_USE,	/* cached, key matches, no fault since: use it (bench: do not rerun) */
	DRG_VERDICT_NONE,	/* no file (first boot of this root image) */
	DRG_VERDICT_BAD,	/* malformed file */
	DRG_VERDICT_STALE,	/* key differs: bench, library or kernel changed */
	DRG_VERDICT_FAULT,	/* a GPU fault was recorded after it */
};
/* text: the cache file's contents or NULL; key: this boot's key; fault: the fault file exists */
enum drg_verdict_use drg_verdict_check(const char *text, uint64_t key, int fault, int *mode);
const char *drg_verdict_why(enum drg_verdict_use u);

#endif
