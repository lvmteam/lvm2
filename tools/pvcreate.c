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
#include "defaults.h"

const char _really_init[] =
    "Really INITIALIZE physical volume \"%s\" of volume group \"%s\" [y/n]? ";

/*
 * See if we may pvcreate on this device.
 * 0 indicates we may not.
 */
static int pvcreate_check(struct cmd_context *cmd, const char *name)
{
	struct physical_volume *pv;

	/* is the partition type set correctly ? */
	if ((arg_count(cmd, force_ARG) < 1) && !is_lvm_partition(name)) {
		log_error("%s: Not LVM partition type: use -f to override",
			  name);
		return 0;
	}

	/* is there a pv here already */
	if (!(pv = pv_read(cmd, name, NULL, NULL)))
		return 1;

	/* orphan ? */
	if (!pv->vg_name[0])
		return 1;

	/* Allow partial & exported VGs to be destroyed. */
	/* we must have -ff to overwrite a non orphan */
	if (arg_count(cmd, force_ARG) < 2) {
		log_error("Can't initialize physical volume \"%s\" of "
			  "volume group \"%s\" without -ff", name, pv->vg_name);
		return 0;
	}

	/* prompt */
	if (!arg_count(cmd, yes_ARG) &&
	    yes_no_prompt(_really_init, name, pv->vg_name) == 'n') {
		log_print("%s: physical volume not initialized", name);
		return 0;
	}

	if (arg_count(cmd, force_ARG)) {
		log_print("WARNING: Forcing physical volume creation on "
			  "%s%s%s%s", name,
			  pv->vg_name[0] ? " of volume group \"" : "",
			  pv->vg_name[0] ? pv->vg_name : "",
			  pv->vg_name[0] ? "\"" : "");
	}

	return 1;
}

static void pvcreate_single(struct cmd_context *cmd, const char *pv_name,
			    void *handle)
{
	struct physical_volume *pv, *existing_pv;
	struct id id, *idp = NULL;
	char *uuid;
	uint64_t size = 0;
	struct device *dev;
	struct list mdas;
	int pvmetadatacopies;
	uint64_t pvmetadatasize;
	struct volume_group *vg;
	char *restorefile;
	uint64_t pe_start = 0;
	uint32_t extent_count = 0, extent_size = 0;

	if (arg_count(cmd, uuidstr_ARG)) {
		uuid = arg_str_value(cmd, uuidstr_ARG, "");
		if (!id_read_format(&id, uuid))
			return;
		if ((dev = device_from_pvid(cmd, &id)) &&
		    (dev != dev_cache_get(pv_name, cmd->filter))) {
			log_error("uuid %s already in use on \"%s\"", uuid,
				  dev_name(dev));
			return;
		}
		idp = &id;
	}

	if (arg_count(cmd, restorefile_ARG)) {
		restorefile = arg_str_value(cmd, restorefile_ARG, "");
		/* The uuid won't already exist */
		init_partial(1);
		if (!(vg = backup_read_vg(cmd, NULL, restorefile))) {
			log_error("Unable to read volume group from %s",
				  restorefile);
			return;
		}
		init_partial(0);
		if (!(existing_pv = find_pv_in_vg_by_uuid(vg, idp))) {
			log_error("Can't find uuid %s in backup file %s",
				  uuid, restorefile);
			return;
		}
		pe_start = existing_pv->pe_start;
		extent_size = existing_pv->pe_size;
		extent_count = existing_pv->pe_count;
	}

	if (!lock_vol(cmd, "", LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return;
	}

	if (!pvcreate_check(cmd, pv_name))
		goto error;

	size = arg_uint64_value(cmd, physicalvolumesize_ARG, 0) * 2;

	pvmetadatasize = arg_uint64_value(cmd, metadatasize_ARG, 0) * 2;
	if (!pvmetadatasize)
		pvmetadatasize = find_config_int(cmd->cf->root,
						 "metadata/pvmetadatasize",
						 '/', DEFAULT_PVMETADATASIZE);

	pvmetadatacopies = arg_int_value(cmd, metadatacopies_ARG, -1);
	if (pvmetadatacopies < 0)
		pvmetadatacopies = find_config_int(cmd->cf->root,
						   "metadata/pvmetadatacopies",
						   '/',
						   DEFAULT_PVMETADATACOPIES);

	if (!(dev = dev_cache_get(pv_name, cmd->filter))) {
		log_error("%s: Couldn't find device.", pv_name);
		goto error;
	}

	list_init(&mdas);
	if (!(pv = pv_create(cmd->fmt, dev, idp, size, pe_start,
			     extent_count, extent_size,
			     pvmetadatacopies, pvmetadatasize, &mdas))) {
		log_error("Failed to setup physical volume \"%s\"", pv_name);
		goto error;
	}

	log_verbose("Set up physical volume for \"%s\" with %" PRIu64
		    " available sectors", pv_name, pv->size);

	/* Wipe existing label first */
	if (!label_remove(pv->dev)) {
		log_error("Failed to wipe existing label on %s", pv_name);
		goto error;
	}

	log_very_verbose("Writing physical volume data to disk \"%s\"",
			 pv_name);
	if (!(pv_write(cmd, pv, &mdas, arg_int_value(cmd, labelsector_ARG,
						     DEFAULT_LABELSECTOR)))) {
		log_error("Failed to write physical volume \"%s\"", pv_name);
		goto error;
	}

	log_print("Physical volume \"%s\" successfully created", pv_name);

      error:
	unlock_vg(cmd, "");
	return;
}

int pvcreate(struct cmd_context *cmd, int argc, char **argv)
{
	int i;

	if (!argc) {
		log_error("Please enter a physical volume path");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, restorefile_ARG) && !arg_count(cmd, uuidstr_ARG)) {
		log_error("--uuid is required with --restorefile");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, uuidstr_ARG) && argc != 1) {
		log_error("Can only set uuid on one volume at once");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, yes_ARG) && !arg_count(cmd, force_ARG)) {
		log_error("Option y can only be given with option f");
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

	for (i = 0; i < argc; i++) {
		pvcreate_single(cmd, argv[i], NULL);
		pool_empty(cmd->mem);
	}

	return 0;
}
