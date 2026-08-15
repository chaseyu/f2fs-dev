// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/falloc.h>
#include <linux/f2fs.h>
#include <linux/fs.h>
#include <linux/fscrypt.h>
#include <linux/fsverity.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#define KSFT_SKIP 4
#define DATA_LEN 65537
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

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

static void make_path(char *path, size_t size, const char *dir, const char *name)
{
	int len = snprintf(path, size, "%s/%s", dir, name);

	if (len < 0 || (size_t)len >= size) {
		errno = ENAMETOOLONG;
		fail("make_path");
	}
}

static void fill_pattern(uint8_t *buf, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(i * 131U + (i >> 7) + seed * 17U);
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

static void check_seek(int fd, off_t offset, int whence, off_t expected, const char *what)
{
	off_t result = lseek(fd, offset, whence);

	if (result != expected) {
		fprintf(stderr, "FAIL: %s returned %lld, expected %lld\n", what,
			(long long)result, (long long)expected);
		exit(EXIT_FAILURE);
	}
}

static void test_buffered_io(const char *dir)
{
	static const size_t chunks[] = { 1, 4094, 2, 4096, 4097, 8191, 3 };
	uint8_t *expected = malloc(DATA_LEN);
	uint8_t *actual = malloc(DATA_LEN);
	uint8_t patch[9000];
	char path[PATH_MAX];
	size_t pos = 0;
	size_t chunk = 0;
	void *map;
	char xattr[32] = { 0 };
	int fd;

	expect(expected && actual, "allocate data buffers");
	fill_pattern(expected, DATA_LEN, 1);
	fill_pattern(patch, sizeof(patch), 9);
	make_path(path, sizeof(path), dir, "subpage-buffered.bin");
	unlink(path);

	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open buffered file");
	while (pos < DATA_LEN) {
		size_t len = chunks[chunk++ % ARRAY_SIZE(chunks)];

		if (len > DATA_LEN - pos)
			len = DATA_LEN - pos;
		pwrite_all(fd, expected + pos, len, pos);
		pos += len;
	}
	if (fsync(fd))
		fail("fsync buffered file");

	pwrite_all(fd, patch, sizeof(patch), 12287);
	memcpy(expected + 12287, patch, sizeof(patch));
	if (fdatasync(fd))
		fail("fdatasync buffered file");
	pread_all(fd, actual, DATA_LEN, 0);
	expect(!memcmp(actual, expected, DATA_LEN), "buffered data mismatch");

	map = mmap(NULL, DATA_LEN, PROT_READ, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED)
		fail("private read mmap");
	expect(!memcmp(map, expected, DATA_LEN), "private mmap mismatch");
	if (munmap(map, DATA_LEN))
		fail("munmap");

	map = mmap(NULL, DATA_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("shared writable mmap");
	((uint8_t *)map)[0] ^= 0x5a;
	((uint8_t *)map)[4095] ^= 0xa5;
	((uint8_t *)map)[4096] ^= 0x3c;
	((uint8_t *)map)[8191] ^= 0xc3;
	((uint8_t *)map)[8192] ^= 0x69;
	((uint8_t *)map)[12287] ^= 0x96;
	((uint8_t *)map)[12288] ^= 0x0f;
	((uint8_t *)map)[16383] ^= 0xf0;
	((uint8_t *)map)[16384] ^= 0x55;
	((uint8_t *)map)[DATA_LEN - 1] ^= 0xaa;
	expected[0] ^= 0x5a;
	expected[4095] ^= 0xa5;
	expected[4096] ^= 0x3c;
	expected[8191] ^= 0xc3;
	expected[8192] ^= 0x69;
	expected[12287] ^= 0x96;
	expected[12288] ^= 0x0f;
	expected[16383] ^= 0xf0;
	expected[16384] ^= 0x55;
	expected[DATA_LEN - 1] ^= 0xaa;
	if (msync(map, DATA_LEN, MS_SYNC))
		fail("msync shared writable mmap");
	if (munmap(map, DATA_LEN))
		fail("munmap shared writable mmap");
	if (fsync(fd))
		fail("fsync shared writable mmap");
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))
		fail("posix_fadvise shared writable mmap");
	pread_all(fd, actual, DATA_LEN, 0);
	expect(!memcmp(actual, expected, DATA_LEN),
	       "shared writable mmap data mismatch");

	if (fsetxattr(fd, "user.f2fs_subpage", "value", 5, 0))
		fail("fsetxattr");
	expect(fgetxattr(fd, "user.f2fs_subpage", xattr, sizeof(xattr)) == 5,
	       "fgetxattr length");
	expect(!memcmp(xattr, "value", 5), "fgetxattr value");

	if (ftruncate(fd, 16383))
		fail("truncate shrink");
	if (fsync(fd))
		fail("fsync after shrink");
	if (ftruncate(fd, DATA_LEN))
		fail("truncate extend");
	if (fsync(fd))
		fail("fsync after extend");
	memset(actual, 0xa5, DATA_LEN);
	pread_all(fd, actual, DATA_LEN, 0);
	expect(!memcmp(actual, expected, 16383), "truncate prefix mismatch");
	for (pos = 16383; pos < DATA_LEN; pos++)
		expect(actual[pos] == 0, "truncate exposed nonzero tail data");

	if (close(fd))
		fail("close buffered file");
	free(actual);
	free(expected);
}

