/* DRG hardware test W1: modem (MSS) boot through the MBA, see drg_w1.c. */
#ifndef DRG_W1_H
#define DRG_W1_H
#include <stdint.h>

#ifndef DRG_HWLOG_TYPES
#define DRG_HWLOG_TYPES
/* shim.c cpu_put: one log line "label aaaaaaaa vvvvvvvv ok|xN"; rc 0 = ok, -1..-9 = failure code */
typedef void (*drg_log_fn)(const char *label, uint32_t a, uint32_t v, int rc);
/* shim.c: write the whole log to the userdata log area now */
typedef void (*drg_flush_fn)(void);
#endif

/* 0: modem authenticated (MBA status 4); <0: stopped at that step (all logged) */
int drg_w1_test(drg_log_fn log, drg_flush_fn flush);

#endif
