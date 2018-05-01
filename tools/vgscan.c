/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

static int _vgscan_single(struct cmd_context *cmd, const char *vg_name,
			  struct volume_group *vg,
			  struct processing_handle *handle __attribute__((unused)))
{
	log_print_unless_silent("Found %svolume group \"%s\" using metadata type %s",
				vg_is_exported(vg) ? "exported " : "", vg_name,
				vg->fid->fmt->name);

	check_current_backup(vg);

	return ECMD_PROCESSED;
}

/*
 * Two main vgscan cases related to lvmetad usage:
 * 1. vgscan
 * 2. vgscan --cache
 *
 * 1. The 'vgscan' command (without --cache) may or may not attempt to
 * repopulate the lvmetad cache, and may or may not use the lvmetad
 * cache to display VG info:
 *
 * i. If lvmetad is being used and is in a normal state, then 'vgscan'
 * will simply read and display VG info from the lvmetad cache.
 *
 * ii. If lvmetad is not being used, 'vgscan' will read all devices to
 * display the VG info.
 *
 * iii. If lvmetad is being used, but has been disabled (because of
 * duplicate devs), or has a non-matching token
 * (because the device filter is different from the device filter last
 * used to populate lvmetad), then 'vgscan' will begin by rescanning
 * devices to repopulate lvmetad.  If lvmetad is enabled after the
 * rescan, then 'vgscan' will simply read and display VG info from the
 * lvmetad cache (like case i).  If lvmetad is disabled after the
 * rescan, then 'vgscan' will read all devices to display VG info
 * (like case ii).
 *
 * 2. The 'vgscan --cache' command will always attempt to repopulate
 * the lvmetad cache by rescanning all devs (regardless of whether
 * lvmetad was previously disabled or had an unmatching token.)
 * lvmetad may be enabled or disabled after the rescan (depending
 * on whether duplicate devs were found).
 * If enabled, then it will simply read and display VG info from the
 * lvmetad cache (like case 1.i.).  If disabled, then it will
 * read all devices to display VG info (like case 1.ii.)
 */

int vgscan(struct cmd_context *cmd, int argc, char **argv)
{
	const char *reason = NULL;
	int maxret, ret;

	if (argc) {
		log_error("Too many parameters on command line");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, notifydbus_ARG)) {
		if (!lvmnotify_is_supported()) {
			log_error("Cannot notify dbus: lvm is not built with dbus support.");
			return ECMD_FAILED;
		}
		if (!find_config_tree_bool(cmd, global_notify_dbus_CFG, NULL)) {
			log_error("Cannot notify dbus: notify_dbus is disabled in lvm config.");
			return ECMD_FAILED;
		}
		set_pv_notify(cmd);
		set_vg_notify(cmd);
		set_lv_notify(cmd);
		return ECMD_PROCESSED;
	}

	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	if (cmd->filter->wipe)
		cmd->filter->wipe(cmd->filter);
	lvmcache_destroy(cmd, 1, 0);

	if (!lvmetad_used() && arg_is_set(cmd, cache_long_ARG))
		log_verbose("Ignoring vgscan --cache command because lvmetad is not in use.");

	if (lvmetad_used() && (arg_is_set(cmd, cache_long_ARG) || !lvmetad_token_matches(cmd) || lvmetad_is_disabled(cmd, &reason))) {
		if (lvmetad_used() && !lvmetad_pvscan_all_devs(cmd, arg_is_set(cmd, cache_long_ARG))) {
			log_warn("WARNING: Not using lvmetad because cache update failed.");
			lvmetad_make_unused(cmd);
		}

		if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
			log_warn("WARNING: Not using lvmetad because %s.", reason);
			lvmetad_make_unused(cmd);
		}
	}

	if (!lvmetad_used())
		log_print_unless_silent("Reading all physical volumes.  This may take a while...");
	else
		log_print_unless_silent("Reading volume groups from cache.");

	maxret = process_each_vg(cmd, argc, argv, NULL, NULL, 0, 0, NULL, &_vgscan_single);

	if (arg_is_set(cmd, mknodes_ARG)) {
		ret = vgmknodes(cmd, argc, argv);
		if (ret > maxret)
			maxret = ret;
	}

	unlock_vg(cmd, NULL, VG_GLOBAL);
	return maxret;
}