static void test_fallocate(const char *dir)
{
	const size_t initial_len = 16 * 4096 + 1;
	const size_t final_len = initial_len + 2 * 4096;
	uint8_t *expected = calloc(1, final_len);
	uint8_t *actual = malloc(final_len);
	uint8_t *map;
	char path[PATH_MAX];
	struct stat st;
	int fd;

	expect(expected && actual, "allocate fallocate buffers");
	fill_pattern(expected, initial_len, 31);
	make_path(path, sizeof(path), dir, "subpage-fallocate.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open fallocate file");
	pwrite_all(fd, expected, initial_len, 0);

	if (fallocate(fd, FALLOC_FL_KEEP_SIZE, initial_len + 4096, 4096))
		fail("fallocate keep size");
	if (fstat(fd, &st))
		fail("fstat fallocate keep size");
	expect((size_t)st.st_size == initial_len,
	       "fallocate keep size changed i_size");
	if (fallocate(fd, 0, initial_len + 4096, 4096))
		fail("fallocate extend");
	if (fstat(fd, &st))
		fail("fstat fallocate extend");
	expect((size_t)st.st_size == final_len,
	       "fallocate did not extend to exact size");

	map = mmap(NULL, final_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("mmap fallocate file");
	if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		      4095, 2 * 4096 + 3))
		fail("fallocate punch hole");
	memset(expected + 4095, 0, 2 * 4096 + 3);
	map[4096] = 0xb6;
	expected[4096] = 0xb6;
	if (fallocate(fd, FALLOC_FL_ZERO_RANGE, 16383, 2 * 4096 + 5))
		fail("fallocate zero range");
	memset(expected + 16383, 0, 2 * 4096 + 5);
	map[16384] = 0xc7;
	expected[16384] = 0xc7;

	errno = 0;
	expect(fallocate(fd, FALLOC_FL_COLLAPSE_RANGE, 0, 16384) == -1 &&
	       errno == EOPNOTSUPP, "collapse range was not rejected");
	errno = 0;
	expect(fallocate(fd, FALLOC_FL_INSERT_RANGE, 0, 16384) == -1 &&
	       errno == EOPNOTSUPP, "insert range was not rejected");
	if (msync(map, final_len, MS_SYNC))
		fail("msync fallocate file");
	if (munmap(map, final_len))
		fail("munmap fallocate file");
	if (fsync(fd))
		fail("fsync fallocate file");
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))
		fail("posix_fadvise fallocate file");
	pread_all(fd, actual, final_len, 0);
	expect(!memcmp(actual, expected, final_len),
	       "fallocate data mismatch");

	if (close(fd))
		fail("close fallocate file");
	free(actual);
	free(expected);
}

