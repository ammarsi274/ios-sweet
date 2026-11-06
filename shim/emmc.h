#pragma once
/* DRG: read-only probe of the eMMC host controller ABL leaves running */
void drg_emmc_probe(void);
/* multi-block CMD18/CMD25 test on a scratch region past the iOS image (build with -DDRG_EMMC_MB_TEST) */
void drg_emmc_mb_test(void);
/* G2 GPU bring-up log in the unused part of userdata (see emmc.c) */
int drg_emmc_log_open(void);
int drg_emmc_log_write(const char *buf, unsigned len);
#include <stdint.h>
uint64_t drg_emmc_log_lba(void);
#include "gptslot.h"
/* boot slots in the GPT (gptslot.c): GPT_SLOTS_NORMAL / GPT_SLOTS_TWRP_ONCE */
int drg_emmc_slots(int mode, struct gpt_slots_result *r);
