/*
 * drgles ELF loader: loads the Nokia 6.1 Plus's own Adreno driver libraries
 * (AArch64 ELF shared objects built for Android) into a process that is
 * not Android: TWRP's Linux on the bench, iOS later. Our own code.
 *
 * What it supports is what the Nokia libraries use (measured on vendor_b,
 * drg/gpu-research/TWRP-MEASURED.md): ET_DYN, PT_LOAD/PT_DYNAMIC/PT_GNU_RELRO,
 * RELA + JMPREL with R_AARCH64_{RELATIVE,ABS64,GLOB_DAT,JUMP_SLOT}, DT_HASH and
 * DT_GNU_HASH, DT_INIT/INIT_ARRAY, DF_SYMBOLIC. No TLS segment is used.
 * Anything else stops the load with a message, never a guess.
 *
 * Thread pointer: the libraries read their stack-protector canary from
 * [TPIDR_EL0 + 0x28] (bionic's TLS slot 5). XNU keeps the CPU number in
 * TPIDR_EL0, so every `mrs Xt, TPIDR_EL0` in an executable segment is
 * rewritten to `adrp Xt, <canary page>`; that read-only page holds the
 * canary at +0x28 and zeros elsewhere. The bench does the same rewrite.
 */
#ifndef DRGLES_ELF_H
#define DRGLES_ELF_H

#include <stddef.h>
#include <stdint.h>

struct drg_dso;

/*
 * Symbols the libraries import from Android's system libraries (libc, libm,
 * libdl, liblog, libcutils, libz, libc++, libhardware, libsync, ...).
 * `call` is 1 for a PLT import (R_AARCH64_JUMP_SLOT), 0 for a data or
 * address import. Return NULL if unknown.
 */
typedef void *(*drg_resolver)(const char *name, int call, void *ctx);

struct drg_elf_config {
	const char *const *dirs;	/* where the Nokia libraries are, searched in order */
	const char *const *system;	/* NEEDED names satisfied by the resolver */
	drg_resolver resolve;
	void *ctx;
	int verbose;
};

void drg_elf_setup(const struct drg_elf_config *cfg);

/* Load (once) a library by file name or path (path is reduced to its base name),
 * with its dependencies, relocated and initialised. NULL on failure, see
 * drg_elf_error(). A system name returns DRG_ELF_SYSTEM. */
struct drg_dso *drg_elf_open(const char *name);
#define DRG_ELF_SYSTEM ((struct drg_dso *)1)

/* Address of a defined global symbol. dso NULL: every loaded library in load
 * order, then the resolver. dso DRG_ELF_SYSTEM: the resolver only. */
void *drg_elf_sym(struct drg_dso *dso, const char *name);

/* dladdr: 1 if addr is inside a loaded library. */
int drg_elf_addr(const void *addr, const char **file, void **base, const char **sym, void **symaddr);

const char *drg_elf_error(void);

/* Number of `mrs Xt, TPIDR_EL0` rewritten so far (all libraries). */
unsigned drg_elf_tp_rewrites(void);

#endif
