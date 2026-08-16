// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define ENTRY_COUNT 64

struct sync_context {
	atomic_bool stop;
	atomic_int error;
	int dirfd;
};

static void fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void make_name(char *name, size_t size, unsigned int round,
		      unsigned int entry, char generation)
{
	int ret;

	ret = snprintf(name, size,
		       "entry-%c-%06u-%03u-abcdefghijklmnopqrstuvwxyz0123456789",
		       generation, round, entry);
	if (ret < 0 || (size_t)ret >= size) {
		errno = ENAMETOOLONG;
		fail("format directory entry name");
	}
}

static void create_entry(int dirfd, const char *name)
{
	static const char byte = 'x';
	int fd;

	fd = openat(dirfd, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
		    0600);
	if (fd < 0)
		fail("create directory entry");
	if (write(fd, &byte, sizeof(byte)) != sizeof(byte))
		fail("write directory entry");
	if (close(fd))
		fail("close directory entry");
}

static void sync_directory(int dirfd)
{
	if (fsync(dirfd))
		fail("fsync directory");
	if (syncfs(dirfd))
		fail("syncfs directory filesystem");
}

static void *sync_worker(void *arg)
{
	struct sync_context *ctx = arg;

	while (!atomic_load_explicit(&ctx->stop, memory_order_relaxed)) {
		if (fsync(ctx->dirfd) || syncfs(ctx->dirfd)) {
			atomic_store(&ctx->error, errno);
			break;
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct statfs st;
	char path[4096];
	char old_name[128];
	char new_name[128];
	unsigned int rounds = 200;
	unsigned int round;
	unsigned int i;
	long page_size;
	struct sync_context sync_ctx;
	pthread_t sync_thread;
	int dirfd;
	int ret;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s MOUNTPOINT [ROUNDS]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 3) {
		char *end;
		unsigned long value = strtoul(argv[2], &end, 0);

		if (!value || *end || value > 100000) {
			errno = EINVAL;
			fail("parse rounds");
		}
		rounds = value;
	}

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size < 0)
		fail("sysconf page size");
	if (statfs(argv[1], &st))
		fail("statfs mountpoint");
	if ((unsigned long)st.f_type != F2FS_SUPER_MAGIC) {
		errno = EINVAL;
		fail("mountpoint is not F2FS");
	}
	if ((uint64_t)page_size <= (uint64_t)st.f_bsize) {
		errno = EOPNOTSUPP;
		fail("filesystem is not subpage");
	}

	if (snprintf(path, sizeof(path), "%s/subpage-dirsync", argv[1]) >=
			(int)sizeof(path)) {
		errno = ENAMETOOLONG;
		fail("format test directory path");
	}
	if (mkdir(path, 0700) && errno != EEXIST)
		fail("create test directory");
	dirfd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dirfd < 0)
		fail("open test directory");
	sync_ctx.dirfd = dirfd;
	atomic_init(&sync_ctx.stop, false);
	atomic_init(&sync_ctx.error, 0);
	ret = pthread_create(&sync_thread, NULL, sync_worker, &sync_ctx);
	if (ret) {
		errno = ret;
		fail("create sync thread");
	}

	for (round = 0; round < rounds; round++) {
		for (i = 0; i < ENTRY_COUNT; i++) {
			make_name(old_name, sizeof(old_name), round, i, 'a');
			create_entry(dirfd, old_name);
		}
		sync_directory(dirfd);

		for (i = 0; i < ENTRY_COUNT; i += 2) {
			make_name(old_name, sizeof(old_name), round, i, 'a');
			make_name(new_name, sizeof(new_name), round, i, 'b');
			if (renameat(dirfd, old_name, dirfd, new_name))
				fail("replace directory entry");
		}
		sync_directory(dirfd);

		for (i = 0; i < ENTRY_COUNT; i++) {
			make_name(old_name, sizeof(old_name), round, i,
				  i & 1 ? 'a' : 'b');
			if (unlinkat(dirfd, old_name, 0))
				fail("remove directory entry");
		}
		sync_directory(dirfd);

		if (!(round % 25))
			printf("round %u/%u\n", round + 1, rounds);
	}
	atomic_store(&sync_ctx.stop, true);
	ret = pthread_join(sync_thread, NULL);
	if (ret) {
		errno = ret;
		fail("join sync thread");
	}
	ret = atomic_load(&sync_ctx.error);
	if (ret) {
		errno = ret;
		fail("concurrent directory sync");
	}

	if (close(dirfd))
		fail("close test directory");
	if (rmdir(path))
		fail("remove test directory");
	printf("PASS: %u subpage directory truncate/reuse sync rounds\n",
	       rounds);
	return EXIT_SUCCESS;
}
