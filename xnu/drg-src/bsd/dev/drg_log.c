/*
 * DRG console log ring for the live log over USB (patch 0921).
 *
 * Every console write (osfmk/console/serial_console.c _cnputs, before the
 * GUI mute) is appended to a 1 MiB ring; drg_usb.c's live-log thread copies
 * new bytes out and sends them as UDP to 192.168.7.1:5140 every 100 ms
 * (boot-arg drgusb=2, drgusblog=1 by default). Ported from the other phone's
 * drg_ufs.c ring, without its flush to storage (DRG keeps its kernel.log
 * through userland).
 */
#if defined(ARM64_BOARD_CONFIG_DRG)
#include <sys/param.h>
#include <sys/systm.h>
#include <os/atomic_private.h>

#define DRG_LOG_SIZE    (1u << 20)

static char drg_log_ring[DRG_LOG_SIZE];
static uint64_t _Atomic drg_log_total;

/* Called for all console output, any context: never blocks. */
void drg_log_append(const char *c, int size);

void
drg_log_append(const char *c, int size)
{
	if (size <= 0) {
		return;
	}
	uint64_t at = os_atomic_add_orig(&drg_log_total, (uint64_t)size, relaxed);
	for (int i = 0; i < size; i++) {
		drg_log_ring[(at + (uint64_t)i) % DRG_LOG_SIZE] = c[i];
	}
}

/*
 * Copy console bytes [*pos, total) (at most max). A reader that fell more
 * than the ring behind skips ahead. Returns the byte count; *pos advances.
 */
uint32_t drg_log_copy(uint64_t *pos, char *out, uint32_t max);

uint32_t
drg_log_copy(uint64_t *pos, char *out, uint32_t max)
{
	uint64_t total = os_atomic_load(&drg_log_total, relaxed);
	if (total - *pos > DRG_LOG_SIZE) {
		*pos = total - DRG_LOG_SIZE;
	}
	uint32_t n = (uint32_t)MIN((uint64_t)max, total - *pos);
	for (uint32_t i = 0; i < n; i++) {
		out[i] = drg_log_ring[(*pos + i) % DRG_LOG_SIZE];
	}
	*pos += n;
	return n;
}

uint64_t drg_log_position(void);

uint64_t
drg_log_position(void)
{
	return os_atomic_load(&drg_log_total, relaxed);
}
#endif /* ARM64_BOARD_CONFIG_DRG */
