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

int vgextend(struct cmd_context *cmd, int argc, char **argv)
{
	char *vg_name;
	struct volume_group *vg = NULL;
	int consistent = 1;

	if (!argc) {
		log_error("Please enter volume group name and "
			  "physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	vg_name = skip_dev_dir(cmd, argv[0], NULL);
	argc--;
	argv++;

	if (!lock_vol(cmd, ORPHAN, LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return ECMD_FAILED;
	}

	if (!validate_name(vg_name)) {
		log_error("Volume group name \"%s\" is invalid",
			  vg_name);
		return ECMD_FAILED;
	}

	log_verbose("Checking for volume group \"%s\"", vg_name);
	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE | LCK_NONBLOCK)) {
		unlock_vg(cmd, ORPHAN);
		log_error("Can't get lock for %s", vg_name);
		goto error;
	}

	if (!(vg = vg_read(cmd, vg_name, NULL, &consistent)) || !consistent) {
		log_error("Volume group \"%s\" not found.", vg_name);
		goto error;
	}

	if (!vg_check_status(vg, CLUSTERED | EXPORTED_VG |
				 LVM_WRITE | RESIZEABLE_VG))
		goto error;

/********** FIXME
	log_print("maximum logical volume size is %s",
		  (dummy = lvm_show_size(LVM_LV_SIZE_MAX(vg) / 2, LONG)));
	dm_free(dummy);
	dummy = NULL;
**********/

	if (!archive(vg))
		goto error;

	/* extend vg */
	if (!vg_extend(vg, argc, argv))
		goto error;

	/* ret > 0 */
	log_verbose("Volume group \"%s\" will be extended by %d new "
		    "physical volumes", vg_name, argc);

	/* store vg on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg))
		goto error;

	backup(vg);

	unlock_vg(cmd, vg_name);
	unlock_vg(cmd, ORPHAN);

	log_print("Volume group \"%s\" successfully extended", vg_name);

	return ECMD_PROCESSED;

      error:
	unlock_vg(cmd, vg_name);
	unlock_vg(cmd, ORPHAN);
	return ECMD_FAILED;
}
