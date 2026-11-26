/*
 * bt-twrp: the shim's B2 Bluetooth firmware download (shim/drg_bt.c b2_run), done again in TWRP
 * through Nokia's own kernel drivers: /dev/btpower (bluetooth-power.c, BT_CMD_PWR_CTRL) for the
 * supplies and /dev/ttyHS0 (msm_serial_hs, the same BLSP2 UART1 at 0xc1af000) with the kernel's
 * hardware flow control. Boot #57: in the shim all 480 patch segments went out but the last one
 * got no answer (UART SR 0xc8d, RX ready). Here every received byte is logged, so the run tells
 * whether the chip answers the last segment when the UART is Linux's (then the shim's UART path
 * loses it) or not (then the protocol is wrong).
 *   bt-twrp PATCH NVM      (crbtfw21.tlv, crnv21.bin from bluetooth_a, pushed by hand)
 * Same sequence as the shim: power pulses 0xC0 @2400 / 0xFC @115200, HCI reset, EDL version,
 * patch in 243-byte EDL_PATCH_TLV_REQ segments (0xFC00 {0x1E, len, data}), NVM (HMD's tag 17/27
 * edits, baud code 0 = 115200), HCI reset, Read_BD_ADDR, Read_Local_Version, EDL version.
 * Our own code; Linux ABI numbers from the kernel headers.
 */

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned int u32;
typedef unsigned char u8;

extern void __libc_init(void *raw_args, void (*onexit)(void), int (*slingshot)(int, char **, char **),
			const void *structors);
extern int printf(const char *, ...);
extern int fflush(void *);
extern int open(const char *, int, ...);
extern ssize_t read(int, void *, size_t);
extern ssize_t write(int, const void *, size_t);
extern int close(int);
extern int ioctl(int, int, ...);
extern int usleep(unsigned);
extern int clock_gettime(int, void *);
extern int *__errno(void);
#define O_RDONLY 0
#define O_RDWR 2
#define O_NOCTTY 0400
#define O_NONBLOCK 04000

/* Linux arm64 termios (asm-generic): struct termios for TCGETS/TCSETS */
struct ktermios { u32 iflag, oflag, cflag, lflag; u8 line, cc[19]; };
#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSBRK 0x5409
#define TCFLSH 0x540B
#define CBAUD 0010017
#define B2400 0000013
#define B115200 0010002
#define CS8 0000060
#define CREAD 0000200
#define CLOCAL 0004000
#define CRTSCTS 020000000000u
#define VTIME 5
#define VMIN 6
#define MSM_ENABLE_UART_CLOCK 0x5441	/* msm_serial_hs.h TIOCPMGET */
#define BT_CMD_PWR_CTRL 0xbfad		/* bluetooth-power.h */

static int tty = -1;

static long ms_now(void)
{
	long ts[2];
	clock_gettime(1, ts);	/* CLOCK_MONOTONIC */
	return ts[0] * 1000 + ts[1] / 1000000;
}

static int set_line(u32 baud, int flow)
{
	struct ktermios t;
	if (ioctl(tty, TCGETS, &t))
		return -1;
	t.iflag = t.oflag = t.lflag = 0;
	t.cflag = baud | CS8 | CREAD | CLOCAL | (flow ? CRTSCTS : 0);
	t.cc[VMIN] = 0;
	t.cc[VTIME] = 0;
	return ioctl(tty, TCSETS, &t);
}

static void drain(void) { (void)ioctl(tty, TCSBRK, 1); }

static int tx(const u8 *b, u32 n)
{
	u32 done = 0;
	long t0 = ms_now();
	while (done < n) {
		ssize_t w = write(tty, b + done, n - done);
		if (w > 0)
			done += (u32)w;
		else if (ms_now() - t0 > 2000)
			return -1;
		else
			usleep(200);
	}
	return 0;
}

/* receive buffer and H4 parser (the shim's parse_rx, plus a raw byte log) */
static u8 rx[4096];
static u32 rxn, raw_total, logged_bytes;
static struct { u32 vse_ok, vse_bad, cc, other, sleep, wake, ack, junk, last_res; } es;
static u8 last_ev[32];
static u32 last_ev_n, want_op;
#define SEEN_VSE 1
#define SEEN_CC 2
#define SEEN_OTHER 4
#define SEEN_ACK 8
#define SEEN_VSE_OTHER 16

