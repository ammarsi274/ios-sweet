/*
 * Minimal polled RPM client for the Nokia 6.1 Plus (DRG): switch on the
 * touch analog supply pm660l_l6, which the RPM owns (boot #27: SPMI owner
 * EE 4, so a direct PMIC write is refused).
 *
 * Link, from this phone's device tree:
 *   qcom,glink-rpm-native-xprt  msgram 0x778000 + 0x7000, IPC 0x17911008 mask 1
 *   qcom,rpm-smd (qcom,rpm-glink)  edge "rpm", channel "rpm_requests", no reg
 *     -> RPM message format v0 (rpm-smd.c reads a version only from reg)
 *   rpm-regulator-ldob6  resource "ldob" id 6 (pm660l_l6), 1.7-3.3 V
 * Protocol, from HMD's DRG kernel (drivers/soc/qcom/glink_smem_native_xprt.c,
 * glink.c, rpm-smd.c, drivers/regulator/rpm-smd-regulator.c):
 *   - message RAM ends with a 256-byte TOC: magic 0x67727430, count, then
 *     {id, offset, size}; TX FIFO id 0x61703272, RX FIFO id 0x72326170.
 *     offset -> {read_index, write_index}, FIFO data right after it.
 *     32-bit accesses only; 8-byte alignment; 8 bytes kept free.
 *   - after each FIFO write: write index, then 1 to the IPC register
 *   - commands {u16 id, u16 p1, u32 p2}: VERSION 0 (v1, features 4),
 *     VERSION_ACK 1, OPEN 2 (+ name, padded to 8), OPEN_ACK 4,
 *     TX_DATA 9 (+ u32 size, u32 size_left, data padded to 8; intentless)
 *   - v0 request: {service 'req' 0x716572, request_len, msg_id, set 0 (active),
 *     resource type, resource id, data_len} + KVPs {key, nbytes, value}:
 *     "swen" = 1, "uv" = microvolts
 * Polled, no interrupts; every stage is printed.
 */
#include <stddef.h>
#include <stdint.h>
#include "console.h"
#include "drg_rpm.h"

#define RPM_MSGRAM		0x00778000UL
#define RPM_MSGRAM_SIZE		0x7000UL
#define RPM_IPC			0x17911008UL
#define RPM_IPC_MASK		0x1u
#define TOC_MAGIC		0x67727430u
#define TOC_TX			0x61703272u
#define TOC_RX			0x72326170u

#define CMD_VERSION		0
#define CMD_VERSION_ACK		1
#define CMD_OPEN		2
#define CMD_OPEN_ACK		4
#define CMD_TX_DATA		9

#define LCID			1

static void delay_ms(unsigned int ms)
{
	uint64_t frq, t0, t;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
	do {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
	} while (t - t0 < frq / 1000 * ms);
}

static inline uint32_t r32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void w32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uintptr_t tx_desc, tx_fifo, rx_desc, rx_fifo;
static uint32_t tx_size, rx_size;

static int fifo_find(uint32_t id, uintptr_t *desc, uintptr_t *fifo, uint32_t *size)
{
	uintptr_t toc = RPM_MSGRAM + RPM_MSGRAM_SIZE - 256;
	uint32_t n = r32(toc + 4);
	if (r32(toc) != TOC_MAGIC || n > 20)
		return -1;
	for (uint32_t i = 0; i < n; i++) {
		uintptr_t e = toc + 8 + 12 * i;
		if (r32(e) != id)
			continue;
		*desc = RPM_MSGRAM + r32(e + 4);
		*fifo = *desc + 8;
		*size = r32(e + 8);
		if (*fifo + *size > RPM_MSGRAM + RPM_MSGRAM_SIZE)
			return -2;
		return 0;
	}
	return -3;
}

