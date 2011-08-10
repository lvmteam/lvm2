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

static int _restore_pv(struct volume_group *vg, char *pv_name)
{
	struct pv_list *pvl = NULL;
	pvl = find_pv_in_vg(vg, pv_name);
	if (!pvl) {
		log_warn("WARNING: PV %s not found in VG %s", pv_name, vg->name);
		return 0;
	}

	if (!(pvl->pv->status & MISSING_PV)) {
		log_warn("WARNING: PV %s was not missing in VG %s", pv_name, vg->name);
		return 0;
	}

	if (!pvl->pv->dev) {
		log_warn("WARNING: The PV %s is still missing.", pv_name);
		return 0;
	}

	pvl->pv->status &= ~MISSING_PV;
	return 1;
}

int vgextend(struct cmd_context *cmd, int argc, char **argv)
{
	const char *vg_name;
	struct volume_group *vg = NULL;
	int r = ECMD_FAILED;
	struct pvcreate_params pp;
	int fixed = 0, i = 0;

	if (!argc) {
		log_error("Please enter volume group name and "
			  "physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	vg_name = skip_dev_dir(cmd, argv[0], NULL);
	argc--;
	argv++;

	if (arg_count(cmd, metadatacopies_ARG)) {
		log_error("Invalid option --metadatacopies, "
			  "use --pvmetadatacopies instead.");
		return EINVALID_CMD_LINE;
	}
	pvcreate_params_set_defaults(&pp);
	if (!pvcreate_params_validate(cmd, argc, argv, &pp)) {
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, restoremissing_ARG))
		cmd->handles_missing_pvs = 1;

	log_verbose("Checking for volume group \"%s\"", vg_name);
	vg = vg_read_for_update(cmd, vg_name, NULL, 0);
	if (vg_read_error(vg)) {
		release_vg(vg);
		stack;
		return ECMD_FAILED;
	}

	if (!archive(vg))
		goto_bad;

	if (arg_count(cmd, restoremissing_ARG)) {
		for (i = 0; i < argc; ++i) {
			if (_restore_pv(vg, argv[i]))
				++ fixed;
		}
		if (!fixed) {
			log_error("No PV has been restored.");
			goto_bad;
		}
	} else { /* no --restore, normal vgextend */
		if (!lock_vol(cmd, VG_ORPHANS, LCK_VG_WRITE)) {
			log_error("Can't get lock for orphan PVs");
			unlock_and_release_vg(cmd, vg, vg_name);
			return ECMD_FAILED;
		}

		if (arg_count(cmd, metadataignore_ARG) &&
		    (vg_mda_copies(vg) != VGMETADATACOPIES_UNMANAGED) &&
		    (pp.force == PROMPT) &&
		    yes_no_prompt("Override preferred number of copies "
			  "of VG %s metadata? [y/n]: ",
				  vg_name) == 'n') {
			log_error("Volume group %s not changed", vg_name);
			goto_bad;
		}

		/* extend vg */
		if (!vg_extend(vg, argc, (const char* const*)argv, &pp))
			goto_bad;

		if (arg_count(cmd, metadataignore_ARG) &&
		    (vg_mda_copies(vg) != VGMETADATACOPIES_UNMANAGED) &&
		    (vg_mda_copies(vg) != vg_mda_used_count(vg))) {
			log_warn("WARNING: Changing preferred number of copies of VG %s "
			 "metadata from %"PRIu32" to %"PRIu32, vg_name,
				 vg_mda_copies(vg), vg_mda_used_count(vg));
			vg_set_mda_copies(vg, vg_mda_used_count(vg));
		}

		/* ret > 0 */
		log_verbose("Volume group \"%s\" will be extended by %d new "
			    "physical volumes", vg_name, argc);
	}

	/* store vg on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg))
		goto_bad;

	backup(vg);
	log_print("Volume group \"%s\" successfully extended", vg_name);
	r = ECMD_PROCESSED;

bad:
	if (!arg_count(cmd, restoremissing_ARG))
		unlock_vg(cmd, VG_ORPHANS);
	unlock_and_release_vg(cmd, vg, vg_name);
	return r;
}
