/*
 * Copyright (C) 2001 Sistina Software
 *
 * vgcreate is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * vgcreate is distributed in the hope that it will be useful,
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

#define DEFAULT_EXTENT 4096	/* In KB */

int vgcreate(struct cmd_context *cmd, int argc, char **argv)
{
	size_t max_lv, max_pv;
	uint32_t extent_size;
	char *vg_name;
	char vg_path[PATH_MAX];
	struct volume_group *vg;
	const char *tag;

	if (!argc) {
		log_error("Please provide volume group name and "
			  "physical volumes");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume name(s)");
		return EINVALID_CMD_LINE;
	}

	vg_name = argv[0];
	max_lv = arg_uint_value(cmd, maxlogicalvolumes_ARG, 0);
	max_pv = arg_uint_value(cmd, maxphysicalvolumes_ARG, 0);

 	if (!(cmd->fmt->features & FMT_UNLIMITED_VOLS)) {
 		if (!max_lv)
 			max_lv = 255;
 		if (!max_pv)
 			max_pv = 255;
 		if (max_lv > 255 || max_pv > 255) {
 			log_error("Number of volumes may not exceed 255");
 			return EINVALID_CMD_LINE;
 		}
 	}
  
  	if (arg_sign_value(cmd, physicalextentsize_ARG, 0) == SIGN_MINUS) {
  		log_error("Physical extent size may not be negative");
  		return EINVALID_CMD_LINE;
  	}
  
 	if (arg_sign_value(cmd, maxlogicalvolumes_ARG, 0) == SIGN_MINUS) {
 		log_error("Max Logical Volumes may not be negative");
  		return EINVALID_CMD_LINE;
  	}
  
 	if (arg_sign_value(cmd, maxphysicalvolumes_ARG, 0) == SIGN_MINUS) {
 		log_error("Max Physical Volumes may not be negative");
  		return EINVALID_CMD_LINE;
  	}
  
 	/* Units of 512-byte sectors */
 	extent_size =
 	    arg_uint_value(cmd, physicalextentsize_ARG, DEFAULT_EXTENT) * 2;

	if (!extent_size) {
		log_error("Physical extent size may not be zero");
		return EINVALID_CMD_LINE;
	}

  	/* Strip dev_dir if present */
  	if (!strncmp(vg_name, cmd->dev_dir, strlen(cmd->dev_dir)))
  		vg_name += strlen(cmd->dev_dir);

	snprintf(vg_path, PATH_MAX, "%s%s", cmd->dev_dir, vg_name);
	if (path_exists(vg_path)) {
		log_error("%s: already exists in filesystem", vg_path);
		return ECMD_FAILED;
	}

	if (!validate_name(vg_name)) {
		log_error("New volume group name \"%s\" is invalid", vg_name);
		return ECMD_FAILED;
	}

	/* Create the new VG */
	if (!(vg = vg_create(cmd, vg_name, extent_size, max_pv, max_lv,
			     argc - 1, argv + 1)))
		return ECMD_FAILED;

	if (max_lv != vg->max_lv)
		log_error("Warning: Setting maxlogicalvolumes to %d "
			  "(0 means unlimited)", vg->max_lv);

	if (max_pv != vg->max_pv)
		log_error("Warning: Setting maxphysicalvolumes to %d "
			  "(0 means unlimited)", vg->max_pv);

 	if (arg_count(cmd, addtag_ARG)) {
 		if (!(tag = arg_str_value(cmd, addtag_ARG, NULL))) {
 			log_error("Failed to get tag");
 			return ECMD_FAILED;
 		}
  
  		if (!(vg->fid->fmt->features & FMT_TAGS)) {
  			log_error("Volume group format does not support tags");
  			return ECMD_FAILED;
  		}
  
 		if (!str_list_add(cmd->mem, &vg->tags, tag)) {
 			log_error("Failed to add tag %s to volume group %s",
 				  tag, vg_name);
 			return ECMD_FAILED;
 		}
 	}
 
	if (!lock_vol(cmd, "", LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return ECMD_FAILED;
	}

	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE | LCK_NONBLOCK)) {
		log_error("Can't get lock for %s", vg_name);
		unlock_vg(cmd, "");
		return ECMD_FAILED;
	}

	if (!archive(vg)) {
		unlock_vg(cmd, vg_name);
		unlock_vg(cmd, "");
		return ECMD_FAILED;
	}

	/* Store VG on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg)) {
		unlock_vg(cmd, vg_name);
		unlock_vg(cmd, "");
		return ECMD_FAILED;
	}

	unlock_vg(cmd, vg_name);
	unlock_vg(cmd, "");

	backup(vg);

	log_print("Volume group \"%s\" successfully created", vg->name);

	return ECMD_PROCESSED;
}
