/*
 * drgpu RPM client: GPU votes (MX/CX levels, GPU bus bandwidth) over the
 * glink "rpm_requests" channel, polled. Same protocol as shim/drg_rpm.c,
 * which brought up the touch supply on this phone (boots #27-#29), from
 * HMD's DRG kernel (glink_smem_native_xprt.c, glink.c, rpm-smd.c):
 *   - message RAM 0x778000 + 0x7000 ends with a 256-byte TOC (magic
 *     0x67727430): TX FIFO 0x61703272, RX FIFO 0x72326170, each {read_index,
 *     write_index} followed by the data; 32-bit accesses, 8-byte units
 *   - after writing: the write index, then 1 to IPC 0x17911008
 *   - commands {u16 id, u16 p1, u32 p2}: VERSION 0, VERSION_ACK 1, OPEN 2,
 *     OPEN_ACK 4, TX_DATA 9 (+ size, size_left, data padded to 8)
 *   - v0 request: 'req', length, msg_id, set 0 (active), type, id, data
 *     length, then KVPs {key, nbytes, value}
 * Attach mode continues the channel the shim opened before XNU (it keeps
 * the shared indices; a second OPEN of the same channel is not sent).
 */
#include "drgpu.h"

#define TOC_MAGIC	0x67727430u
#define TOC_TX		0x61703272u
#define TOC_RX		0x72326170u
#define CMD_VERSION	0
#define CMD_VERSION_ACK	1
#define CMD_OPEN	2
#define CMD_OPEN_ACK	4
#define CMD_TX_DATA	9
#define LCID		1
#define IPC_OFF		0x8
#define IPC_MASK	0x1u
#define RAM_SIZE	0x7000u

static int fifo_find(struct drgpu *g, uint32_t id, uintptr_t *desc, uintptr_t *fifo, uint32_t *size)
{
	uintptr_t toc = g->rpm_ram + RAM_SIZE - 256;
	uint32_t n = drgpu_rd(toc + 4);
	if (drgpu_rd(toc) != TOC_MAGIC || n > 20)
		return -1;
	for (uint32_t i = 0; i < n; i++) {
		uintptr_t e = toc + 8 + 12 * i;
		if (drgpu_rd(e) != id)
			continue;
		*desc = g->rpm_ram + drgpu_rd(e + 4);
		*fifo = *desc + 8;
		*size = drgpu_rd(e + 8);
		return *fifo + *size > g->rpm_ram + RAM_SIZE ? -2 : 0;
	}
	return -3;
}

static int fifo_write(struct drgpu *g, const uint32_t *w, uint32_t len)
{
	uint32_t rd = drgpu_rd(g->rpm_tx_desc), wr = drgpu_rd(g->rpm_tx_desc + 4);
	uint32_t avail = rd <= wr ? g->rpm_tx_size - wr + rd : rd - wr;
	if (rd >= g->rpm_tx_size || wr >= g->rpm_tx_size || avail < len + 8)
		return -1;
	for (uint32_t i = 0; i < len / 4; i++) {
		drgpu_wr(g->rpm_tx_fifo + wr, w[i]);
		wr += 4;
		if (wr >= g->rpm_tx_size)
			wr = 0;
	}
	drgpu_wr(g->rpm_tx_desc + 4, wr);
	drgpu_wr(g->rpm_ipc + IPC_OFF, IPC_MASK);
	return 0;
}

static uint32_t rx_avail(struct drgpu *g)
{
	uint32_t rd = drgpu_rd(g->rpm_rx_desc), wr = drgpu_rd(g->rpm_rx_desc + 4);
	if (rd >= g->rpm_rx_size || wr >= g->rpm_rx_size)
		return 0;
	return wr >= rd ? wr - rd : g->rpm_rx_size - rd + wr;
}

static void fifo_read(struct drgpu *g, uint32_t *w, uint32_t len)
{
	uint32_t rd = drgpu_rd(g->rpm_rx_desc);
	for (uint32_t i = 0; i < len / 4; i++) {
		uint32_t v = drgpu_rd(g->rpm_rx_fifo + rd);
		if (w)
			w[i] = v;
		rd += 4;
		if (rd >= g->rpm_rx_size)
			rd = 0;
	}
	drgpu_wr(g->rpm_rx_desc, rd);
}

