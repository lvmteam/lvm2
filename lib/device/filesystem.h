/*
 * Copyright (C) 2022 Red Hat, Inc. All rights reserved.
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

#ifndef _FILESYSTEM_H
#define _FILESYSTEM_H

#define FSTYPE_MAX 16

struct fs_info {
	char fstype[FSTYPE_MAX];
	char mount_dir[PATH_MAX];
	char fs_dev_path[PATH_MAX]; /* usually lv dev, can be crypt dev */
	unsigned int fs_block_size_bytes; /* 512 or 4k */
	uint64_t fs_last_byte; /* last byte on the device used by the fs */
	uint32_t crypt_offset_bytes; /* offset in bytes of crypt data on LV */
	dev_t crypt_devt; /* dm-crypt device between the LV and FS */
	uint64_t crypt_dev_size_bytes;

	unsigned nofs:1;
	unsigned unmounted:1;
	unsigned mounted:1;
	/* for resizing */
	unsigned needs_reduce:1;
	unsigned needs_extend:1;
	unsigned needs_fsck:1;
	unsigned needs_unmount:1;
	unsigned needs_mount:1;
	unsigned needs_crypt:1;
};

int fs_get_info(struct cmd_context *cmd, struct logical_volume *lv,
                struct fs_info *fsi, int include_mount);

int fs_extend_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		uint64_t newsize_bytes, char *fsmode);
int fs_reduce_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		uint64_t newsize_bytes, char *fsmode);
int crypt_resize_script(struct cmd_context *cmd, struct logical_volume *lv, struct fs_info *fsi,
		uint64_t newsize_bytes_fs);

int fs_mount_state_is_misnamed(struct cmd_context *cmd, struct logical_volume *lv, char *lv_path, char *fstype);
int lv_crypt_is_active(struct cmd_context *cmd, char *lv_path);

#endif
