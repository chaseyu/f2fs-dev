// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define FILE_COUNT 8
#define MAX_SIZE (2U * 1024U * 1024U)
#define DEFAULT_ITERATIONS 10000U
#define KSFT_SKIP 4

struct model_file {
	uint8_t *data;
	size_t size;
	char path[PATH_MAX];
	int fd;
};

static uint64_t rng_state = UINT64_C(0x8a5cd789635d2dff);
static unsigned int current_iteration;
static unsigned int current_operation;

static void fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void expect(bool condition, const char *what)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", what);
		exit(EXIT_FAILURE);
	}
}

static uint32_t next_random(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 16);
}

static void pwrite_all(int fd, const void *buf, size_t len, off_t offset)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t written = pwrite(fd, p, len, offset);

		if (written < 0)
			fail("pwrite");
		expect(written != 0, "pwrite made no progress");
		p += written;
		len -= written;
		offset += written;
	}
}

static void pread_all(int fd, void *buf, size_t len, off_t offset)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t got = pread(fd, p, len, offset);

		if (got < 0)
			fail("pread");
		expect(got != 0, "unexpected EOF");
		p += got;
		len -= got;
		offset += got;
	}
}

static void verify_range(struct model_file *file, size_t offset, size_t len)
{
	uint8_t *actual;
	size_t i;

	if (!len)
		return;
	actual = malloc(len);
	expect(actual, "allocate verification buffer");
	pread_all(file->fd, actual, len, offset);
	if (memcmp(actual, file->data + offset, len)) {
		for (i = 0; i < len; i++)
			if (actual[i] != file->data[offset + i])
				break;
		fprintf(stderr, "FAIL: iteration %u operation %u mismatch in %s\n",
			current_iteration, current_operation, file->path);
		fprintf(stderr, "at %zu (range %zu+%zu): got 0x%02x expected 0x%02x\n",
			offset + i, offset, len, actual[i], file->data[offset + i]);
		exit(EXIT_FAILURE);
	}
	free(actual);
}

static void verify_file(struct model_file *file)
{
	struct stat st;

	if (fstat(file->fd, &st))
		fail("fstat");
	expect((size_t)st.st_size == file->size, "file size mismatch");
	verify_range(file, 0, file->size);
}

static void reopen_file(struct model_file *file)
{
	if (close(file->fd))
		fail("close before reopen");
	file->fd = open(file->path, O_RDWR);
	if (file->fd < 0)
		fail("reopen stress file");
}

static void write_random(struct model_file *file, unsigned int iteration, uint8_t *write_buf)
{
	size_t offset = next_random() % MAX_SIZE;
	size_t len = 1 + next_random() % 32768;
	size_t i;

	if (len > MAX_SIZE - offset)
		len = MAX_SIZE - offset;
	if (iteration < 32)
		fprintf(stderr, "iteration %u write %s offset=%zu len=%zu\n",
			iteration, file->path, offset, len);
	for (i = 0; i < len; i++)
		write_buf[i] = (uint8_t)(iteration * 29U + i * 131U +
				offset + (i >> 5));
	if (offset > file->size)
		memset(file->data + file->size, 0, offset - file->size);
	pwrite_all(file->fd, write_buf, len, offset);
	memcpy(file->data + offset, write_buf, len);
	if (file->size < offset + len)
		file->size = offset + len;
	verify_range(file, offset, len);
}

static void truncate_random(struct model_file *file)
{
	size_t new_size = next_random() % (MAX_SIZE + 1U);

	if (current_iteration < 32)
		fprintf(stderr, "iteration %u truncate %s old=%zu new=%zu\n",
			current_iteration, file->path, file->size, new_size);

	if (new_size > file->size)
		memset(file->data + file->size, 0, new_size - file->size);
	else
		memset(file->data + new_size, 0, MAX_SIZE - new_size);
	if (ftruncate(file->fd, new_size))
		fail("random ftruncate");
	file->size = new_size;
}

