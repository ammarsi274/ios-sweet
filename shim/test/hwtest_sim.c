/*
 * Host simulation of the W1 (drg_w1.c) and B1 (drg_bt.c) hardware tests.
 * The real code runs unchanged except for the DRG_HOST_SIM seams (timer,
 * the SMC instruction, UART register access). Everything the code touches by
 * physical address is mapped at that address (MAP_FIXED_NOREPLACE, x86-64
 * non-PIE), and small models stand in for the hardware:
 *   - GCC branches: CLK_OFF follows the enable/vote bit
 *   - QDSP6SS: XO CBCR, BHS status
 *   - PBL/MBA: status 1 after the Q6 starts with RMB_MBA_IMAGE pointing at
 *     the real mba.mbn in MSS-owned memory; status 3 when META_DATA holds
 *     the real modem.mdt in MSS-owned memory; status 4 when CODE_LENGTH
 *     reaches the total and every segment matches its .bNN with a zero
 *     tail, the region MSS-only; 6 after 0xffffdead
 *   - TZ hyp_assign: argument layout checked word by word, ownership of
 *     each range tracked; a source list that does not match the current
 *     owners is refused, as TZ would
 *   - eMMC: a GPT whose modem_a is the saved modem_a.img (read-only)
 *   - RPM: logs each request
 *   - MSS restart register 0 at the start (boot #55); asserting it clears
 *     the RMB status registers
 *   - UART + WCN3990: see the model below (receiver timing, auto RFR,
 *     breaks, chip boot time)
 * Failure injection (env SIM_FAIL): pbl, mdt, seg, auth, smcN (Nth SMC
 * fails), probe (QDSP6 dry read faults), ram (RAM touch fails), mssrun (Q6
 * found running: must be left alone), mssstale (PBL/MBA status left from
 * an earlier boot: restart cycle, then a normal boot), btdead, btprobe.
 * B1 must end with rc 0 and the decoded SoC version unless btdead/btprobe:
 * the drg_bt.c of commit 430e9f8 fails here the way boot #55 did.
 *   gcc -O2 -Wall -Wextra -DDRG_HOST_SIM -DBOARD_DRG -static -no-pie -pthread -I.. \
 *       -o hwtest_sim hwtest_sim.c ../drg_w1.c ../drg_bt.c ../drg_fat.c
 *   ./hwtest_sim modem_a.img extracted-files-dir
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "drg_w1.h"
#include "drg_bt.h"

static const char *fail;
static int fail_is(const char *s) { return fail && !strcmp(fail, s); }
static int failures;
#define CHECK(c, ...) do { if (!(c)) { printf("SIM CHECK FAILED: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

/* ---- memory at physical addresses ---- */
static void map(uintptr_t a, size_t n, int fill)
{
	void *p = mmap((void *)a, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p != (void *)a) {
		fprintf(stderr, "map %lx: %s\n", (unsigned long)a, strerror(errno));
		exit(2);
	}
	memset(p, fill, n);
}
static inline volatile uint32_t *R(uintptr_t a) { return (volatile uint32_t *)a; }

#define GCC 0x100000UL
#define QDSP6 0x04080000UL
#define RMB 0x04180000UL
#define MODEM 0x8ac00000UL
#define MBA 0x94800000UL
#define META 0x94900000UL
#define FIH 0xac000000UL

/* ---- timer ---- */
uint64_t sim_now(void)
{
	static unsigned calls;
	if (++calls % 64 == 0)
		sched_yield();	/* the model thread must keep up with the code's 200 us branch polls on a busy host */
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint64_t sim_freq(void) { return 1000000000ull; }

/* ---- the log, formatted as shim.c cpu_put ---- */
static char logbuf[65536];
static unsigned loglen;
static void cpu_put(const char *label, uint32_t a, uint32_t v, int rc)
{
	char t[64];
	unsigned n = 0;
	while (*label && n < 24)
		t[n++] = *label++;
	n += (unsigned)sprintf(t + n, " %08x %08x %c%c\n", a, v, rc == 0 ? 'o' : 'x', rc == 0 ? 'k' : (char)('0' - rc));
	fwrite(t, 1, n, stdout);
	for (unsigned i = 0; i < n && loglen < sizeof(logbuf); i++)
		logbuf[loglen++] = t[i];
}
static unsigned flushes;
static void flush(void) { flushes++; }

/* ---- shim.c / console / rpm / emmc stubs ---- */
void con_str(const char *label, const char *s) { printf("[screen] %s %s\n", label, s); }
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	printf("[screen] %s %s%08x %s%08x\n", label, a, va, b ? b : "", vb);
}

