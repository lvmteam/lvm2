/*
 * Copyright (C) 2001  Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include "tools.h"

static int lvscan_single(struct cmd_context *cmd, struct logical_volume *lv,
			 void *handle)
{
	struct lvinfo info;
	int lv_total = 0;
	ulong lv_capacity_total = 0;

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

	/* FIXME sprintf? */

	lv_total++;

	lv_capacity_total += lv->size;

	return 0;
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
