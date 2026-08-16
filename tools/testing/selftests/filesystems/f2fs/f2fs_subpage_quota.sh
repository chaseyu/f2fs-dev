#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

KSFT_SKIP=4
tmp=
loopdev=
mounted=0
mkfs=${MKFS_F2FS:-mkfs.f2fs}
quota_test=${F2FS_SUBPAGE_QUOTA_TEST:-$(dirname "$0")/f2fs_subpage_quota}

skip()
{
	echo "SKIP: $*"
	exit "$KSFT_SKIP"
}

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

cleanup()
{
	if [ "$mounted" -eq 1 ]; then
		umount "$tmp/mnt" >/dev/null 2>&1 || true
	fi
	if [ -n "$loopdev" ]; then
		losetup -d "$loopdev" >/dev/null 2>&1 || true
	fi
	if [ -n "$tmp" ]; then
		rm -rf "$tmp"
	fi
}

attach_loop()
{
	loopdev=$(losetup -f) || fail "find free loop device"
	losetup "$loopdev" "$tmp/fs.img" || fail "attach loop device"
}

trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || skip "must run as root"
for tool in getconf losetup mount truncate umount; do
	command -v "$tool" >/dev/null 2>&1 || skip "$tool is not installed"
done
command -v "$mkfs" >/dev/null 2>&1 || skip "$mkfs is not installed"
[ -x "$quota_test" ] || skip "$quota_test is not executable"

page_size=$(getconf PAGESIZE)
[ "$page_size" -gt 4096 ] || skip "PAGE_SIZE is not larger than 4K"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/f2fs-subpage-quota.XXXXXX")
mkdir "$tmp/mnt"
truncate -s 256M "$tmp/fs.img"
"$mkfs" -q -f -g android -l data -b 4096 -w 4096 "$tmp/fs.img" || \
	fail "format quota-enabled F2FS image"

attach_loop
mount -t f2fs -o usrquota,grpquota,prjquota "$loopdev" "$tmp/mnt" || \
	fail "initial quota mount"
mounted=1
"$quota_test" "$tmp/mnt" "$loopdev" seed || fail "seed quota records"
sync
umount "$tmp/mnt" || fail "unmount seeded filesystem"
mounted=0
losetup -d "$loopdev" >/dev/null 2>&1 || true
loopdev=

# Drop the dquot and page caches by detaching and remounting the image.  The
# read must then come from the on-disk quota inode rather than the live dquot.
attach_loop
mount -t f2fs -o usrquota,grpquota,prjquota "$loopdev" "$tmp/mnt" || \
	fail "quota remount"
mounted=1
"$quota_test" "$tmp/mnt" "$loopdev" verify || \
	fail "verify quota records after remount"

echo "PASS: F2FS subpage quota records survived remount"
