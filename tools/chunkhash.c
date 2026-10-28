/*
 * chunkhash: one 64-bit hash per 1 MiB chunk of a file or block device.
 *
 *   chunkhash PATH COUNT     prints COUNT lines "index hash" (hex)
 *
 * Built twice from this file: static aarch64 for the phone (runs in TWRP
 * against /dev/block/...), native for the PC (against the last image flashed).
 * Comparing both lists finds the chunks iOS changed, so only those are copied
 * over USB (tools/drg-pull-changed.sh). The hash is FNV-1a over 64-bit words
 * plus the chunk index: a change detector, not a security hash.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHUNK (1u << 20)

int
main(int argc, char **argv)
{
	static uint64_t buf[CHUNK / 8];

	if (argc != 3) {
		fprintf(stderr, "usage: chunkhash PATH COUNT\n");
		return 2;
	}
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	unsigned long count = strtoul(argv[2], NULL, 0);
	for (unsigned long i = 0; i < count; i++) {
		size_t got = 0;
		while (got < CHUNK) {
			ssize_t n = read(fd, (char *)buf + got, CHUNK - got);
			if (n <= 0) {
				fprintf(stderr, "short read at chunk %lu\n", i);
				return 1;
			}
			got += (size_t)n;
		}
		uint64_t h = 0xcbf29ce484222325ull ^ i;
		for (size_t w = 0; w < CHUNK / 8; w++) {
			h ^= buf[w];
			h *= 0x100000001b3ull;
		}
		printf("%lu %016llx\n", i, (unsigned long long)h);
	}
	return 0;
}
