// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/quota.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define KSFT_SKIP 4
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

static void check_quota(const char *device, int type, unsigned int id)
{
	struct dqblk dq = {};
	char what[80];

	if (!quotactl(QCMD(Q_GETQUOTA, type), device, id, (char *)&dq))
		goto check_usage;
	snprintf(what, sizeof(what), "Q_GETQUOTA type=%d id=%u", type, id);
	fail(what);

check_usage:
	if (!dq.dqb_curspace || !dq.dqb_curinodes) {
		fprintf(stderr,
			"FAIL: empty quota usage for type=%d id=%u (space=%llu inodes=%llu)\n",
			type, id, (unsigned long long)dq.dqb_curspace,
			(unsigned long long)dq.dqb_curinodes);
		exit(EXIT_FAILURE);
	}
}

static void seed_id(const char *dir, unsigned int id)
{
	struct fsxattr fsx = {};
	char path[4096];
	char data[4096];
	ssize_t written;
	int fd;

	expect(snprintf(path, sizeof(path), "%s/quota-%u", dir, id) > 0,
	       "format quota test path");
	unlink(path);
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd < 0)
		fail("open quota test file");
	memset(data, (unsigned char)id, sizeof(data));
	written = write(fd, data, sizeof(data));
	if (written != sizeof(data))
		fail("write quota test file");

	if (ioctl(fd, FS_IOC_FSGETXATTR, &fsx))
		fail("FS_IOC_FSGETXATTR");
	fsx.fsx_projid = id;
	if (ioctl(fd, FS_IOC_FSSETXATTR, &fsx))
		fail("FS_IOC_FSSETXATTR");
	if (fchown(fd, id, id))
		fail("fchown quota test file");
	if (fsync(fd))
		fail("fsync quota test file");

	if (close(fd))
		fail("close quota test file");
}

static void verify_id(const char *dir, const char *device, unsigned int id)
{
	char path[4096];

	expect(snprintf(path, sizeof(path), "%s/quota-%u", dir, id) > 0,
	       "format quota test path");
	check_quota(device, USRQUOTA, id);
	check_quota(device, GRPQUOTA, id);
	check_quota(device, PRJQUOTA, id);
	if (unlink(path))
		fail("unlink quota test file");
}

int main(int argc, char **argv)
{
	static const unsigned int ids[] = { 2000, 10084, 65534 };
	struct statfs st;
	long page_size;
	size_t i;

	if (argc != 4) {
		fprintf(stderr,
			"usage: %s MOUNTPOINT BLOCK_DEVICE seed|verify\n",
			argv[0]);
		return KSFT_SKIP;
	}
	if (strcmp(argv[3], "seed") && strcmp(argv[3], "verify")) {
		fprintf(stderr,
			"usage: %s MOUNTPOINT BLOCK_DEVICE seed|verify\n",
			argv[0]);
		return KSFT_SKIP;
	}
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 4096) {
		printf("SKIP: PAGE_SIZE is not larger than 4K\n");
		return KSFT_SKIP;
	}
	if (statfs(argv[1], &st))
		fail("statfs mountpoint");
	if ((unsigned long)st.f_type != F2FS_SUPER_MAGIC) {
		printf("SKIP: mountpoint is not F2FS\n");
		return KSFT_SKIP;
	}
	if ((uint64_t)st.f_bsize >= (uint64_t)page_size) {
		printf("SKIP: filesystem block size is not subpage\n");
		return KSFT_SKIP;
	}

	if (!strcmp(argv[3], "seed")) {
		for (i = 0; i < ARRAY_SIZE(ids); i++)
			seed_id(argv[1], ids[i]);
		printf("PASS: seeded subpage quota records\n");
		return EXIT_SUCCESS;
	}

	for (i = 0; i < ARRAY_SIZE(ids); i++)
		verify_id(argv[1], argv[2], ids[i]);

	printf("PASS: verified subpage quota records after remount\n");
	return EXIT_SUCCESS;
}
