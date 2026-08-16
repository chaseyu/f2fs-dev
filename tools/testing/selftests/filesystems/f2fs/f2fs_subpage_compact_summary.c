// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define BLOCK_SIZE 4096U
#define BLOCK_COUNT 480U

static void fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void expect(int condition, const char *what)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", what);
		exit(EXIT_FAILURE);
	}
}

static void fill_block(uint8_t *buffer, unsigned int block)
{
	unsigned int i;

	for (i = 0; i < BLOCK_SIZE; i++)
		buffer[i] = (uint8_t)(block * 37U + i * 131U + (i >> 4));
}

static void write_block(int fd, const uint8_t *buffer, unsigned int block)
{
	ssize_t written;

	written = pwrite(fd, buffer, BLOCK_SIZE, (off_t)block * BLOCK_SIZE);
	if (written != BLOCK_SIZE) {
		if (written >= 0)
			errno = EIO;
		fail("write compact-summary block");
	}
}

static void read_block(int fd, uint8_t *buffer, unsigned int block)
{
	ssize_t got;

	got = pread(fd, buffer, BLOCK_SIZE, (off_t)block * BLOCK_SIZE);
	if (got != BLOCK_SIZE) {
		if (got >= 0)
			errno = EIO;
		fail("read compact-summary block");
	}
}

static void populate_summary_file(const char *path)
{
	uint8_t buffer[BLOCK_SIZE];
	unsigned int block;
	int fd;

	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0)
		fail("create compact-summary file");
	for (block = 0; block < BLOCK_COUNT; block++) {
		fill_block(buffer, block);
		write_block(fd, buffer, block);
	}
	if (fsync(fd))
		fail("fsync compact-summary seed");
	if (syncfs(fd))
		fail("sync compact-summary seed filesystem");
	if (close(fd))
		fail("close compact-summary seed");
}

static void check_summary_file(const char *path)
{
	uint8_t actual[BLOCK_SIZE];
	uint8_t expected[BLOCK_SIZE];
	struct stat st;
	unsigned int block;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		fail("open compact-summary file after remount");
	if (fstat(fd, &st))
		fail("stat compact-summary file");
	expect(st.st_size == (off_t)BLOCK_COUNT * BLOCK_SIZE,
	       "compact-summary file size changed across remount");
	for (block = 0; block < BLOCK_COUNT; block++) {
		fill_block(expected, block);
		read_block(fd, actual, block);
		expect(!memcmp(actual, expected, BLOCK_SIZE),
		       "compact-summary data changed across remount");
	}

	/* Persist the restored current-segment summaries in a new checkpoint. */
	fill_block(expected, BLOCK_COUNT);
	write_block(fd, expected, BLOCK_COUNT);
	if (fsync(fd))
		fail("fsync compact-summary append");
	if (syncfs(fd))
		fail("sync compact-summary verify filesystem");
	if (close(fd))
		fail("close compact-summary verify");
}

int main(int argc, char **argv)
{
	struct statfs st;
	char path[4096];
	long page_size;

	if (argc != 3) {
		fprintf(stderr, "usage: %s MOUNTPOINT seed|verify\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (strcmp(argv[2], "seed") && strcmp(argv[2], "verify")) {
		fprintf(stderr, "usage: %s MOUNTPOINT seed|verify\n", argv[0]);
		return EXIT_FAILURE;
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0)
		fail("read system page size");
	if (statfs(argv[1], &st))
		fail("stat compact-summary mountpoint");
	expect((unsigned long)st.f_type == F2FS_SUPER_MAGIC,
	       "mountpoint is not F2FS");
	expect((uint64_t)st.f_bsize < (uint64_t)page_size,
	       "filesystem block size is not subpage");
	expect(st.f_bsize == BLOCK_SIZE,
	       "compact-summary regression requires 4K filesystem blocks");
	if (snprintf(path, sizeof(path), "%s/subpage-compact-summary", argv[1]) >=
			(int)sizeof(path)) {
		errno = ENAMETOOLONG;
		fail("format compact-summary path");
	}

	if (!strcmp(argv[2], "seed"))
		populate_summary_file(path);
	else
		check_summary_file(path);
	printf("PASS: compact summaries %s across 4K blocks\n", argv[2]);
	return EXIT_SUCCESS;
}
