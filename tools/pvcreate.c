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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"
#include "metadata-exported.h"

/*
 * Intial sanity checking of recovery-related command-line arguments.
 * These args are: --restorefile, --uuid, and --physicalvolumesize
 *
 * Output arguments:
 * pp: structure allocated by caller, fields written / validated here
 */
static int pvcreate_restore_params_validate(struct cmd_context *cmd,
					    int argc, char **argv,
					    struct pvcreate_params *pp)
{
	const char *uuid = NULL;
	struct volume_group *vg;
	struct pv_list *existing_pvl;

	if (arg_count(cmd, restorefile_ARG) && !arg_count(cmd, uuidstr_ARG)) {
		log_error("--uuid is required with --restorefile");
		return 0;
	}

	if (!arg_count(cmd, restorefile_ARG) && arg_count(cmd, uuidstr_ARG)) {
		if (!arg_count(cmd, norestorefile_ARG) &&
		    find_config_tree_bool(cmd,
					  "devices/require_restorefile_with_uuid",
					  DEFAULT_REQUIRE_RESTOREFILE_WITH_UUID)) {
			log_error("--restorefile is required with --uuid");
			return 0;
		}
	}

	if (arg_count(cmd, uuidstr_ARG) && argc != 1) {
		log_error("Can only set uuid on one volume at once");
		return 0;
	}

 	if (arg_count(cmd, uuidstr_ARG)) {
		uuid = arg_str_value(cmd, uuidstr_ARG, "");
		if (!id_read_format(&pp->id, uuid))
			return 0;
		pp->idp = &pp->id;
	}

	if (arg_count(cmd, restorefile_ARG)) {
		pp->restorefile = arg_str_value(cmd, restorefile_ARG, "");
		/* The uuid won't already exist */
		if (!(vg = backup_read_vg(cmd, NULL, pp->restorefile))) {
			log_error("Unable to read volume group from %s",
				  pp->restorefile);
			return 0;
		}
		if (!(existing_pvl = find_pv_in_vg_by_uuid(vg, pp->idp))) {
			log_error("Can't find uuid %s in backup file %s",
				  uuid, pp->restorefile);
			return 0;
		}
		pp->pe_start = pv_pe_start(existing_pvl->pv);
		pp->extent_size = pv_pe_size(existing_pvl->pv);
		pp->extent_count = pv_pe_count(existing_pvl->pv);
		release_vg(vg);
	}

	if (arg_sign_value(cmd, physicalvolumesize_ARG, 0) == SIGN_MINUS) {
		log_error("Physical volume size may not be negative");
		return 0;
	}
	pp->size = arg_uint64_value(cmd, physicalvolumesize_ARG, UINT64_C(0));

	if (arg_count(cmd, restorefile_ARG) || arg_count(cmd, uuidstr_ARG))
		pp->zero = 0;
	return 1;
}

int pvcreate(struct cmd_context *cmd, int argc, char **argv)
{
	int i;
	int ret = ECMD_PROCESSED;
	struct pvcreate_params pp;
	struct physical_volume *pv;

	pvcreate_params_set_defaults(&pp);

	if (!pvcreate_restore_params_validate(cmd, argc, argv, &pp)) {
		return EINVALID_CMD_LINE;
	}
	if (!pvcreate_params_validate(cmd, argc, argv, &pp)) {
		return EINVALID_CMD_LINE;
	}

	for (i = 0; i < argc; i++) {
		if (!lock_vol(cmd, VG_ORPHANS, LCK_VG_WRITE)) {
			log_error("Can't get lock for orphan PVs");
			return ECMD_FAILED;
		}

		dm_unescape_colons_and_at_signs(argv[i], NULL, NULL);

		if (!(pv = pvcreate_single(cmd, argv[i], &pp, 1))) {
			stack;
			ret = ECMD_FAILED;
		}

		free_pv_fid(pv);
		unlock_vg(cmd, VG_ORPHANS);
		if (sigint_caught())
			return ret;
	}

	return ret;
}
