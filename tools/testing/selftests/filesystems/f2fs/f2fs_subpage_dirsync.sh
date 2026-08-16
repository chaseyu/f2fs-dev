#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

KSFT_SKIP=4
TMPDIR=${TMPDIR:-/tmp}
MKFS_F2FS=${MKFS_F2FS:-mkfs.f2fs}
TEST_BINARY=${F2FS_SUBPAGE_DIRSYNC_TEST:-./f2fs_subpage_dirsync}
ROUNDS=${F2FS_SUBPAGE_DIRSYNC_ROUNDS:-200}
WORKDIR="$TMPDIR/f2fs-subpage-dirsync-$$"
IMAGE="$WORKDIR/image"
MOUNTPOINT="$WORKDIR/mnt"
LOOPDEV=

cleanup()
{
	if mountpoint -q "$MOUNTPOINT" 2>/dev/null; then
		umount "$MOUNTPOINT" || true
	fi
	if [ -n "$LOOPDEV" ] && losetup "$LOOPDEV" >/dev/null 2>&1; then
		losetup -d "$LOOPDEV" || true
	fi
	rm -rf "$WORKDIR"
}
trap cleanup EXIT INT TERM

if [ "$(id -u)" -ne 0 ]; then
	echo "SKIP: root privileges are required"
	exit "$KSFT_SKIP"
fi
if [ "$(getconf PAGE_SIZE)" -le 4096 ]; then
	echo "SKIP: PAGE_SIZE is not larger than 4K"
	exit "$KSFT_SKIP"
fi
for tool in "$MKFS_F2FS" losetup mount umount mountpoint truncate; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "SKIP: missing $tool"
		exit "$KSFT_SKIP"
	fi
done

mkdir -p "$MOUNTPOINT"
truncate -s 512M "$IMAGE"
"$MKFS_F2FS" -q -f -b 4096 "$IMAGE"
LOOPDEV=$(losetup -f --show "$IMAGE")
mount -t f2fs "$LOOPDEV" "$MOUNTPOINT"
"$TEST_BINARY" "$MOUNTPOINT" "$ROUNDS"
sync
umount "$MOUNTPOINT"
losetup -d "$LOOPDEV"
LOOPDEV=
echo "PASS: F2FS subpage directory sync regression"
