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

int lvremove_single(char *lv_name);

int lvremove(int argc, char **argv)
{
	int opt;
	int ret = 0;
	char *lv_name;

	if (argc == 0) {
		log_error("please enter a logical volume path");
		return EINVALID_CMD_LINE;
	}

	for (opt = 0; opt < argc; opt++) {
		lv_name = argv[opt];

		if ((ret = lvremove_single(lv_name)))
			break;

	}

	return ret;
}

int lvremove_single(char *lv_name)
{
	int ret = 0;
	char *vg_name = NULL;
	char buffer[NAME_LEN];

	struct io_space *ios;
	struct volume_group *vg;
	struct logical_volume *lv;

	ios = active_ios();

	lv_name = lvm_check_default_vg_name(lv_name, buffer, sizeof (buffer));
	/* does VG exist? */
	vg_name = vg_name_of_lv(lv_name);

	log_verbose("Finding volume group %s", vg_name);
	if (!(vg = ios->vg_read(ios, vg_name))) {
		log_error("volume group %s doesn't exist", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg->status & ACTIVE)) {
		log_error("volume group %s must be active before removing "
			  "logical volume", vg_name);
		return ECMD_FAILED;
	}

	if (!(lv = lv_find(vg, lv_name))) {
		log_error("can't find logical volume %s in volume group %s",
			  lv_name, vg_name);
		return ECMD_FAILED;
	}

	if (lv->status & SNAPSHOT_ORG) {
		log_error("can't remove logical volume %s under snapshot",
			  lv_name);
		return ECMD_FAILED;
	}

	if (lv->open) {
		log_error("can't remove open %s logical volume %s",
			  lv->status & SNAPSHOT ? "snapshot" : "",
			  lv_name);
		return ECMD_FAILED;
	}

	if (!arg_count(force_ARG)) {
		if (yes_no_prompt
		    ("Do you really want to remove %s? [y/n]: ",
		     lv_name) == 'n') {
			log_print("logical volume %s not removed", lv_name);
			return 0;
		}
	}

	log_verbose("releasing logical volume %s", lv_name);
	if (lv_remove(vg, lv)) {
		log_error("Error releasing logical volume %s", lv_name);
		return ECMD_FAILED;
	}

	log_verbose("unlinking special file %s", lv_name);
	if (!lvm_check_devfs() && unlink(lv_name) == -1)
		log_error("Error unlinking special file %s", lv_name);

	/* store it on disks */
	if (ios->vg_write(vg))
		return ECMD_FAILED;

	if ((ret = do_autobackup(vg_name, vg)))
		return ret;

	log_print("logical volume %s successfully removed", lv_name);
	return 0;
}
