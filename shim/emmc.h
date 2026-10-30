#pragma once
/* DRG: read-only probe of the eMMC host controller ABL leaves running */
void drg_emmc_probe(void);
/* G2 GPU bring-up log in the unused part of userdata (see emmc.c) */
int drg_emmc_log_open(void);
int drg_emmc_log_write(const char *buf, unsigned len);
#include <stdint.h>
uint64_t drg_emmc_log_lba(void);