static u32 le16p(const u8 *p) { return (u32)(p[0] | p[1] << 8); }
static u32 le32p(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

static void log_ev(const char *what, const u8 *e, u32 n)
{
	printf("  %s:", what);
	for (u32 i = 0; i < n && i < 24; i++)
		printf(" %02x", e[i]);
	printf("\n");
}

static u32 parse(void)
{
	u32 p = 0, seen = 0;
	while (p < rxn) {
		u8 b = rx[p];
		if (b == 0x04) {
			if (p + 3 > rxn || p + 3u + rx[p + 2] > rxn)
				break;
			const u8 *e = rx + p;
			u32 n = 3u + e[2];
			last_ev_n = n < sizeof(last_ev) ? n : sizeof(last_ev);
			for (u32 k = 0; k < last_ev_n; k++)
				last_ev[k] = e[k];
			if (e[1] == 0xff && n >= 6 && e[3] == 0x00 && e[4] == 0x04) {
				es.last_res = e[5];
				if (e[5] == 0)
					es.vse_ok++;
				else
					es.vse_bad++;
				seen |= SEEN_VSE;
			} else if (e[1] == 0xff) {
				seen |= SEEN_VSE_OTHER;
			} else if (e[1] == 0x0e && n >= 6) {
				es.cc++;
				if (le16p(e + 4) == want_op)
					seen |= SEEN_CC;
			} else {
				es.other++;
				seen |= SEEN_OTHER;
			}
			if (es.vse_ok + es.vse_bad + es.cc + es.other <= 40 || e[1] != 0xff)
				log_ev("event", e, n);
			p += n;
		} else if (b == 0xfe) {
			es.sleep++, p++;
		} else if (b == 0xfd) {
			static const u8 wack[1] = { 0xfc };
			es.wake++, p++;
			(void)tx(wack, 1);
		} else if (b == 0xfc) {
			es.ack++, p++;
			seen |= SEEN_ACK;
		} else {
			if (es.junk++ < 16)
				printf("  junk byte %02x\n", b);
			p++;
		}
	}
	for (u32 k = p; k < rxn; k++)
		rx[k - p] = rx[k];
	rxn -= p;
	return seen;
}

static u32 pump(u32 want, u32 ms)
{
	long t0 = ms_now();
	u32 seen = 0;
	do {
		ssize_t r = read(tty, rx + rxn, sizeof(rx) - rxn);
		if (r > 0) {
			raw_total += (u32)r;
			if (logged_bytes < 64) {
				printf("  rx %ld bytes:", (long)r);
				for (ssize_t i = 0; i < r && i < 24; i++)
					printf(" %02x", rx[rxn + i]);
				printf("\n");
				logged_bytes += (u32)r;
			}
			rxn += (u32)r;
			seen |= parse();
			if (seen & want)
				break;
		} else {
			usleep(500);
		}
	} while (ms_now() - t0 < (long)ms);
	return seen;
}

static int cmd(const char *what, const u8 *c, u32 n, u32 ms)
{
	(void)pump(0, 2);
	want_op = le16p(c + 1);
	last_ev_n = 0;
	if (tx(c, n))
		return printf("%s: write failed\n", what), -1;
	u32 s = pump(want_op == 0xfc00 ? SEEN_VSE_OTHER | SEEN_VSE : SEEN_CC, ms);
	int ok = want_op == 0xfc00 ? (s & (SEEN_VSE_OTHER | SEEN_VSE)) != 0 : (s & SEEN_CC) != 0;
	printf("%s: %s\n", what, ok ? "answered" : "NO ANSWER");
	if (ok)
		log_ev("answer", last_ev, last_ev_n);
	return ok ? 0 : -1;
}

static u8 seg_cmd[6 + 243];

/* mode as the shim: 0 every segment acked, 1 only the last, 2 decide on the first */
static int download(const char *what, const u8 *f, u32 size, int mode)
{
	u32 nseg = (size + 242) / 243;
	long t0 = ms_now();
	es.vse_ok = es.vse_bad = 0;
	printf("%s: %u bytes, %u segments, mode %d\n", what, size, nseg, mode);
	for (u32 i = 0; i < nseg; i++) {
		u32 off = i * 243, n = size - off < 243 ? size - off : 243;
		int last = i + 1 == nseg;
		u32 wait = last ? 3000 : mode == 0 ? 500 : mode == 2 && i == 0 ? 150 : 0;
		(void)pump(0, 0);
		seg_cmd[0] = 0x01, seg_cmd[1] = 0x00, seg_cmd[2] = 0xfc, seg_cmd[3] = (u8)(n + 2);
		seg_cmd[4] = 0x1e, seg_cmd[5] = (u8)n;
		for (u32 k = 0; k < n; k++)
			seg_cmd[6 + k] = f[off + k];
		want_op = 0xfc00;
		u32 ok_before = es.vse_ok;
		if (tx(seg_cmd, 6 + n))
			return printf("%s: write failed at segment %u\n", what, i), -1;
		if (last)
			drain();
		long tw = ms_now();
		u32 seen = wait ? pump(SEEN_VSE, wait) : 0;
		if (last)
			printf("%s: last segment (%u bytes) answer after %ld ms (seen %x)\n", what, n, ms_now() - tw, seen);
		int rc = !wait ? 0 : es.vse_ok > ok_before ? 0 : es.vse_bad ? -3 : -2;
		if (mode == 2 && i == 0 && !last) {
			mode = rc == 0 ? 0 : 1;
			printf("%s: segment 0 %s -> mode %d (seen %x)\n", what, rc ? "not acked" : "acked", mode, seen);
			if (rc == -2)
				rc = 0;
		}
		if (rc) {
			printf("%s: segment %u (of %u) FAILED rc %d, last result %u, acks %u, rx total %u bytes\n", what, i,
			       nseg, rc, es.last_res, es.vse_ok, raw_total);
			/* anything late? */
			(void)pump(0, 2000);
			printf("%s: after 2 s more: acks %u bad %u cc %u other %u junk %u, rx total %u\n", what, es.vse_ok,
			       es.vse_bad, es.cc, es.other, es.junk, raw_total);
			return -(int)(1 + i);
		}
	}
	(void)pump(0, 20);
	printf("%s: done in %ld ms, acks %u, cc %u, other %u, rx total %u\n", what, ms_now() - t0, es.vse_ok, es.cc,
	       es.other, raw_total);
	return 0;
}

static int nvm_edit(u8 *f, u32 size)
{
	u32 tl = le32p(f), off = 4, end = 4 + (tl >> 8), edits = 0, blk_off = 0, blk_end = 0;
	if (end != size)
		return -1;
	if ((tl & 0xff) == 2) {
		blk_off = 4, blk_end = end;
	} else {
		while (off + 4 <= end) {
			u32 st = le32p(f + off), sl = st >> 8;
			if (off + 4 + sl > end)
				return -2;
			if ((st & 0xff) == 2) {
				blk_off = off + 4, blk_end = off + 4 + sl;
				break;
			}
			off += 4 + sl;
		}
	}
	if (!blk_end)
		return -3;
	for (u32 i = blk_off; i + 12 <= blk_end;) {
		u32 tag = le16p(f + i), len = le16p(f + i + 2);
		u8 *d = f + i + 12;
		if (i + 12 + len > blk_end)
			return -4;
		if (tag == 17 && len >= 3) {
			printf("nvm tag 17: %02x %02x %02x ->", d[0], d[1], d[2]);
			d[0] |= 0x80, d[2] = 0;
			printf(" %02x %02x %02x\n", d[0], d[1], d[2]);
			edits++;
		} else if (tag == 27 && len >= 1) {
			printf("nvm tag 27: %02x ->", d[0]);
			d[0] |= 1;
			printf(" %02x\n", d[0]);
			edits++;
		}
		i += 12 + len;
	}
	return edits == 2 ? 0 : -5;
}

static u8 patch[128 * 1024], nvm[16 * 1024];

static int load(const char *path, u8 *dst, u32 max)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	u32 n = 0;
	ssize_t r;
	while (n < max && (r = read(fd, dst + n, max - n)) > 0)
		n += (u32)r;
	close(fd);
	return (int)n;
}