static void test_seek_dirty_prealloc(const char *dir)
{
	const off_t len = 4 * 4096;
	char path[PATH_MAX];
	char value = 'x';
	int fd;

	make_path(path, sizeof(path), dir, "subpage-seek-dirty.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open dirty seek file");
	if (ftruncate(fd, len))
		fail("size dirty seek file");
	if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, len))
		fail("preallocate dirty seek file");
	pwrite_all(fd, &value, 1, 2 * 4096);

	check_seek(fd, 0, SEEK_HOLE, 0, "SEEK_HOLE clean prealloc block");
	check_seek(fd, 0, SEEK_DATA, 2 * 4096,
		   "SEEK_DATA dirty prealloc block");
	check_seek(fd, 2 * 4096, SEEK_HOLE, 3 * 4096,
		   "SEEK_HOLE after dirty prealloc block");
	errno = 0;
	expect(lseek(fd, 3 * 4096, SEEK_DATA) == -1 && errno == ENXIO,
	       "SEEK_DATA after dirty prealloc block");

	if (fdatasync(fd))
		fail("fdatasync dirty seek file");
	check_seek(fd, 0, SEEK_DATA, 2 * 4096,
		   "SEEK_DATA written prealloc block");
	check_seek(fd, 2 * 4096, SEEK_HOLE, 3 * 4096,
		   "SEEK_HOLE after written prealloc block");
	if (close(fd))
		fail("close dirty seek file");
}

static void test_mmap_fallocate_extension(const char *dir)
{
	const size_t len = 4 * 4096 + 1;
	uint8_t *actual = malloc(len);
	uint8_t *map;
	char path[PATH_MAX];
	char stop = 1;
	int sync_pipe[2];
	int status;
	pid_t child;
	size_t i;
	int fd;

	expect(actual, "allocate mmap fallocate readback");
	make_path(path, sizeof(path), dir, "subpage-mmap-extend.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open mmap fallocate file");
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("mmap fallocate extension file");
	if (pipe2(sync_pipe, O_CLOEXEC | O_NONBLOCK))
		fail("pipe mmap fallocate sync");

	child = fork();
	if (child < 0)
		fail("fork mmap fallocate sync");
	if (!child) {
		char command;

		close(sync_pipe[1]);
		for (;;) {
			ssize_t got = read(sync_pipe[0], &command, 1);

			if (got >= 0)
				break;
			if (errno != EAGAIN || fsync(fd))
				_exit(EXIT_FAILURE);
		}
		_exit(EXIT_SUCCESS);
	}
	close(sync_pipe[0]);

	for (i = 0; i < len; i++) {
		int err = posix_fallocate(fd, i, 1);

		if (err) {
			errno = err;
			fail("posix_fallocate mmap extension");
		}
		map[i] = 0x78;
		expect(map[i] == 0x78, "mmap extension write was lost");
	}
	if (write(sync_pipe[1], &stop, 1) != 1)
		fail("stop mmap fallocate sync");
	if (close(sync_pipe[1]))
		fail("close mmap fallocate sync pipe");
	if (waitpid(child, &status, 0) != child)
		fail("wait mmap fallocate sync");
	expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
	       "mmap fallocate sync failed");

	for (i = 0; i < len; i++)
		expect(map[i] == 0x78, "mmap extension data was modified");
	if (msync(map, len, MS_SYNC))
		fail("msync mmap fallocate extension");
	if (munmap(map, len))
		fail("munmap mmap fallocate extension");
	if (fsync(fd))
		fail("fsync mmap fallocate extension");
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))
		fail("posix_fadvise mmap fallocate extension");
	pread_all(fd, actual, len, 0);
	for (i = 0; i < len; i++)
		expect(actual[i] == 0x78,
		       "persisted mmap extension data was modified");
	if (close(fd))
		fail("close mmap fallocate extension");
	free(actual);
}

static void test_mapped_punch_rewrite(const char *dir)
{
	const size_t len = 3 * 4096;
	uint8_t *expected = malloc(len);
	uint8_t *actual = malloc(len);
	uint8_t *map;
	char path[PATH_MAX];
	int fd;

	expect(expected && actual, "allocate mapped punch buffers");
	memset(expected, 0x58, len);
	make_path(path, sizeof(path), dir, "subpage-mapped-punch.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open mapped punch file");
	pwrite_all(fd, expected, len, 0);
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("mmap mapped punch file");

	memset(map + 2048, 0x5a, 2 * 4096);
	if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		      2048, 2 * 4096))
		fail("punch mapped file");
	memset(map + 2048, 0x59, 2 * 4096);
	memset(expected + 2048, 0x59, 2 * 4096);
	if (msync(map, len, MS_SYNC))
		fail("msync mapped punch file");
	if (munmap(map, len))
		fail("munmap mapped punch file");
	if (fsync(fd))
		fail("fsync mapped punch file");
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))
		fail("posix_fadvise mapped punch file");
	pread_all(fd, actual, len, 0);
	expect(!memcmp(actual, expected, len),
	       "mapped rewrite after punch mismatch");
	if (close(fd))
		fail("close mapped punch file");
	free(actual);
	free(expected);
}

