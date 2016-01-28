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

static int vgscan_single(struct cmd_context *cmd, const char *vg_name,
			 struct volume_group *vg,
			 struct processing_handle *handle __attribute__((unused)))
{
	log_print_unless_silent("Found %svolume group \"%s\" using metadata type %s",
				vg_is_exported(vg) ? "exported " : "", vg_name,
				vg->fid->fmt->name);

	check_current_backup(vg);

	return ECMD_PROCESSED;
}

int vgscan(struct cmd_context *cmd, int argc, char **argv)
{
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

	if (lvmetad_used() && (arg_is_set(cmd, cache_long_ARG) || !lvmetad_token_matches(cmd))) {
		if (lvmetad_used() && !lvmetad_pvscan_all_devs(cmd, NULL, arg_is_set(cmd, cache_long_ARG))) {
			log_warn("WARNING: Not using lvmetad because cache update failed.");
			lvmetad_set_active(cmd, 0);
		}
	}

	if (!lvmetad_used())
		log_print_unless_silent("Reading all physical volumes.  This may take a while...");
	else
		log_print_unless_silent("Reading volume groups from cache.");

	maxret = process_each_vg(cmd, argc, argv, NULL, 0, NULL,
				 &vgscan_single);

	if (arg_count(cmd, mknodes_ARG)) {
		ret = vgmknodes(cmd, argc, argv);
		if (ret > maxret)
			maxret = ret;
	}

	unlock_vg(cmd, VG_GLOBAL);
	return maxret;
}