static uintptr_t probe_fail_addr;
int drg_probe_read(int idx, uintptr_t addr, uint32_t *val, uint64_t *esr, unsigned timeout_us)
{
	(void)timeout_us;
	CHECK(idx >= 5 && idx <= 7, "probe on core %d", idx);
	if (addr == probe_fail_addr) {
		*esr = 0xbf000000;	/* SError */
		return -3;
	}
	*val = *R(addr);
	*esr = 0;
	return 0;
}
static unsigned probe_calls;
int drg_probe_call(int idx, void (*fn)(void), uint64_t *esr, unsigned timeout_us)
{
	(void)timeout_us;
	CHECK(idx >= 5 && idx <= 7, "probe call on core %d", idx);
	*esr = 0;
	if (++probe_calls == 2 && fail_is("ram")) {	/* the second RAM touch (metadata buffer) faults */
		*esr = 0xbf000000;
		return -3;
	}
	fn();
	return 0;
}

static uint32_t rpm_n;
static void fourcc(uint32_t v, char *s)
{
	for (int i = 0; i < 4; i++) {
		char c = (char)(v >> (8 * i));
		s[i] = c >= 32 && c < 127 ? c : '.';
	}
	s[4] = 0;
}
int drg_rpm_request_kvp(uint32_t type, uint32_t id, const uint32_t *kvp, uint32_t words)
{
	char t[5], k[5];
	fourcc(type, t);
	printf("[rpm] %s %u:", t, id);
	for (uint32_t i = 0; i + 3 <= words; i += 3) {
		fourcc(kvp[i], k);
		CHECK(kvp[i + 1] == 4, "kvp size");
		printf(" %s=%u", k, kvp[i + 2]);
	}
	printf("\n");
	CHECK(words % 3 == 0 && words <= 16, "kvp words %u", words);
	rpm_n++;
	return 0;
}
void drg_rpm_last_ack(uint32_t w[4])
{
	w[0] = 0x6b6361;	/* not known: logged as is */
	w[1] = 12;
	w[2] = rpm_n;
	w[3] = 0;
}

static FILE *img;
#define PART_FIRST 1000000u
#define PART_SECTORS 225280u
static uint32_t emmc_reads, emmc_sectors;
int drg_emmc_read(uint64_t lba, uint32_t count, void *buf)
{
	uintptr_t b = (uintptr_t)buf;
	if (count == 0 || count > 4096 || (b & 7) || b + (uint64_t)count * 512 > 0x100000000ULL)
		return -10;
	emmc_reads++;
	emmc_sectors += count;
	uint8_t *p = buf;
	memset(p, 0, (size_t)count * 512);
	if (lba == 1) {	/* GPT header */
		memcpy(p, "EFI PART", 8);
		uint64_t ent = 2;
		uint32_t n = 8, esz = 128;
		memcpy(p + 72, &ent, 8);
		memcpy(p + 80, &n, 4);
		memcpy(p + 84, &esz, 4);
		return 0;
	}
	if (lba == 2 || lba == 3) {	/* entries 0-7: modem_b at index 2, modem_a at index 5 */
		const char *names[8] = { "xbl_a", "modemst1", "modem_b", "fsg", "persist", "modem_a", "bluetooth_a",
					 "userdata" };
		for (int i = 0; i < 4; i++) {
			int idx = (int)(lba - 2) * 4 + i;
			uint8_t *e = p + 128 * i;
			uint64_t f = idx == 5 ? PART_FIRST : 10000u * (unsigned)(idx + 1), l = f + PART_SECTORS - 1;
			memcpy(e + 32, &f, 8);
			memcpy(e + 40, &l, 8);
			for (int k = 0; names[idx][k]; k++)
				e[56 + 2 * k] = (uint8_t)names[idx][k];
		}
		return 0;
	}
	if (lba >= PART_FIRST && lba + count <= PART_FIRST + PART_SECTORS) {
		if (fseeko(img, (off_t)(lba - PART_FIRST) * 512, SEEK_SET) || fread(p, 512, count, img) != count)
			return -5;
		return 0;
	}
	CHECK(0, "eMMC read outside the GPT and modem_a: lba %llu", (unsigned long long)lba);
	return -6;
}