static void test_mmap_sparse_and_truncate(const char *dir)
{
	const size_t len = 16 * 4096 + 1;
	uint8_t *expected = calloc(1, len);
	uint8_t *actual = malloc(len);
	uint8_t *map;
	char path[PATH_MAX];
	size_t i;
	int fd;

	expect(expected && actual, "allocate mmap buffers");
	make_path(path, sizeof(path), dir, "subpage-mmap-sparse.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open sparse mmap file");
	if (ftruncate(fd, len))
		fail("size sparse mmap file");

	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("mmap sparse file");
	for (i = 0; i < len; i += 4096) {
		map[i] = (uint8_t)(i / 4096 + 1);
		expected[i] = map[i];
	}
	map[len - 1] = 0xe7;
	expected[len - 1] = 0xe7;
	if (msync(map, len, MS_SYNC))
		fail("msync sparse mmap file");

	if (ftruncate(fd, 4097))
		fail("shrink mapped sparse file");
	if (ftruncate(fd, len))
		fail("extend mapped sparse file");
	memset(expected + 4097, 0, len - 4097);
	map[4096] = 0x71;
	map[8192] = 0x82;
	map[16384] = 0x93;
	map[len - 1] = 0xa4;
	expected[4096] = 0x71;
	expected[8192] = 0x82;
	expected[16384] = 0x93;
	expected[len - 1] = 0xa4;
	if (msync(map, len, MS_SYNC))
		fail("msync truncated mmap file");
	if (munmap(map, len))
		fail("munmap sparse file");
	if (fsync(fd))
		fail("fsync sparse mmap file");
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))
		fail("posix_fadvise sparse mmap file");
	pread_all(fd, actual, len, 0);
	expect(!memcmp(actual, expected, len),
	       "sparse mmap or truncate data mismatch");

	if (close(fd))
		fail("close sparse mmap file");
	free(actual);
	free(expected);
}

static void test_mmap_concurrent_writeback(const char *dir)
{
	const size_t len = 256 * 4096;
	const unsigned int rounds = 1000;
	uint8_t *expected = calloc(1, len);
	uint8_t *actual = malloc(len);
	uint8_t *map;
	char path[PATH_MAX];
	unsigned int round;
	size_t block;
	int status;
	pid_t child;
	int fd;

	expect(expected && actual, "allocate concurrent mmap buffers");
	make_path(path, sizeof(path), dir, "subpage-mmap-concurrent.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open concurrent mmap file");
	if (ftruncate(fd, len))
		fail("size concurrent mmap file");
	map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED)
		fail("mmap concurrent file");

	child = fork();
	if (child < 0)
		fail("fork concurrent mmap writer");
	if (!child) {
		for (round = 0; round < rounds; round++) {
			for (block = 0; block < len / 4096; block++)
				map[block * 4096 + 17] =
					(uint8_t)(round * 13U + block);
			if (!(round % 31) && msync(map, len, MS_ASYNC))
				_exit(EXIT_FAILURE);
		}
		if (msync(map, len, MS_SYNC))
			_exit(EXIT_FAILURE);
		_exit(EXIT_SUCCESS);
	}

	for (round = 0; round < rounds; round++) {
		for (block = 0; block < len / 4096; block++)
			map[block * 4096 + 31] =
				(uint8_t)(round * 29U + block);
		if (!(round % 23) && fdatasync(fd))
			fail("concurrent mmap fdatasync");
	}
	if (waitpid(child, &status, 0) != child)
		fail("wait concurrent mmap writer");
	expect(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
	       "concurrent mmap writer failed");
	for (block = 0; block < len / 4096; block++) {
		expected[block * 4096 + 17] =
			(uint8_t)((rounds - 1) * 13U + block);
		expected[block * 4096 + 31] =
			(uint8_t)((rounds - 1) * 29U + block);
	}
	if (msync(map, len, MS_SYNC))
		fail("final concurrent mmap msync");
	if (munmap(map, len))
		fail("munmap concurrent file");
	if (fsync(fd))
		fail("fsync concurrent mmap file");
	if (posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED))
		fail("posix_fadvise concurrent mmap file");
	pread_all(fd, actual, len, 0);
	expect(!memcmp(actual, expected, len),
	       "concurrent mmap writeback data mismatch");

	if (close(fd))
		fail("close concurrent mmap file");
	free(actual);
	free(expected);
}