/* handle everything the RPM sent; returns bit 0 version acked, bit 1 open acked, bit 2 remote open */
static uint32_t rx_poll(struct drgpu *g)
{
	uint32_t seen = 0;
	while (rx_avail(g) >= 8) {
		uint32_t c[2];
		fifo_read(g, c, 8);
		uint32_t id = c[0] & 0xffff, p1 = c[0] >> 16, p2 = c[1];
		if (id == CMD_VERSION) {
			uint32_t ack[2] = { CMD_VERSION_ACK | 1u << 16, p2 & 0x4 };
			(void)fifo_write(g, ack, 8);
		} else if (id == CMD_VERSION_ACK) {
			seen |= 1;
		} else if (id == CMD_OPEN) {
			fifo_read(g, 0, (p2 + 7) & ~7u);	/* channel name */
			uint32_t ack[2] = { CMD_OPEN_ACK | p1 << 16, 0 };
			(void)fifo_write(g, ack, 8);
			seen |= 4;
		} else if (id == CMD_OPEN_ACK) {
			if (p1 == LCID)
				seen |= 2;
		} else if (id == CMD_TX_DATA) {
			uint32_t h[2];
			fifo_read(g, h, 8);	/* size, size_left */
			uint32_t padded = (h[0] + 7) & ~7u;
			for (uint32_t i = 0; i < padded / 4; i++) {
				uint32_t v;
				fifo_read(g, &v, 4);
				if (i < 4)
					g->rpm_last_ack[i] = v;
			}
			g->rpm_acks++;
		}
	}
	return seen;
}

int drgpu_rpm_attach(struct drgpu *g, int link_already_open)
{
	if (g->rpm_up)
		return 0;
	if (fifo_find(g, TOC_TX, &g->rpm_tx_desc, &g->rpm_tx_fifo, &g->rpm_tx_size) ||
	    fifo_find(g, TOC_RX, &g->rpm_rx_desc, &g->rpm_rx_fifo, &g->rpm_rx_size))
		return -1;
	g->rpm_msg_id = 0x1000;	/* apart from the shim's ids */
	if (link_already_open) {
		(void)rx_poll(g);	/* drain what the shim left */
		g->rpm_up = 1;
		return 0;
	}
	drgpu_wr(g->rpm_tx_desc + 4, 0);	/* as the Linux transport does at probe */
	drgpu_wr(g->rpm_rx_desc, 0);
	uint32_t ver[2] = { CMD_VERSION | 1u << 16, 0x4 };
	if (fifo_write(g, ver, 8))
		return -2;
	uint32_t st = 0;
	uint64_t end = drgpu_ticks() + drgpu_freq() / 2;
	while (!(st & 1) && drgpu_ticks() < end)
		st |= rx_poll(g);
	uint32_t open[6] = { CMD_OPEN | LCID << 16, 13, 0, 0, 0, 0 };
	const char *name = "rpm_requests";
	for (int i = 0; i < 13; i++)
		((char *)&open[2])[i] = name[i];
	if (fifo_write(g, open, 24))
		return -3;
	end = drgpu_ticks() + drgpu_freq() / 2;
	while (!(st & 2) && drgpu_ticks() < end)
		st |= rx_poll(g);
	if (!(st & 2))
		return -4;
	g->rpm_up = 1;
	return 0;
}

static int rpm_send(struct drgpu *g, uint32_t type, uint32_t id, const uint32_t *kvp, uint32_t kvp_words)
{
	if (!g->rpm_up || kvp_words > 16)
		return -1;
	uint32_t data = 28 + 4 * kvp_words, words = 4 + (data + 7) / 8 * 2;
	uint32_t pkt[4 + 7 + 16 + 1];
	for (uint32_t i = 0; i < words; i++)
		pkt[i] = 0;
	pkt[0] = CMD_TX_DATA | LCID << 16;
	pkt[2] = data;
	pkt[4] = 0x716572;			/* 'req' */
	pkt[5] = 20 + 4 * kvp_words;
	pkt[6] = g->rpm_msg_id++;
	pkt[7] = 0;				/* active set */
	pkt[8] = type;
	pkt[9] = id;
	pkt[10] = 4 * kvp_words;
	for (uint32_t i = 0; i < kvp_words; i++)
		pkt[11 + i] = kvp[i];
	uint32_t before = g->rpm_acks;
	if (fifo_write(g, pkt, 4 * words))
		return -5;
	uint64_t end = drgpu_ticks() + drgpu_freq() / 2;
	while (g->rpm_acks == before && drgpu_ticks() < end)
		(void)rx_poll(g);
	if (g->rpm_acks == before)
		return -6;
	/* an answer carrying only "msg#" (12 bytes) has no error KVP */
	return g->rpm_last_ack[1] == 12 ? 0 : -7;
}

int drgpu_rpm_kvp2(struct drgpu *g, uint32_t type, uint32_t id, uint32_t k1, uint32_t v1, uint32_t k2, uint32_t v2)
{
	const uint32_t kvp[6] = { k1, 4, v1, k2, 4, v2 };
	return rpm_send(g, type, id, kvp, 6);
}

int drgpu_rpm_kvp64(struct drgpu *g, uint32_t type, uint32_t id, uint32_t key, uint64_t v)
{
	const uint32_t kvp[4] = { key, 8, (uint32_t)v, (uint32_t)(v >> 32) };
	return rpm_send(g, type, id, kvp, 4);
}
