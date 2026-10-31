/*
 * Host test of drg-kmsg's append logic: a kernel log stream seen through a
 * circular buffer of SIZE bytes, read at random moments. The concatenated
 * output must equal the stream exactly (repeated identical lines included)
 * when no read misses more than the buffer holds, and must mark the loss
 * when one does.
 *   gcc -O1 -Wall -Wextra -DKMSG_TEST -o /tmp/kmsgt kmsg_test.c ../drg-kmsg.c && /tmp/kmsgt
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

long kmsg_new_from(const char *prev, size_t plen, const char *cur, size_t clen);

#define SIZE 4096
static char stream[1 << 20];
static size_t slen;
static char out[2 << 20];
static size_t olen;

static void emit(const char *line)
{
	size_t n = strlen(line);
	memcpy(stream + slen, line, n);
	slen += n;
}

/* what proc_kmsgbuf returns: the last SIZE bytes written */
static size_t snap(char *buf)
{
	size_t n = slen < SIZE ? slen : SIZE;
	memcpy(buf, stream + slen - n, n);
	return n;
}

static int run(int max_step, int expect_loss)
{
	static char a[SIZE], b[SIZE];
	size_t plen = 0, done = 0;
	int lost = 0;
	olen = 0;
	slen = 0;
	srand(7);
	int k = 0;
	while (slen < sizeof(stream) - 4096) {
		int burst = rand() % max_step;
		for (int i = 0; i < burst; i++) {
			char line[96];
			/* every third line repeats exactly, as "exec xpcproxy" does */
			if (k % 3 == 0)
				snprintf(line, sizeof(line), "exec xpcproxy\n");
			else
				snprintf(line, sizeof(line), "line %d: kernel text\n", k);
			emit(line);
			k++;
		}
		size_t clen = snap(b);
		long from = kmsg_new_from(a, plen, b, clen);
		if (from < 0) {
			lost++;
			from = 0;
		}
		memcpy(out + olen, b + from, clen - (size_t)from);
		olen += clen - (size_t)from;
		memcpy(a, b, clen);
		plen = clen;
		done++;
	}
	int exact = olen == slen && !memcmp(out, stream, slen);
	printf("max %d lines per read, %zu reads: %zu bytes out of %zu, losses %d -> %s\n", max_step, done, olen, slen,
	       lost, expect_loss ? (lost ? "loss marked: ok" : "LOSS NOT SEEN") : (exact ? "exact" : "MISMATCH"));
	return expect_loss ? lost == 0 : !exact;
}

int main(void)
{
	int fails = 0;
	fails += run(60, 0);	/* < 4096 bytes between reads: nothing may be lost */
	fails += run(150, 0);
	fails += run(400, 1);	/* more than the buffer between reads: must be marked */
	printf(fails ? "FAIL\n" : "PASS\n");
	return fails;
}
