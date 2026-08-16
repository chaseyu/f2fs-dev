#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

KSFT_SKIP=4
tmp=
loopdev=
mounted=0
mkfs=${MKFS_F2FS:-mkfs.f2fs}
fsck=${FSCK_F2FS:-fsck.f2fs}
summary_test=${F2FS_SUBPAGE_COMPACT_SUMMARY_TEST:-$(dirname "$0")/f2fs_subpage_compact_summary}

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
for tool in cat getconf grep losetup mount truncate umount; do
	command -v "$tool" >/dev/null 2>&1 || skip "$tool is not installed"
done
command -v "$mkfs" >/dev/null 2>&1 || skip "$mkfs is not installed"
command -v "$fsck" >/dev/null 2>&1 || skip "$fsck is not installed"
[ -x "$summary_test" ] || skip "$summary_test is not executable"
[ "$(getconf PAGESIZE)" -gt 4096 ] || skip "PAGE_SIZE is not larger than 4K"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/f2fs-subpage-compact-summary.XXXXXX")
mkdir "$tmp/mnt"
truncate -s 512M "$tmp/fs.img"
"$mkfs" -q -f -b 4096 "$tmp/fs.img" || fail "format 4K F2FS image"

attach_loop
mount -t f2fs "$loopdev" "$tmp/mnt" || fail "initial subpage mount"
mounted=1
"$summary_test" "$tmp/mnt" seed || fail "seed compact summaries"
umount "$tmp/mnt" || fail "unmount seeded filesystem"
mounted=0
losetup -d "$loopdev" >/dev/null 2>&1 || true
loopdev=

attach_loop
mount -t f2fs "$loopdev" "$tmp/mnt" || fail "compact-summary remount"
mounted=1
"$summary_test" "$tmp/mnt" verify || fail "verify compact summaries"
umount "$tmp/mnt" || fail "unmount verified filesystem"
mounted=0
losetup -d "$loopdev" >/dev/null 2>&1 || true
loopdev=

if ! "$fsck" --dry-run -f "$tmp/fs.img" >"$tmp/fsck.log" 2>&1; then
	cat "$tmp/fsck.log" >&2
	fail "fsck rejected compact summaries after remount"
fi
if grep -q '\[ASSERT\]' "$tmp/fsck.log"; then
	cat "$tmp/fsck.log" >&2
	fail "fsck found invalid compact summaries after remount"
fi

echo "PASS: F2FS subpage compact summaries survived remount"