/* ---- files from the saved partition (extracted, sha256-checked against a kernel FAT mount) ---- */
static const char *fdir;
static uint8_t *load(const char *name, size_t *n)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", fdir, name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *b = malloc(*n ? *n : 1);
	if (fread(b, 1, *n, f) != *n)
		exit(2);
	fclose(f);
	return b;
}

/* ---- TZ: hyp_assign with ownership ---- */
struct own {
	uint64_t a, s;
	uint32_t vms;	/* bit per VMID */
	uint32_t perm[32];
};
static struct own owns[16];
static int nown;
static unsigned smc_calls;

static struct own *own_of(uint64_t a, uint64_t s)
{
	for (int i = 0; i < nown; i++)
		if (owns[i].a == a && owns[i].s == s)
			return &owns[i];
	for (int i = 0; i < nown; i++)	/* overlap with a different range: not modelled */
		CHECK(a + s <= owns[i].a || a >= owns[i].a + owns[i].s, "assign overlaps an earlier range differently");
	owns[nown] = (struct own){ a, s, 1u << 3, { 0 } };
	owns[nown].perm[3] = 7;
	return &owns[nown++];
}
static uint32_t owners(uint64_t a, uint64_t s) { return own_of(a, s)->vms; }

void sim_smc(volatile uint64_t *x)
{
	smc_calls++;
	char fs[8];
	snprintf(fs, sizeof(fs), "smc%u", smc_calls);
	CHECK(x[0] == 0x42000c16, "x0 %llx", (unsigned long long)x[0]);
	CHECK(x[1] == 0x1117, "x1 %llx", (unsigned long long)x[1]);
	CHECK(x[3] == 16, "x3 %llu", (unsigned long long)x[3]);
	CHECK(x[2] < 1ull << 32 && x[4] < 1ull << 32 && x[5] < 1ull << 32, "arg buffers above 4G");
	const uint64_t *mp = (const uint64_t *)(uintptr_t)x[2];
	const uint32_t *src = (const uint32_t *)(uintptr_t)x[4];
	const uint64_t *ext = (const uint64_t *)(uintptr_t)x[5];
	uint32_t nsrc = (uint32_t)ext[0] / 4, ndst = (uint32_t)ext[2] / 24;
	CHECK(ext[0] % 4 == 0 && nsrc >= 1 && nsrc <= 2, "src bytes %llu", (unsigned long long)ext[0]);
	CHECK(ext[2] % 24 == 0 && ndst >= 1 && ndst <= 2, "dst bytes %llu", (unsigned long long)ext[2]);
	CHECK(ext[3] == 0 && ext[4] == 0 && ext[5] == 0 && ext[6] == 0, "extra args 3..6 not 0");
	const uint8_t *dst = (const uint8_t *)(uintptr_t)ext[1];
	uint64_t a = mp[0], s = mp[1];
	uint32_t want = 0, srcm = 0;
	printf("[tz] hyp_assign %08llx+%08llx src", (unsigned long long)a, (unsigned long long)s);
	for (uint32_t i = 0; i < nsrc; i++) {
		printf(" %u", src[i]);
		srcm |= 1u << (src[i] & 31);
	}
	printf(" ->");
	struct own *o = own_of(a, s);
	uint32_t perm[32] = { 0 };
	for (uint32_t i = 0; i < ndst; i++) {
		uint32_t vm, pm, cs;
		uint64_t ctx;
		memcpy(&vm, dst + 24 * i, 4);
		memcpy(&pm, dst + 24 * i + 4, 4);
		memcpy(&ctx, dst + 24 * i + 8, 8);
		memcpy(&cs, dst + 24 * i + 16, 4);
		CHECK(ctx == 0 && cs == 0, "dest ctx not 0");
		printf(" %u:%c%c%c", vm, pm & 4 ? 'r' : '-', pm & 2 ? 'w' : '-', pm & 1 ? 'x' : '-');
		want |= 1u << (vm & 31);
		perm[vm & 31] = pm;
	}
	int refuse = srcm != o->vms || fail_is(fs);
	printf("%s\n", refuse ? "  REFUSED" : "");
	if (refuse) {
		x[0] = (uint64_t)-1;
		return;
	}
	o->vms = want;
	memcpy(o->perm, perm, sizeof(perm));
	x[0] = 0;
	x[1] = x[2] = x[3] = 0;
}
static int mss_only(uint64_t a, uint64_t s) { return owners(a, s) == (1u << 15); }

