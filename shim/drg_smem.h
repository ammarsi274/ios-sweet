/*
 * Read-only SMEM access for the shim's hardware tests (W2 modem, A1 ADSP).
 * Layout and lookup as HMD's msm_smem.c (research/hmd-drg-kernel); nothing
 * is ever written to SMEM.
 */
#ifndef DRG_SMEM_H
#define DRG_SMEM_H
#include <stdint.h>
#include "drg_w1.h"	/* drg_log_fn */

#define SMEM_HOST_ANY		0xffffffffu	/* SMEM_ANY_HOST_FLAG: common partition, else the legacy TOC */
#define SMEM_HOST_MODEM		1
#define SMEM_HOST_Q6		2		/* ADSP */

#define SMEM_SSR_REASON_MSS0	421
#define SMEM_SSR_REASON_LPASS0	423
#define SMEM_SMP2P_MODEM_BASE	435		/* + read pid (apps 0): the modem's outbound SMP2P item */
#define SMEM_SMP2P_AUDIO_BASE	443
#define SMEM_GLINK_DESC		478		/* SMEM_GLINK_NATIVE_XPRT_DESCRIPTOR */
#define SMEM_GLINK_FIFO_0	479
#define SMEM_GLINK_FIFO_1	480

/* 0 ok (base/size/partitions logged once); negative: SMEM not usable, every other call returns "absent" */
int drg_smem_init(drg_log_fn log, const char *tag);
/* item address (0 = absent) and its size */
uintptr_t drg_smem_item(uint32_t id, uint32_t host, uint32_t *size);

/*
 * SMP2P item (smp2p_private.h struct smp2p_smem + entries {char name[16];
 * u32 value}): value of the entry named name (*found = 0 when absent);
 * with log, the header and every entry are logged.
 */
uint32_t drg_smp2p_entry(uint32_t id, uint32_t host, const char *name, int *found, drg_log_fn log, const char *tag);
/* first byte of a string item (0 if empty/absent); with log, up to 128 bytes logged, 8 per line, as ASCII-in-hex */
uint32_t drg_smem_string(uint32_t id, uint32_t host, drg_log_fn log, const char *label);

#endif
