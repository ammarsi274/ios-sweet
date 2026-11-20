#ifndef DRG_A1_H
#define DRG_A1_H
#include <stdint.h>
#include "drg_w1.h"	/* drg_log_fn, drg_flush_fn */

/* research/reports/audio.md A1: (a) TAS2557 amp probe over I2C, (b) ADSP boot through TZ PAS 1, then shut down */
int drg_a1_test(drg_log_fn log, drg_flush_fn flush);

#endif