/* ---- hardware models ---- */
static volatile int stop_models;
static uint8_t *mba_f, *mdt_f;
static size_t mba_n, mdt_n;
static uint64_t total_memsz;

static int seg_ok(void)
{
	uint32_t phoff, phnum;
	memcpy(&phoff, mdt_f + 28, 4);
	phnum = (uint32_t)(mdt_f[44] | mdt_f[45] << 8);
	for (uint32_t i = 0; i < phnum; i++) {
		uint32_t ph[8];
		memcpy(ph, mdt_f + phoff + 32 * i, 32);
		if (!(ph[0] == 1 && ((ph[6] >> 24) & 7) != 2 && ph[5]))
			continue;
		uint8_t *m = (uint8_t *)(uintptr_t)ph[3];	/* region starts at the image's own base */
		if (ph[4]) {
			char nm[16];
			size_t fn;
			snprintf(nm, sizeof(nm), "modem.b%02u", i);
			uint8_t *f = load(nm, &fn);
			if (!f || fn != ph[4] || memcmp(m, f, fn)) {
				printf("SIM segment %u content mismatch\n", i);
				free(f);
				return 0;
			}
			free(f);
		}
		for (uint32_t k = ph[4]; k < ph[5]; k++)
			if (m[k]) {
				printf("SIM segment %u tail not zero at %x\n", i, k);
				return 0;
			}
	}
	return 1;
}

static void setbit31(uintptr_t a, int on)
{
	if (on)
		__atomic_fetch_or((uint32_t *)a, 0x80000000u, __ATOMIC_SEQ_CST);
	else
		__atomic_fetch_and((uint32_t *)a, ~0x80000000u, __ATOMIC_SEQ_CST);
}

static void *model(void *arg)
{
	(void)arg;
	int pbl_done = 0, meta_done = 0, load_started = 0, auth_done = 0, unlocked = 0;
	while (!stop_models) {
		/* GCC branches: CLK_OFF (bit 31) follows the enable / vote bit */
		static const uint32_t br[] = { 0x8a000, 0x4401c, 0x8a03c, 0x8a004, 0x27004 };
		for (unsigned i = 0; i < sizeof(br) / 4; i++) {
			uint32_t v = *R(GCC + br[i]);
			setbit31(GCC + br[i], !(v & 1));	/* atomic: the code's enable-bit writes are never lost */
		}
		uint32_t vote = *R(GCC + 0x52004);
		*R(GCC + 0x38004) = (vote & (1u << 10)) ? 0 : 0x80000000u;	/* boot rom ahb */
		*R(GCC + 0x25004) = (vote & (1u << 15)) ? 0 : 0x80000000u;	/* blsp2 ahb */
		if (*R(GCC + 0x2700c) & 1)
			__atomic_fetch_and((uint32_t *)(GCC + 0x2700c), ~1u, __ATOMIC_SEQ_CST);	/* RCG update done */
		/* QDSP6SS */
		uint32_t cb = *R(QDSP6 + 0x38);
		setbit31(QDSP6 + 0x38, !(cb & 1));
		*R(QDSP6 + 0xc4) = (*R(QDSP6 + 0x30) >> 24) & 1;
		/* PBL / MBA */
		if (*R(GCC + 0x79000) & 1)	/* block reset: RMB statuses back to 0 */
			*R(RMB + 4) = 0, *R(RMB + 0xc) = 0;
		int running = !(*R(GCC + 0x79000) & 1) && !(*R(QDSP6 + 0x14) & 3) && (*R(QDSP6 + 0x20) & 2);
		if (running && !pbl_done && !fail_is("pbl") && !fail_is("mssrun")) {
			int ok = *R(RMB + 0) == MBA && mss_only(MBA, 0x100000) && !memcmp((void *)MBA, mba_f, mba_n);
			*R(RMB + 4) = ok ? 1 : 3;
			usleep(2000);
			*R(RMB + 0xc) = ok ? 1 : 0;
			pbl_done = 1;
		}
		uint32_t cmd = *R(RMB + 8);
		int32_t st = (int32_t)*R(RMB + 0xc);
		if (pbl_done && cmd == 1 && !meta_done) {
			uint32_t md = *R(RMB + 0x10);
			int ok = md == META && mss_only(META, (mdt_n + 0xfff) & ~0xfffu) &&
				 !memcmp((void *)(uintptr_t)md, mdt_f, mdt_n) && !fail_is("mdt");
			*R(RMB + 0xc) = ok ? 3 : (uint32_t)-2;
			meta_done = 1;
		}
		if (meta_done && cmd == 2 && !load_started) {
			CHECK(*R(RMB + 0x14) == MODEM, "CODE_START %x", *R(RMB + 0x14));
			load_started = 1;
		}
		if (load_started && fail_is("seg") && *R(RMB + 0x18) > 0x1000000 && st >= 0)
			*R(RMB + 0xc) = (uint32_t)-11;
		if (load_started && !auth_done && *R(RMB + 0x18) == total_memsz && mss_only(MODEM, 0x7600000)) {
			int ok = seg_ok() && !fail_is("auth");
			*R(RMB + 0xc) = ok ? 4 : (uint32_t)-5;
			auth_done = 1;
		}
		if (cmd == 0xffffdead && !unlocked) {
			*R(RMB + 0xc) = 6;
			unlocked = 1;
		}
		__asm__ volatile("pause");	/* spin: branch status must follow within the code's 200 us polls */
	}
	return NULL;
}

