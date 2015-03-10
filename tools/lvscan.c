/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

static int _lvscan_single_lvmetad(struct cmd_context *cmd, struct logical_volume *lv)
{
	struct pv_list *pvl;
	struct dm_list all_pvs;
	char pvid_s[64] __attribute__((aligned(8)));

	if (!lvmetad_used()) {
		log_verbose("Ignoring lvscan --cache because lvmetad is not in use.");
		return ECMD_PROCESSED;
	}

	dm_list_init(&all_pvs);

	if (!get_pv_list_for_lv(lv->vg->vgmem, lv, &all_pvs))
		return ECMD_FAILED;

	dm_list_iterate_items(pvl, &all_pvs) {
		if (!pvl->pv->dev) {
			if (!id_write_format(&pvl->pv->id, pvid_s, sizeof(pvid_s)))
				stack;
			else
				log_warn("WARNING: Device for PV %s already missing, skipping.",
					 pvid_s);
			continue;
		}
		if (!lvmetad_pvscan_single(cmd, pvl->pv->dev, NULL, 0))
			return ECMD_FAILED;
	}

	return ECMD_PROCESSED;
}

static int lvscan_single(struct cmd_context *cmd, struct logical_volume *lv,
			 struct processing_handle *handle __attribute__((unused)))
{
	struct lvinfo info;
	int inkernel, snap_active = 1;
	dm_percent_t snap_percent;     /* fused, fsize; */

	const char *active_str, *snapshot_str;

	if (arg_count(cmd, cache_long_ARG))
		return _lvscan_single_lvmetad(cmd, lv);

	if (!arg_count(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	inkernel = lv_info(cmd, lv, 0, &info, 0, 0) && info.exists;
	if (lv_is_cow(lv)) {
		if (inkernel &&
		    (snap_active = lv_snapshot_percent(lv, &snap_percent)))
			if (snap_percent == DM_PERCENT_INVALID)
				snap_active = 0;
	}

/* FIXME Add -D arg to skip this! */
	if (inkernel && snap_active)
		active_str = "ACTIVE   ";
	else
		active_str = "inactive ";

	if (lv_is_origin(lv))
		snapshot_str = "Original";
	else if (lv_is_cow(lv))
		snapshot_str = "Snapshot";
	else
		snapshot_str = "        ";

	log_print_unless_silent("%s%s '%s%s/%s' [%s] %s", active_str, snapshot_str,
				cmd->dev_dir, lv->vg->name, lv->name,
				display_size(cmd, lv->size),
				get_alloc_string(lv->alloc));

	return ECMD_PROCESSED;
}

int lvscan(struct cmd_context *cmd, int argc, char **argv)
{
	if (argc && !arg_count(cmd, cache_long_ARG)) {
		log_error("No additional command line arguments allowed");
		return EINVALID_CMD_LINE;
	}

	return process_each_lv(cmd, argc, argv, 0, NULL,
			       &lvscan_single);
}
