/* Minimal polled RPM client for the Nokia 6.1 Plus (DRG), see drg_rpm.c. */
#ifndef DRG_RPM_H
#define DRG_RPM_H
#include <stdint.h>

#define RPM_RES_LDOB		0x626f646cu	/* "ldob": PM660L LDOs */

int drg_rpm_ldo_enable(uint32_t type, uint32_t id, uint32_t uv);

#endif
