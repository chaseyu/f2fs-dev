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

	errno = 0;
	map = mmap(NULL, DATA_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	expect(map == MAP_FAILED && errno == EOPNOTSUPP,
	       "shared writable mmap was not rejected");

	errno = 0;
	expect(fallocate(fd, 0, 0, 4096) == -1 && errno == EOPNOTSUPP,
	       "fallocate was not rejected");

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