static void test_sync_and_sparse(const char *dir)
{
	char sync_path[PATH_MAX];
	char sparse_path[PATH_MAX];
	char value = 'x';
	char readback[7];
	int fd;

	make_path(sync_path, sizeof(sync_path), dir, "subpage-osync.bin");
	unlink(sync_path);
	fd = open(sync_path, O_CREAT | O_EXCL | O_RDWR | O_SYNC, 0600);
	if (fd < 0)
		fail("open O_SYNC file");
	pwrite_all(fd, "osync!", 6, 4093);
	if (fdatasync(fd))
		fail("fdatasync O_SYNC file");
	memset(readback, 0, sizeof(readback));
	pread_all(fd, readback, 6, 4093);
	expect(!memcmp(readback, "osync!", 6), "O_SYNC data mismatch");
	if (close(fd))
		fail("close O_SYNC file");

	make_path(sparse_path, sizeof(sparse_path), dir, "subpage-sparse.bin");
	unlink(sparse_path);
	fd = open(sparse_path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open sparse file");
	if (ftruncate(fd, 32768))
		fail("size sparse file");
	pwrite_all(fd, &value, 1, 4096);
	pwrite_all(fd, &value, 1, 12288);
	pwrite_all(fd, &value, 1, 20480);
	if (fsync(fd))
		fail("fsync sparse file");

	check_seek(fd, 0, SEEK_DATA, 4096, "SEEK_DATA block 1");
	check_seek(fd, 4096, SEEK_HOLE, 8192, "SEEK_HOLE block 1");
	check_seek(fd, 8192, SEEK_DATA, 12288, "SEEK_DATA block 3");
	check_seek(fd, 12288, SEEK_HOLE, 16384, "SEEK_HOLE block 3");
	check_seek(fd, 16384, SEEK_DATA, 20480, "SEEK_DATA block 5");
	check_seek(fd, 20480, SEEK_HOLE, 24576, "SEEK_HOLE block 5");
	errno = 0;
	expect(lseek(fd, 24576, SEEK_DATA) == -1 && errno == ENXIO,
	       "SEEK_DATA after final extent");
	if (close(fd))
		fail("close sparse file");
}

static void test_gc_ioctl(int fd)
{
	__u32 sync = 1;

	if (geteuid())
		return;
	if (ioctl(fd, F2FS_IOC_GARBAGE_COLLECT, &sync) &&
	    errno != ENODATA && errno != EAGAIN)
		fail("F2FS_IOC_GARBAGE_COLLECT");
}

static void test_orphan_checkpoint(const char *dir, int dirfd)
{
	uint8_t block[4096];
	char path[PATH_MAX];
	unsigned int i;
	int fd;

	fill_pattern(block, sizeof(block), 23);
	make_path(path, sizeof(path), dir, "subpage-orphan.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open orphan file");
	for (i = 0; i < 256; i++)
		pwrite_all(fd, block, sizeof(block), (off_t)i * sizeof(block));
	if (fsync(fd))
		fail("fsync orphan file");
	if (unlink(path))
		fail("unlink orphan file");
	if (fsync(dirfd))
		fail("fsync orphan directory");
	if (syncfs(dirfd))
		fail("checkpoint open orphan");
	if (close(fd))
		fail("close orphan file");
}

static void test_rejected_mutating_features(const char *dir, int dirfd)
{
	struct fsverity_enable_arg verity = {
		.version = 1,
		.hash_algorithm = FS_VERITY_HASH_ALG_SHA256,
		.block_size = 4096,
	};
	struct fscrypt_policy_v2 policy = {
		.version = FSCRYPT_POLICY_V2,
		.contents_encryption_mode = FSCRYPT_MODE_AES_256_XTS,
		.filenames_encryption_mode = FSCRYPT_MODE_AES_256_CTS,
	};
	struct f2fs_defragment defrag = {
		.start = 0,
		.len = 4096,
	};
	struct f2fs_move_range move = {
		.pos_in = 0,
		.pos_out = 0,
		.len = 4096,
	};
	uint64_t block_count = 0;
	uint32_t pin = 1;
	char path[PATH_MAX];
	char crypt_path[PATH_MAX];
	int flags;
	int fd;

	make_path(path, sizeof(path), dir, "subpage-unsupported.bin");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open unsupported feature file");
	move.dst_fd = fd;

	errno = 0;
	expect(ioctl(fd, F2FS_IOC_DEFRAGMENT, &defrag) == -1 &&
	       errno == EOPNOTSUPP, "defragment was not rejected");
	errno = 0;
	expect(ioctl(fd, F2FS_IOC_MOVE_RANGE, &move) == -1 &&
	       errno == EOPNOTSUPP, "move range was not rejected");
	errno = 0;
	expect(ioctl(fd, F2FS_IOC_SET_PIN_FILE, &pin) == -1 &&
	       errno == EOPNOTSUPP, "pin file was not rejected");
	errno = 0;
	expect(ioctl(dirfd, F2FS_IOC_RESIZE_FS, &block_count) == -1 &&
	       errno == EOPNOTSUPP, "resize was not rejected");
	errno = 0;
	expect(ioctl(fd, F2FS_IOC_START_ATOMIC_WRITE) == -1 &&
	       errno == EOPNOTSUPP, "atomic write was not rejected");
	if (ioctl(fd, FS_IOC_GETFLAGS, &flags))
		fail("FS_IOC_GETFLAGS");
	flags |= FS_COMPR_FL;
	errno = 0;
	expect(ioctl(fd, FS_IOC_SETFLAGS, &flags) == -1 &&
	       errno == EOPNOTSUPP, "compression was not rejected");

	if (close(fd))
		fail("close unsupported feature file");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		fail("open unsupported feature file readonly");
	errno = 0;
	expect(ioctl(fd, FS_IOC_ENABLE_VERITY, &verity) == -1 &&
	       errno == EOPNOTSUPP, "verity enable was not rejected");
	if (close(fd))
		fail("close verity feature file");

	make_path(crypt_path, sizeof(crypt_path), dir, "subpage-crypt-dir");
	rmdir(crypt_path);
	if (mkdir(crypt_path, 0700))
		fail("mkdir encryption feature directory");
	fd = open(crypt_path, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		fail("open encryption feature directory");
	errno = 0;
	expect(ioctl(fd, FS_IOC_SET_ENCRYPTION_POLICY, &policy) == -1 &&
	       errno == EOPNOTSUPP, "encryption policy was not rejected");
	if (close(fd))
		fail("close encryption feature directory");
	if (rmdir(crypt_path))
		fail("rmdir encryption feature directory");
}

int main(int argc, char **argv)
{
	struct statfs st;
	long page_size;
	int dirfd;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s MOUNTPOINT\n", argv[0]);
		return KSFT_SKIP;
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (statfs(argv[1], &st))
		fail("statfs");
	if (page_size <= 0 ||
	    (uint64_t)page_size <= (uint64_t)st.f_bsize) {
		printf("SKIP: page size %ld is not larger than block size %ld\n",
		       page_size, (long)st.f_bsize);
		return KSFT_SKIP;
	}
	expect(st.f_bsize == 4096, "filesystem block size is not 4096");

	test_buffered_io(argv[1]);
	test_fallocate(argv[1]);
	test_seek_dirty_prealloc(argv[1]);
	test_mmap_fallocate_extension(argv[1]);
	test_mapped_punch_rewrite(argv[1]);
	test_mmap_sparse_and_truncate(argv[1]);
	test_mmap_concurrent_writeback(argv[1]);
	test_sync_and_sparse(argv[1]);
	dirfd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (dirfd < 0)
		fail("open mount directory");
	test_orphan_checkpoint(argv[1], dirfd);
	test_rejected_mutating_features(argv[1], dirfd);
	test_gc_ioctl(dirfd);
	if (fsync(dirfd))
		fail("fsync mount directory");
	if (close(dirfd))
		fail("close mount directory");

	printf("PASS: F2FS 4K blocks on %ld-byte pages\n", page_size);
	return EXIT_SUCCESS;
}
