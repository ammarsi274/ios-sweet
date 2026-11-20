/*
 * Read-only GPT lookup by partition name (eMMC, drg_emmc_read only), shared
 * by the hardware tests (drg_w1.c modem_a, drg_bt.c bluetooth_a, drg_a1.c).
 */
#ifndef DRG_PART_H
#define DRG_PART_H
#include <stdint.h>

/*
 * 0: *first, *last = the partition's LBAs, *index = its 1-based GPT index.
 * Negative: -1 header read/magic, -2 entry array bad, -3 entry read, -4 not found.
 */
int drg_gpt_find(const char *name, uint64_t *first, uint64_t *last, uint32_t *index);

/* drg_fat read callback for a partition starting at *(uint64_t *)ctx */
int drg_part_read(void *ctx, uint32_t lba, uint32_t count, void *buf);

#endif