static int run(const char *ppath, const char *npath)
{
	int psize = load(ppath, patch, sizeof(patch)), nsize = load(npath, nvm, sizeof(nvm));
	printf("patch %d bytes, nvm %d bytes, patch header product %04x rom %04x mode byte %u\n", psize, nsize,
	       psize > 24 ? le16p(patch + 16) : 0, psize > 24 ? le16p(patch + 18) : 0, psize > 24 ? patch[14] : 0);
	if (psize <= 24 || nsize <= 12)
		return 1;
	int pw = open("/dev/btpower", O_RDWR);
	if (pw < 0)
		return printf("FAIL: /dev/btpower errno %d\n", *__errno()), 1;
	printf("btpower off %d\n", ioctl(pw, BT_CMD_PWR_CTRL, 0));
	usleep(100000);
	printf("btpower on %d\n", ioctl(pw, BT_CMD_PWR_CTRL, 1));
	usleep(100000);
	tty = open("/dev/ttyHS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (tty < 0)
		return printf("FAIL: /dev/ttyHS0 errno %d\n", *__errno()), 1;
	printf("uart clock on %d\n", ioctl(tty, MSM_ENABLE_UART_CLOCK, 0));

	/* power pulses as the shim / upstream qca_wcn3990_init: 0xC0 at 2400, 0xFC at 115200, no flow control */
	static const u8 off_pulse[1] = { 0xc0 }, on_pulse[1] = { 0xfc };
	printf("line 2400 %d\n", set_line(B2400, 0));
	(void)tx(off_pulse, 1);
	drain();
	usleep(10000);
	printf("line 115200 %d\n", set_line(B115200, 0));
	(void)tx(on_pulse, 1);
	drain();
	usleep(100000);
	(void)ioctl(tty, TCFLSH, 0);
	printf("flow on %d\n", set_line(B115200, 1));
	(void)pump(0, 100);

	int rc = 0;
	static const u8 reset[4] = { 0x01, 0x03, 0x0c, 0x00 }, edl[5] = { 0x01, 0x00, 0xfc, 0x01, 0x19 };
	static const u8 ver[4] = { 0x01, 0x01, 0x10, 0x00 }, bdaddr[4] = { 0x01, 0x09, 0x10, 0x00 };
	if (cmd("hci reset", reset, 4, 1000) || cmd("edl version", edl, 5, 1000)) {
		rc = 1;
		goto out;
	}
	printf("nvm edit %d\n", nvm_edit(nvm, (u32)nsize));
	if (download("patch", patch, (u32)psize, patch[14] ? 2 : 0)) {
		rc = 1;
		goto out;
	}
	if (download("nvm", nvm, (u32)nsize, 0)) {
		rc = 1;
		goto out;
	}
	if (cmd("hci reset after patch", reset, 4, 1000))
		rc = 1;
	(void)cmd("read bd_addr", bdaddr, 4, 500);
	(void)cmd("read local version", ver, 4, 500);
	(void)cmd("edl version after", edl, 5, 1000);
	printf("ibs: sleep %u wake %u ack %u junk %u\n", es.sleep, es.wake, es.ack, es.junk);
out:
	close(tty);
	printf("btpower off %d\n", ioctl(pw, BT_CMD_PWR_CTRL, 0));
	close(pw);
	return rc;
}

static int main_(int argc, char **argv, char **envp)
{
	(void)envp;
	int rc = argc < 3 ? (printf("usage: bt-twrp PATCH NVM\n"), 2) : run(argv[1], argv[2]);
	printf("RESULT: %s\n", rc ? "FAIL" : "PASS");
	fflush(0);
	return rc;
}

__attribute__((used)) static void start_c(void *raw_args)
{
	static void *structors[6];
	__libc_init(raw_args, 0, main_, structors);
}
__asm__(".text\n.globl _start\n.type _start,%function\n_start:\n"
	"mov x29, #0\nmov x30, #0\nmov x0, sp\nb start_c\n");