/* ---- UART + WCN3990 ----
 * Time-based model of the UART DM v1.4 receiver and the chip, built to
 * behave as boot #55 showed (logs/pull55/cpuprobe.txt):
 *   - bytes arrive one per character time (86.8 us at 115200) into a
 *     packing buffer; whole words go to the RX FIFO (RXRDY), the partial
 *     word only at a stale event (line idle for the IPR stale count of
 *     character times) or FORCE_STALE; RX_TOTAL_SNAP = bytes of the
 *     transfer at the stale event, a DMRX write starts the next transfer
 *   - auto RFR (MR1 RX_RDY_CTL): "ready" while FIFO + packing bytes <= the
 *     MR1 level; the chip still sends 5 bytes after RFR goes not-ready
 *     (its own FIFO; 1 + 5 = the 6 bytes boot #55 got with level 0) [model]
 *   - a break (chip TX low) sets ISR RXBREAK until RESET_BREAK_INT and puts
 *     a 0x00 into the FIFO unless MR2 RX_BREAK_ZERO_CHAR_OFF
 *   - chip after the 0xFC pulse: break at +0 and +5 ms, ignores commands
 *     until +60 ms (env SIM_BTBOOT_MS) [model: boot time unknown, boot #55 only shows it is
 *     more than a few ms and less than ~1 s], answers 1 ms after a command
 *   - EDL answer: 04 ff 0e 00 02 + product 0x0a (seen on boot #55), patch,
 *     rom 0x0201, soc id (invented values: crbtfw21)
 */
static uint32_t ur[0x200 / 4];
#define FIFO_WORDS 16				/* 64 bytes (uport->fifosize) */
static uint32_t fw_[FIFO_WORDS], fwn_[FIFO_WORDS];	/* RX FIFO: word, valid bytes */
static unsigned fh, ft;				/* FIFO head/tail (count = ft - fh) */
static uint32_t pk, pkn;			/* packing buffer */
static uint32_t xfer, snap_latch, stale_pend, stale_en, brk_latch, overrun, rfr_manual;
static uint64_t last_rx_t;
static unsigned ncf, txn, chip_on, chip_slack;
static uint64_t chip_boot_t;
static uint8_t txb[64];
static unsigned bt_cmds, bt_ignored;
struct cq { uint64_t t; int16_t b; };		/* b = -1: break */
static struct cq cq[512];
static unsigned cqh, cqt;
static uint64_t chip_next_t;

static uint64_t byte_ns(void) { return ur[0xa0 / 4] == 0x33 ? 4166667 : 86806; }

