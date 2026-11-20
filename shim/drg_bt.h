/* DRG hardware test B1: WCN3990 Bluetooth over BLSP2 UART1, see drg_bt.c. */
#ifndef DRG_BT_H
#define DRG_BT_H
#include <stdint.h>

#ifndef DRG_HWLOG_TYPES
#define DRG_HWLOG_TYPES
/* shim.c cpu_put: one log line "label aaaaaaaa vvvvvvvv ok|xN"; rc 0 = ok, -1..-9 = failure code */
typedef void (*drg_log_fn)(const char *label, uint32_t a, uint32_t v, int rc);
/* shim.c: write the whole log to the userdata log area now */
typedef void (*drg_flush_fn)(void);
#endif

/* 0: HCI Reset answered; <0: stopped at that step (all logged). Powers BT down again either way. */
int drg_bt_test(drg_log_fn log, drg_flush_fn flush);

#endif
