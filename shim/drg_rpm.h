/* Minimal polled RPM client for the Nokia 6.1 Plus (DRG), see drg_rpm.c. */
#ifndef DRG_RPM_H
#define DRG_RPM_H
#include <stdint.h>

#define RPM_RES_LDOB		0x626f646cu	/* "ldob": PM660L LDOs */
#define RPM_RES_RWCX		0x78637772u	/* "rwcx": CX level (pm660l_s3_level) */
#define RPM_RES_RWMX		0x786d7772u	/* "rwmx": MX level (pm660l_s5_level), as in the GPU ftrace */
#define RPM_KEY_SWEN		0x6e657773u	/* "swen" */
#define RPM_KEY_UV		0x00007675u	/* "uv" */
#define RPM_KEY_VLVL		0x6c766c76u	/* "vlvl": voltage level (rpm-smd-regulator.c) */

int drg_rpm_ldo_enable(uint32_t type, uint32_t id, uint32_t uv);
int drg_rpm_request2(uint32_t type, uint32_t id, uint32_t k1, uint32_t v1, uint32_t k2, uint32_t v2);

#endif