static void fifo_push(uint32_t w, uint32_t n)
{
	if (ft - fh >= FIFO_WORDS) {
		overrun = 1;
		return;
	}
	fw_[ft % FIFO_WORDS] = w, fwn_[ft % FIFO_WORDS] = n;
	ft++;
}
static void rx_byte(uint8_t b, uint64_t t)
{
	pk |= (uint32_t)b << (8 * pkn);
	if (++pkn == 4) {
		fifo_push(pk, 4);
		pk = pkn = 0;
	}
	xfer++;
	last_rx_t = t;
}
static void fire_stale(void)
{
	if (pkn) {
		fifo_push(pk, pkn);
		pk = pkn = 0;
	}
	snap_latch = xfer;
	stale_pend = 1;
	stale_en = 0;
}
static unsigned rx_level(void) { return (ft - fh) * 4 + pkn; }
static int rfr_ready(void)
{
	uint32_t mr1 = ur[0];
	if (!(mr1 & 0x80))
		return rfr_manual;
	uint32_t lvl = (mr1 & 0x3f) | ((mr1 & 0xffffff00u) >> 2);
	return rx_level() <= lvl;
}
static void chip_queue(const uint8_t *b, unsigned n, uint64_t t)
{
	for (unsigned i = 0; i < n; i++)
		cq[(cqt++) & 511] = (struct cq){ t, b ? b[i] : -1 };
}
static void tick(void)
{
	uint64_t t = sim_now();
	while (cqh != cqt) {
		uint64_t due = cq[cqh & 511].t > chip_next_t ? cq[cqh & 511].t : chip_next_t;
		if (t < due)
			break;
		if (rfr_ready())
			chip_slack = 5;
		else if (!chip_slack) {
			chip_next_t = t;	/* stalled until RFR is ready again */
			break;
		} else
			chip_slack--;
		int16_t b = cq[(cqh++) & 511].b;
		if (b < 0) {
			brk_latch = 1;
			if (!(ur[1] & 0x100))
				rx_byte(0, due);
		} else {
			rx_byte((uint8_t)b, due);
		}
		chip_next_t = due + byte_ns();
	}
	uint32_t ipr = ur[0x18 / 4], stale_chars = (ipr & 0x1f) | ((ipr & 0xffffff80u) >> 2);
	if (stale_en && !stale_pend && xfer > 0 && t - last_rx_t > (uint64_t)stale_chars * byte_ns())
		fire_stale();
}

