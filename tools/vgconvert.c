/*
 * Copyright (C) 2001 Sistina Software
 *
 * pvcreate is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * pvcreate is distributed in the hope that it will be useful,
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

static int vgconvert_single(struct cmd_context *cmd, const char *vg_name,
			    struct volume_group *vg, int consistent,
			    void *handle)
{
	struct physical_volume *pv, *existing_pv;
	uint64_t size = 0;
	struct list mdas;
	int pvmetadatacopies = 0;
	uint64_t pvmetadatasize = 0;
	uint64_t pe_end = 0, pe_start = 0;
	struct pv_list *pvl;
	int change_made = 0;

	if (!vg) {
		log_error("Unable to find volume group \"%s\"", vg_name);
		return ECMD_FAILED;
	}

	if (!consistent) {
		unlock_vg(cmd, vg_name);
		log_error("Volume group \"%s\" inconsistent", vg_name);
		if (!(vg = recover_vg(cmd, vg_name, LCK_VG_WRITE)))
			return ECMD_FAILED;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vg->name);
		return ECMD_FAILED;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg_name);
		return ECMD_FAILED;
	}

	if (vg->fid->fmt == cmd->fmt) {
		log_error("Volume group \"%s\" already uses format %s",
			  vg_name, cmd->fmt->name);
		return ECMD_FAILED;
	}

	if (cmd->fmt->features & FMT_MDAS) {
		if (arg_sign_value(cmd, metadatasize_ARG, 0) == SIGN_MINUS) {
			log_error("Metadata size may not be negative");
			return EINVALID_CMD_LINE;
		}

		pvmetadatasize = arg_uint64_value(cmd, metadatasize_ARG,
						  UINT64_C(0)) * 2;
		if (!pvmetadatasize)
			pvmetadatasize =
			    find_config_int(cmd->cf->root,
					    "metadata/pvmetadatasize",
					    '/', DEFAULT_PVMETADATASIZE);

		pvmetadatacopies = arg_int_value(cmd, metadatacopies_ARG, -1);
		if (pvmetadatacopies < 0)
			pvmetadatacopies =
			    find_config_int(cmd->cf->root,
					    "metadata/pvmetadatacopies",
					    '/', DEFAULT_PVMETADATACOPIES);
	}

	if (!archive(vg)) {
		log_error("Archive of \"%s\" metadata failed.", vg_name);
		return ECMD_FAILED;
	}

	list_iterate_items(pvl, &vg->pvs) {
		existing_pv = pvl->pv;

		pe_start = existing_pv->pe_start;
		pe_end = existing_pv->pe_count * existing_pv->pe_size
		    + pe_start - 1;

		list_init(&mdas);
		if (!(pv = pv_create(cmd->fmt, existing_pv->dev,
				     &existing_pv->id, size,
				     pe_start, existing_pv->pe_count,
				     existing_pv->pe_size, pvmetadatacopies,
				     pvmetadatasize, &mdas))) {
			log_error("Failed to setup physical volume \"%s\"",
				  dev_name(existing_pv->dev));
			if (change_made)
				log_error("Use pvcreate and vgcfgrestore to "
					  "repair from archived metadata.");
			return ECMD_FAILED;
		}

		/* Need to revert manually if it fails after this point */
		change_made = 1;

		log_verbose("Set up physical volume for \"%s\" with %" PRIu64
			    " available sectors", dev_name(pv->dev), pv->size);

		/* Wipe existing label first */
		if (!label_remove(pv->dev)) {
			log_error("Failed to wipe existing label on %s",
				  dev_name(pv->dev));
			log_error("Use pvcreate and vgcfgrestore to repair "
				  "from archived metadata.");
			return ECMD_FAILED;
		}

		log_very_verbose("Writing physical volume data to disk \"%s\"",
				 dev_name(pv->dev));
		if (!(pv_write(cmd, pv, &mdas,
			       arg_int64_value(cmd, labelsector_ARG,
					       DEFAULT_LABELSECTOR)))) {
			log_error("Failed to write physical volume \"%s\"",
				  dev_name(pv->dev));
			log_error("Use pvcreate and vgcfgrestore to repair "
				  "from archived metadata.");
			return ECMD_FAILED;
		}
		log_verbose("Physical volume \"%s\" successfully created",
			    dev_name(pv->dev));

	}

	log_verbose("Deleting existing metadata for VG %s", vg_name);
	if (!vg_remove(vg)) {
		log_error("Removal of existing metadata for %s failed.",
			  vg_name);
		log_error("Use pvcreate and vgcfgrestore to repair "
			  "from archived metadata.");
		return ECMD_FAILED;
	}

	/* FIXME Cache the label format change so we don't have to skip this */
	if (test_mode()) {
		log_verbose("Test mode: Skipping metadata writing for VG %s in"
			    " format %s", vg_name, cmd->fmt->name);
		return ECMD_PROCESSED;
	}

	log_verbose("Writing metadata for VG %s using format %s", vg_name,
		    cmd->fmt->name);
	if (!backup_restore_vg(cmd, vg)) {
		log_error("Conversion failed for volume group %s.", vg_name);
		log_error("Use pvcreate and vgcfgrestore to repair from "
			  "archived metadata.");
		return ECMD_FAILED;
	}
	log_print("Volume group %s successfully converted", vg_name);

	backup(vg);

	return ECMD_PROCESSED;
}

int vgconvert(struct cmd_context *cmd, int argc, char **argv)
{
	if (!argc) {
		log_error("Please enter volume group(s)");
		return EINVALID_CMD_LINE;
	}

	if (arg_int_value(cmd, labelsector_ARG, 0) >= LABEL_SCAN_SECTORS) {
		log_error("labelsector must be less than %lu",
			  LABEL_SCAN_SECTORS);
		return EINVALID_CMD_LINE;
	}

	if (!(cmd->fmt->features & FMT_MDAS) &&
	    (arg_count(cmd, metadatacopies_ARG) ||
	     arg_count(cmd, metadatasize_ARG))) {
		log_error("Metadata parameters only apply to text format");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, metadatacopies_ARG) &&
	    arg_int_value(cmd, metadatacopies_ARG, -1) > 2) {
		log_error("Metadatacopies may only be 0, 1 or 2");
		return EINVALID_CMD_LINE;
	}

	return process_each_vg(cmd, argc, argv, LCK_VG_WRITE, 0, NULL,
			       &vgconvert_single);
}