static void recreate_file(struct model_file *file, int dirfd)
{
	if (current_iteration < 32)
		fprintf(stderr, "iteration %u recreate %s\n",
			current_iteration, file->path);
	if (close(file->fd))
		fail("close before recreate");
	if (unlink(file->path))
		fail("unlink stress file");
	file->fd = open(file->path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (file->fd < 0)
		fail("recreate stress file");
	memset(file->data, 0, MAX_SIZE);
	file->size = 0;
	if (fsync(dirfd))
		fail("fsync stress directory");
}

int main(int argc, char **argv)
{
	struct model_file files[FILE_COUNT];
	struct statfs st;
	uint8_t *write_buf;
	unsigned int iterations = DEFAULT_ITERATIONS;
	unsigned int i;
	long page_size;
	int dirfd;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s MOUNTPOINT [ITERATIONS]\n", argv[0]);
		return KSFT_SKIP;
	}
	if (argc == 3) {
		char *end;
		unsigned long value = strtoul(argv[2], &end, 0);

		if (!value || *end || value > UINT_MAX) {
			fprintf(stderr, "Invalid iteration count: %s\n", argv[2]);
			return EXIT_FAILURE;
		}
		iterations = value;
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (statfs(argv[1], &st))
		fail("statfs");
	if (page_size <= 0 ||
	    (uint64_t)page_size < (uint64_t)st.f_bsize) {
		printf("SKIP: page size %ld is smaller than block size %ld\n",
		       page_size, (long)st.f_bsize);
		return KSFT_SKIP;
	}
	expect(st.f_bsize >= 4096 &&
	       ((uint64_t)st.f_bsize & ((uint64_t)st.f_bsize - 1)) == 0,
	       "filesystem block size is not a supported power of two");

	dirfd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (dirfd < 0)
		fail("open mount directory");
	write_buf = malloc(32768);
	expect(write_buf, "allocate write buffer");

	memset(files, 0, sizeof(files));
	for (i = 0; i < FILE_COUNT; i++) {
		int len = snprintf(files[i].path, sizeof(files[i].path),
				"%s/subpage-stress-%u.bin", argv[1], i);

		expect(len > 0 && (size_t)len < sizeof(files[i].path),
		       "stress path too long");
		unlink(files[i].path);
		files[i].fd = open(files[i].path, O_CREAT | O_EXCL | O_RDWR, 0600);
		if (files[i].fd < 0)
			fail("create stress file");
		files[i].data = calloc(1, MAX_SIZE);
		expect(files[i].data, "allocate file model");
	}
	if (fsync(dirfd))
		fail("initial directory fsync");

	for (i = 0; i < iterations; i++) {
		struct model_file *file = &files[next_random() % FILE_COUNT];
		unsigned int op = next_random() % 16;

		current_iteration = i;
		current_operation = op;

		if (op < 8) {
			write_random(file, i, write_buf);
		} else if (op < 10) {
			truncate_random(file);
		} else if (op == 10) {
			if (fsync(file->fd))
				fail("random fsync");
		} else if (op == 11) {
			if (fdatasync(file->fd))
				fail("random fdatasync");
		} else if (op == 12) {
			reopen_file(file);
		} else if (op == 13) {
			recreate_file(file, dirfd);
		} else if (file->size) {
			size_t offset = next_random() % file->size;
			size_t len = 1 + next_random() % 65536;

			if (len > file->size - offset)
				len = file->size - offset;
			verify_range(file, offset, len);
		}

		if (!(i % 64) && file->size) {
			size_t offset = next_random() % file->size;
			size_t len = 1 + next_random() % 32768;

			if (len > file->size - offset)
				len = file->size - offset;
			verify_range(file, offset, len);
		}
	}

	for (i = 0; i < FILE_COUNT; i++) {
		if (fsync(files[i].fd))
			fail("final file fsync");
		reopen_file(&files[i]);
		verify_file(&files[i]);
		if (close(files[i].fd))
			fail("close stress file");
		free(files[i].data);
	}
	if (fsync(dirfd))
		fail("final directory fsync");
	if (syncfs(dirfd))
		fail("syncfs");
	if (close(dirfd))
		fail("close mount directory");
	free(write_buf);

	printf("PASS: %u randomized F2FS operations on %ld-byte pages\n", iterations, page_size);
	return EXIT_SUCCESS;
}