static void chip_frame(void)
{
	uint32_t csr = ur[0xa0 / 4];
	printf("[chip] got %u byte(s) at CSR %02x:", txn, csr);
	for (unsigned i = 0; i < txn; i++)
		printf(" %02x", txb[i]);
	uint64_t t = sim_now();
	const char *bm = getenv("SIM_BTBOOT_MS");	/* chip boot time, default 60 ms */
	int ready = chip_on && t - chip_boot_t >= (bm ? strtoull(bm, NULL, 0) : 60) * 1000000ull;
	if (txn == 1 && txb[0] == 0xc0) {
		CHECK(csr == 0x33, "power-off pulse not at 2400");
		chip_on = 0;
		cqh = cqt;
	} else if (txn == 1 && txb[0] == 0xfc) {
		CHECK(csr == 0xcc, "power-on pulse not at 115200");
		chip_on = !fail_is("btdead");
		chip_boot_t = t;
		if (chip_on) {
			chip_queue(NULL, 1, t);
			chip_queue(NULL, 1, t + 5000000);
		}
	} else if (ready && csr == 0xcc && txn >= 4 && txb[0] == 1) {
		bt_cmds++;
		uint16_t op = (uint16_t)(txb[1] | txb[2] << 8);
		uint64_t at = t + 1000000;
		if (op == 0x0c03) {
			const uint8_t ev[] = { 4, 0x0e, 4, 1, 0x03, 0x0c, 0 };
			chip_queue(ev, sizeof(ev), at);
		} else if (op == 0x1001) {
			const uint8_t ev[] = { 4, 0x0e, 0x0c, 1, 0x01, 0x10, 0, 0x08, 0x00, 0x00, 0x08, 0x1d, 0x00, 0x01, 0x02 };
			chip_queue(ev, sizeof(ev), at);
		} else if (op == 0xfc00) {
			const uint8_t ev[] = { 4, 0xff, 0x0e, 0x00, 0x02, 0x0a, 0x00, 0x00, 0x00, 0x2d, 0x00, 0x01, 0x02,
					       0x14, 0x02, 0x01, 0x40 };
			chip_queue(ev, sizeof(ev), at);
		}
	} else if (chip_on) {
		bt_ignored++;
		printf("  (ignored: chip %s)", ready ? "?" : "still booting");
	}
	printf("\n");
	txn = 0;
}
uint32_t sim_urd(uint32_t o)
{
	tick();
	switch (o) {
	case 0xa4:	/* SR: TXRDY, TXEMT always; RXRDY with a word in the FIFO; OVERRUN */
		return 0xc | (ft != fh ? 1 : 0) | (overrun ? 0x10 : 0);
	case 0xb4:
	case 0xac: {
		uint32_t isr = (stale_pend ? 0x8 : 0) | (brk_latch ? 0x4 : 0) | (chip_on ? 0x40 : 0) |
			       (ft - fh > ur[0x20 / 4] ? 0x10 : 0);
		return o == 0xac ? isr & ur[0xb0 / 4] : isr;
	}
	case 0xbc:
		return snap_latch;
	case 0x140: {
		if (ft == fh)
			return 0;
		uint32_t w = fw_[fh % FIFO_WORDS];
		fh++;
		return w;
	}
	default:
		return o < 0x200 ? ur[o / 4] : 0;
	}
}
void sim_uwr(uint32_t o, uint32_t v)
{
	tick();
	if (o == 0xa8) {	/* CR */
		switch (v) {
		case 0x10: fh = ft = 0, pk = pkn = 0, xfer = snap_latch = 0, stale_pend = stale_en = 0; break;
		case 0x30: overrun = 0; break;
		case 0x40: brk_latch = 0; break;
		case 0x80: stale_pend = 0; break;
		case 0x400: fire_stale(); break;
		case 0x500: stale_en = 1; break;
		case 0xd0: rfr_manual = 1; break;
		case 0xe0: rfr_manual = 0; break;
		}
		return;
	}
	if (o == 0x34) {	/* DMRX: next transfer; bytes after the latched snap carry over */
		xfer -= snap_latch;
		snap_latch = 0;
		return;
	}
	if (o == 0x40) {
		ncf = v;
		txn = 0;
		return;
	}
	if (o == 0x100) {
		for (int k = 0; k < 4 && ncf; k++, ncf--)
			txb[txn++ & 63] = (uint8_t)(v >> (8 * k));
		if (!ncf)
			chip_frame();
		return;
	}
	if (o < 0x200)
		ur[o / 4] = v;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s modem_a.img files-dir\n", argv[0]);
		return 2;
	}
	fail = getenv("SIM_FAIL");
	img = fopen(argv[1], "rb");
	fdir = argv[2];
	mba_f = load("mba.mbn", &mba_n);
	mdt_f = load("modem.mdt", &mdt_n);
	if (!img || !mba_f || !mdt_f)
		return 2;
	{	/* total memsz of the loadable segments, for the MBA model */
		uint32_t phoff, phnum = (uint32_t)(mdt_f[44] | mdt_f[45] << 8);
		memcpy(&phoff, mdt_f + 28, 4);
		for (uint32_t i = 0; i < phnum; i++) {
			uint32_t ph[8];
			memcpy(ph, mdt_f + phoff + 32 * i, 32);
			if (ph[0] == 1 && ((ph[6] >> 24) & 7) != 2 && ph[5])
				total_memsz += ph[5];
		}
	}
	map(GCC, 0x100000, 0);
	map(0x01f63000, 0x3000, 0);
	map(0x01fe5000, 0x1000, 0);
	map(0x03500000, 0x14000, 0);
	map(QDSP6, 0x1000, 0);
	map(RMB, 0x1000, 0);
	map(0x0c1af000, 0x1000, 0);
	map(0x17a00000, 0x10000, 0);
	map(MODEM, 0x7e00000, 0xee);	/* garbage: the tails must be zeroed */
	map(MBA, 0x200000, 0xee);
	map(FIH, 0x800000, 0xee);
	*R(GCC + 0x79000) = 0;			/* boot #55: restart register reads 0 */
	if (fail_is("mssrun"))			/* Q6 running: BHS on, core out of reset, clock on */
		*R(QDSP6 + 0x30) = 1u << 24, *R(QDSP6 + 0x14) = 0, *R(QDSP6 + 0x20) = 2, *R(QDSP6 + 0xc4) = 1;
	if (fail_is("mssstale"))		/* statuses left by an earlier boot, Q6 stopped */
		*R(RMB + 4) = 1, *R(RMB + 0xc) = 4;
	*R(GCC + 0x52004) = 0x00000400;		/* boot rom vote already on: must stay on */
	*R(GCC + 0x5200c) = 0;
	*R(0x01f63004) = 1, *R(0x01f64004) = 1, *R(0x01f65004) = 1;	/* halt acks */
	*R(0x03510000) = 0x203, *R(0x03511000) = 0x3, *R(0x03512000) = 0x3, *R(0x03513000) = 0x3;	/* "sleep" pins */
	*R(GCC + 0x27010) = 0x0000;		/* RCG CFG: XO */
	if (fail_is("probe"))
		probe_fail_addr = QDSP6 + 0x14;
	if (fail_is("btprobe"))
		probe_fail_addr = 0x0c1af000 + 0xa4;

	pthread_t th;
	pthread_create(&th, NULL, model, NULL);

	uint32_t vote0 = *R(GCC + 0x52004), pins0 = *R(0x03510000);
	int w1 = drg_w1_test(cpu_put, flush);
	printf("== drg_w1_test -> %d (flushes %u, eMMC reads %u, sectors %u, SMCs %u)\n", w1, flushes, emmc_reads,
	       emmc_sectors, smc_calls);
	int bt = drg_bt_test(cpu_put, flush);
	printf("== drg_bt_test -> %d (HCI commands answered %u)\n", bt, bt_cmds);
	stop_models = 1;
	pthread_join(th, NULL);

	/* end state */
	printf("== ownership at the end\n");
	for (int i = 0; i < nown; i++)
		printf("   %08llx+%08llx vms %04x\n", (unsigned long long)owns[i].a, (unsigned long long)owns[i].s, owns[i].vms);
	if (fail_is("mssrun"))
		CHECK(!(*R(GCC + 0x79000) & 1) && w1 == -9 && smc_calls == 0 && *R(QDSP6 + 0x14) == 0,
		      "running Q6 touched (restart %x, rc %d, SMCs %u)", *R(GCC + 0x79000), w1, smc_calls);
	else if (w1 < 0 && w1 > -8)	/* stopped before step 8: the restart register is untouched */
		CHECK(*R(GCC + 0x79000) == 0, "restart register written before step 8");
	else
		CHECK(*R(GCC + 0x79000) & 1, "MSS not back in reset");
	if (fail_is("mssstale"))
		CHECK(w1 == 0, "stale RMB state: W1 rc %d", w1);
	if (!fail_is("btdead") && !fail_is("btprobe")) {
		CHECK(bt == 0, "B1 rc %d", bt);
		logbuf[loglen < sizeof(logbuf) ? loglen : sizeof(logbuf) - 1] = 0;
		CHECK(strstr(logbuf, "bt rome version 02140201") != NULL, "SoC version not decoded");
		CHECK(strstr(logbuf, "bt fw crbtfw/crnv NN [E] 00000021 00000001 ok") != NULL, "firmware number");
	}
	CHECK(*R(GCC + 0x52004) == vote0, "GCC vote register %x, was %x", *R(GCC + 0x52004), vote0);
	CHECK(!(*R(GCC + 0x8a000) & 1) && !(*R(GCC + 0x4401c) & 1) && !(*R(GCC + 0x8a03c) & 1) &&
		      !(*R(GCC + 0x8a004) & 1) && !(*R(GCC + 0x5200c) & 4),
	      "MSS clocks left on");
	CHECK(*R(0x03510000) == pins0, "BT pins not restored");
	CHECK(!(*R(GCC + 0x27004) & 1), "UART clock left on");
	if (nown) {
		CHECK(own_of(MBA, 0x100000)->vms == 8, "MBA buffer not back with HLOS");
		CHECK(owners(META, 0x3000) == 8 || w1 > -13, "metadata buffer not back with HLOS");
	}
	if (w1 == 0)
		CHECK(mss_only(MODEM, 0x7600000), "region not MSS-only after success");
	else if (w1 <= -14)
		CHECK(owners(MODEM, 0x7600000) == 8, "region not back with HLOS after a failure");
	unsigned lines = 0;
	for (unsigned i = 0; i < loglen; i++)
		lines += logbuf[i] == '\n';
	printf("== log %u bytes, %u lines (the shim's cpu_log holds 32768 with the CPU probe's lines before)\n", loglen,
	       lines);
	printf("%s (%d check failures)\n", failures ? "SIM FAIL" : "SIM PASS", failures);
	return failures ? 1 : 0;
}
