#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

KSFT_SKIP=4
tmp=
loopdev=
mounted=0

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

trap cleanup EXIT INT TERM

[ "$(id -u)" -eq 0 ] || skip "must run as root"
for tool in cmp dd find losetup mkfs.f2fs mount sha256sum sload.f2fs \
		truncate umount; do
	command -v "$tool" >/dev/null 2>&1 || skip "$tool is not installed"
done

page_size=$(getconf PAGESIZE)
[ "$page_size" -gt 4096 ] || skip "PAGE_SIZE is not larger than 4K"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/f2fs-subpage-ro.XXXXXX")
mkdir "$tmp/src" "$tmp/src/nested" "$tmp/mnt"

dd if=/dev/urandom of="$tmp/src/pattern.bin" bs=65537 count=1 status=none
printf 'cross-block-read' > "$tmp/src/nested/boundary.bin"
truncate -s 131072 "$tmp/src/sparse.bin"
printf 'first' | dd of="$tmp/src/sparse.bin" bs=1 seek=4093 conv=notrunc status=none
printf 'second' | dd of="$tmp/src/sparse.bin" bs=1 seek=65533 conv=notrunc status=none

truncate -s 256M "$tmp/fs.img"
mkfs.f2fs -f "$tmp/fs.img" >/dev/null
sload.f2fs -f "$tmp/src" "$tmp/fs.img" >/dev/null
before=$(sha256sum "$tmp/fs.img" | awk '{print $1}')

loopdev=$(losetup --find --show --read-only "$tmp/fs.img")

# The read-path-only commit must reject every route that can write.
if mount -t f2fs "$loopdev" "$tmp/mnt" >/dev/null 2>&1; then
	mounted=1
	fail "subpage filesystem mounted read-write"
fi
if mount -t f2fs -o ro,norecovery "$loopdev" "$tmp/mnt" >/dev/null 2>&1; then
	mounted=1
	fail "norecovery bypassed the strict read-only mount checks"
fi
if mount -t f2fs -o ro,checkpoint=disable "$loopdev" "$tmp/mnt" \
		>/dev/null 2>&1; then
	mounted=1
	fail "checkpoint=disable bypassed the strict read-only mount checks"
fi

mount -t f2fs -o ro "$loopdev" "$tmp/mnt" || fail "clean read-only mount"
mounted=1

cmp "$tmp/src/pattern.bin" "$tmp/mnt/pattern.bin" || fail "pattern read"
cmp "$tmp/src/nested/boundary.bin" "$tmp/mnt/nested/boundary.bin" || \
	fail "nested directory read"
cmp "$tmp/src/sparse.bin" "$tmp/mnt/sparse.bin" || fail "sparse file read"
find "$tmp/mnt" -xdev -print >/dev/null || fail "directory traversal"

if sh -c 'printf x > "$1/must-not-exist"' sh "$tmp/mnt" 2>/dev/null; then
	fail "file creation succeeded on a read-only subpage mount"
fi
if truncate -s 0 "$tmp/mnt/pattern.bin" 2>/dev/null; then
	fail "truncate succeeded on a read-only subpage mount"
fi
if rm "$tmp/mnt/pattern.bin" 2>/dev/null; then
	fail "unlink succeeded on a read-only subpage mount"
fi
if mkdir "$tmp/mnt/must-not-exist" 2>/dev/null; then
	fail "mkdir succeeded on a read-only subpage mount"
fi
if mount -o remount,rw "$tmp/mnt" >/dev/null 2>&1; then
	fail "read-only subpage mount was remounted read-write"
fi

umount "$tmp/mnt"
mounted=0
losetup -d "$loopdev"
loopdev=

after=$(sha256sum "$tmp/fs.img" | awk '{print $1}')
[ "$before" = "$after" ] || fail "filesystem image changed during read-only test"

echo "PASS: F2FS 4K-block image remained byte-identical on ${page_size}-byte pages"
