/*
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "filesystem.h"
#include "lib/log/lvm-logging.h"

#ifdef HAVE_XFS_XFS_H

#include <xfs/xfs.h>

#else /* !HAVE_XFS_XFS_H */

#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>

/* #IOCTL to obtain geometry info */
#define XFS_IOC_FSGEOMETRY 0x8100587e

/* Copy of some basic and hopefully unchangable XFS header data variables */
struct xfs_fsop_geom {
	uint32_t blocksize;	/* filesystem (data) block size */
	uint32_t rtextsize;
	uint32_t agblocks;
	uint32_t agcount;
	uint32_t logblocks;
	uint32_t sectsize;
	uint32_t inodesize;
	uint32_t imaxpct;
	uint64_t datablocks;	/* fsblocks in data subvolume */
	uint32_t reserved[128];	/* whatever size whole structure may have */
};

#endif

int fs_xfs_update_size_mounted(struct cmd_context *cmd, struct logical_volume *lv,
			       char *lv_path, struct fs_info *fsi)
{
	struct xfs_fsop_geom geo = { 0 };
	int ret = 0;
	int fd;

	if ((fd = open(fsi->mount_dir, O_RDONLY)) < 0) {
		log_sys_error("XFS geometry open", fsi->mount_dir);
		return 0;
	}

	if (ioctl(fd, XFS_IOC_FSGEOMETRY, &geo)) {
		log_sys_error("XFS geometry ioctl", fsi->mount_dir);
		goto out;
	}

	/* replace the potentially wrong value from blkid_probe_lookup_value FSLASTBLOCK */
	fsi->fs_last_byte = geo.blocksize * geo.datablocks;
	ret = 1;

	log_debug("XFS geometry blocksize %llu datablocks %llu fs_last_byte %llu from %s %s.",
		  (unsigned long long)geo.blocksize, (unsigned long long)geo.datablocks,
		  (unsigned long long)fsi->fs_last_byte, lv_path, fsi->mount_dir);
out:
	(void)close(fd);

	return ret;
}
