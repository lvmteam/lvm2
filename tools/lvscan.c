/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

static int lvscan_single(struct cmd_context *cmd, struct logical_volume *lv,
			 void *handle)
{
	struct lvinfo info;
	int lv_total = 0;
	uint64_t lv_capacity_total = 0;

	const char *active_str, *snapshot_str;

/* FIXME Add -D arg to skip this! */
	if (lv_info(lv, &info) && info.exists)
		active_str = "ACTIVE   ";
	else
		active_str = "inactive ";

	if (lv_is_origin(lv))
		snapshot_str = "Original";
	else if (lv_is_cow(lv))
		snapshot_str = "Snapshot";
	else
		snapshot_str = "        ";

	log_print("%s%s '%s%s/%s' [%s] %s", active_str, snapshot_str,
		  cmd->dev_dir, lv->vg->name, lv->name,
		  display_size(cmd, lv->size / 2, SIZE_SHORT),
		  get_alloc_string(lv->alloc));

	lv_total++;

	lv_capacity_total += lv->size;

	return ECMD_PROCESSED;
}

int lvscan(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc) {
		log_error("No additional command line arguments allowed");
		return EINVALID_CMD_LINE;
	}

	return process_each_lv(cmd, argc, argv, LCK_VG_READ, NULL,
			       &lvscan_single);
}
