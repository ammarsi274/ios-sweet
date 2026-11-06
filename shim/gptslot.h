#ifndef DRG_GPTSLOT_H
#define DRG_GPTSLOT_H
#include <stdint.h>

/* 512-byte block access by absolute LBA; 0 on success */
struct gpt_io {
	int (*rd)(void *ctx, uint64_t lba, uint8_t *buf);
	int (*wr)(void *ctx, uint64_t lba, const uint8_t *buf);
	void *ctx;
};

#define GPT_SLOTS_NORMAL	0
#define GPT_SLOTS_TWRP_ONCE	1

struct gpt_slots_result {
	int primary, backup;		/* 0 unchanged, 1 written, <0 error (nothing written for that copy) */
	uint8_t was_a, was_b;		/* boot_a / boot_b attribute byte 6 found in the primary copy */
};

/* last_lba: the device's last sector (backup header); returns <0, 0 (nothing to do) or 1 (written) */
int gpt_slots(const struct gpt_io *io, uint64_t last_lba, int mode, struct gpt_slots_result *r);

#endif
