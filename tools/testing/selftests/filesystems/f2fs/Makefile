# SPDX-License-Identifier: GPL-2.0

CFLAGS += -O2 -g -Wall -Wextra -Werror $(KHDR_INCLUDES)
TEST_GEN_PROGS := f2fs_subpage f2fs_subpage_compact_summary \
		  f2fs_subpage_dirsync f2fs_subpage_quota f2fs_subpage_stress
TEST_PROGS := f2fs_subpage_compact_summary.sh f2fs_subpage_dirsync.sh \
	      f2fs_subpage_quota.sh f2fs_subpage_ro.sh

f2fs_subpage_dirsync: LDLIBS += -pthread

include ../../lib.mk
