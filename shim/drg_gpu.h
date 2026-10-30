#pragma once
#include <stdint.h>
/* DRG GPU bring-up G1 (read-only snapshot) + G2a (power, registers) + G2b: drg_gpu.c */
void drg_gpu_bringup(void);
/* shim.c: read one address on spare core idx; 0 ok, -1 read never completed,
 * -2 CPU_ON refused, -3 fault (esr), -4 core never started */
int drg_probe_read(int idx, uintptr_t addr, uint32_t *val, uint64_t *esr, unsigned timeout_us);
/* shim.c: run fn on spare core idx, same return codes */
int drg_probe_call(int idx, void (*fn)(void), uint64_t *esr, unsigned timeout_us);