/* Write len bytes (multiple of 8) of words; 0 or -1 if there is no room. */
static int fifo_write(const uint32_t *w, uint32_t len)
{
	uint32_t rd = r32(tx_desc + 0), wr = r32(tx_desc + 4);
	uint32_t avail = rd <= wr ? tx_size - wr + rd : rd - wr;
	if (rd >= tx_size || wr >= tx_size || avail < len + 8)
		return -1;
	for (uint32_t i = 0; i < len / 4; i++) {
		w32(tx_fifo + wr, w[i]);
		wr += 4;
		if (wr >= tx_size)
			wr = 0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	w32(tx_desc + 4, wr);
	__asm__ volatile("dsb sy" ::: "memory");
	w32(RPM_IPC, RPM_IPC_MASK);
	return 0;
}

static uint32_t rx_avail(void)
{
	uint32_t rd = r32(rx_desc + 0), wr = r32(rx_desc + 4);
	if (rd >= rx_size || wr >= rx_size)
		return 0;
	return wr >= rd ? wr - rd : rx_size - rd + wr;
}

/* Read len bytes (multiple of 4) into w (NULL: skip). */
static void fifo_read(uint32_t *w, uint32_t len)
{
	uint32_t rd = r32(rx_desc + 0);
	for (uint32_t i = 0; i < len / 4; i++) {
		uint32_t v = r32(rx_fifo + rd);
		if (w)
			w[i] = v;
		rd += 4;
		if (rd >= rx_size)
			rd = 0;
	}
	w32(rx_desc + 0, rd);
}

static uint32_t st_version_acked, st_open_acked, st_remote_open, st_data_words[4], st_data_seen;

/* Handle everything the RPM has sent so far. */
static void rx_poll(void)
{
	while (rx_avail() >= 8) {
		uint32_t c[2];
		fifo_read(c, 8);
		uint32_t id = c[0] & 0xffff, p1 = c[0] >> 16, p2 = c[1];
		if (id == CMD_VERSION) {
			uint32_t ack[2] = { CMD_VERSION_ACK | (1u << 16), p2 & 0x4 };
			(void)fifo_write(ack, 8);
		} else if (id == CMD_VERSION_ACK) {
			st_version_acked = 1;
		} else if (id == CMD_OPEN) {
			fifo_read(NULL, (p2 + 7) & ~7u);        /* channel name */
			uint32_t ack[2] = { CMD_OPEN_ACK | (p1 << 16), 0 };
			(void)fifo_write(ack, 8);
			st_remote_open = 1;
		} else if (id == CMD_OPEN_ACK) {
			if (p1 == LCID)
				st_open_acked = 1;
		} else if (id == CMD_TX_DATA) {
			uint32_t h[2];
			fifo_read(h, 8);                        /* size, size_left */
			uint32_t size = h[0], padded = (size + 7) & ~7u;
			for (uint32_t i = 0; i < padded / 4; i++) {
				uint32_t v;
				fifo_read(&v, 4);
				if (i < 4)
					st_data_words[i] = v;
			}
			st_data_seen++;
		}
		/* other commands are 8 bytes and need no answer here */
	}
}

static int wait_for(uint32_t *flag, unsigned int ms)
{
	for (unsigned int i = 0; i < ms; i++) {
		rx_poll();
		if (*flag)
			return 0;
		delay_ms(1);
	}
	return -1;
}

static int link_up;
static uint32_t next_msg_id = 1;

/* VERSION and OPEN of "rpm_requests", once per boot; 0 when the channel is open */
static int link_open(void)
{
	if (link_up)
		return 0;
	int rc = fifo_find(TOC_TX, &tx_desc, &tx_fifo, &tx_size);
	int rc2 = fifo_find(TOC_RX, &rx_desc, &rx_fifo, &rx_size);
	con_hex2("RPM TOC", "tx=", (uint32_t)(tx_desc - RPM_MSGRAM) | tx_size << 16,
	    "rx=", (uint32_t)(rx_desc - RPM_MSGRAM) | rx_size << 16);
	if (rc || rc2)
		return -1;
	con_hex2("RPM IDX", "tx r/w=", r32(tx_desc) << 16 | r32(tx_desc + 4),
	    "rx r/w=", r32(rx_desc) << 16 | r32(rx_desc + 4));

	/* As the Linux transport does at probe: our own indices start at 0. */
	w32(tx_desc + 4, 0);
	w32(rx_desc + 0, 0);

	uint32_t ver[2] = { CMD_VERSION | (1u << 16), 0x4 };
	if (fifo_write(ver, 8))
		return -2;
	int v = wait_for(&st_version_acked, 500);

	uint32_t open[6] = { CMD_OPEN | (LCID << 16), 13, 0, 0, 0, 0 };
	const char *name = "rpm_requests";
	for (int i = 0; i < 13; i++)
		((char *)&open[2])[i] = name[i];
	if (fifo_write(open, 24))
		return -3;
	int o = wait_for(&st_open_acked, 500);
	(void)wait_for(&st_remote_open, 200);
	con_hex2("RPM LINK", "ver/open/ropen=",
	    (uint32_t)(v == 0) << 8 | (uint32_t)(o == 0) << 4 | st_remote_open,
	    "rx r/w=", r32(rx_desc) << 16 | r32(rx_desc + 4));
	if (o)
		return -4;
	link_up = 1;
	return 0;
}

/*
 * One v0 request in the active set with two 4-byte KVPs: header 28 bytes +
 * two KVPs of 12 bytes = 52, padded to 56. Waits for the RPM's answer.
 */
int drg_rpm_request2(uint32_t type, uint32_t id, uint32_t k1, uint32_t v1, uint32_t k2, uint32_t v2)
{
	int rc = link_open();
	if (rc)
		return rc;
	uint32_t pkt[4 + 14] = {
		CMD_TX_DATA | (LCID << 16), 0, 52, 0,
		0x716572,               /* service 'req' */
		20 + 24,                /* request_len: after the first two words */
		next_msg_id++,          /* msg_id */
		0,                      /* active set */
		type, id,
		24,                     /* data_len */
		k1, 4, v1,
		k2, 4, v2,
		0,
	};
	uint32_t before = st_data_seen;
	if (fifo_write(pkt, sizeof(pkt)))
		return -5;
	uint32_t seen = 0;
	for (int i = 0; i < 500 && !seen; i++) {
		rx_poll();
		seen = st_data_seen != before;
		delay_ms(1);
	}
	con_hex2("RPM ACK", "n=", st_data_seen, "w0..1=", st_data_words[0]);
	con_hex2("RPM ACK", "w2=", st_data_words[2], "w4=", st_data_words[3]);
	return seen ? 0 : -6;
}

int drg_rpm_ldo_enable(uint32_t type, uint32_t id, uint32_t uv)
{
	return drg_rpm_request2(type, id, RPM_KEY_SWEN, 1, RPM_KEY_UV, uv);
}
