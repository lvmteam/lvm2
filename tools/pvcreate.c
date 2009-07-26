/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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
 * Intial sanity checking of command-line arguments and fill in 'pp' fields.
 *
 * Input arguments:
 * cmd, argc, argv
 *
 * Output arguments:
 * pp: structure allocated by caller, fields written / validated here
 */
static int pvcreate_validate_params(struct cmd_context *cmd,
				    int argc, char **argv,
				    struct pvcreate_params *pp)
{
	const char *uuid = NULL;
	void *existing_pv;
	struct volume_group *vg;

	memset(pp, 0, sizeof(*pp));

	if (!argc) {
		log_error("Please enter a physical volume path");
		return 0;
	}

	if (arg_count(cmd, restorefile_ARG) && !arg_count(cmd, uuidstr_ARG)) {
		log_error("--uuid is required with --restorefile");
		return 0;
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
		if (!(existing_pv = find_pv_in_vg_by_uuid(vg, pp->idp))) {
			log_error("Can't find uuid %s in backup file %s",
				  uuid, pp->restorefile);
			return 0;
		}
		pp->pe_start = pv_pe_start(existing_pv);
		pp->extent_size = pv_pe_size(existing_pv);
		pp->extent_count = pv_pe_count(existing_pv);
		vg_release(vg);
	}

	if (arg_count(cmd, yes_ARG) && !arg_count(cmd, force_ARG)) {
		log_error("Option y can only be given with option f");
		return 0;
	}

	pp->yes = arg_count(cmd, yes_ARG);
	pp->force = arg_count(cmd, force_ARG);

	if (arg_int_value(cmd, labelsector_ARG, 0) >= LABEL_SCAN_SECTORS) {
		log_error("labelsector must be less than %lu",
			  LABEL_SCAN_SECTORS);
		return 0;
	} else {
		pp->labelsector = arg_int64_value(cmd, labelsector_ARG,
						  DEFAULT_LABELSECTOR);
	}

	if (!(cmd->fmt->features & FMT_MDAS) &&
	    (arg_count(cmd, metadatacopies_ARG) ||
	     arg_count(cmd, metadatasize_ARG)   ||
	     arg_count(cmd, dataalignment_ARG))) {
		log_error("Metadata and data alignment parameters only "
			  "apply to text format.");
		return 0;
	}

	if (arg_count(cmd, metadatacopies_ARG) &&
	    arg_int_value(cmd, metadatacopies_ARG, -1) > 2) {
		log_error("Metadatacopies may only be 0, 1 or 2");
		return 0;
	}

	if (arg_count(cmd, zero_ARG))
		pp->zero = strcmp(arg_str_value(cmd, zero_ARG, "y"), "n");
	else if (arg_count(cmd, restorefile_ARG) || arg_count(cmd, uuidstr_ARG))
		pp->zero = 0;
	else
		pp->zero = 1;

	if (arg_sign_value(cmd, physicalvolumesize_ARG, 0) == SIGN_MINUS) {
		log_error("Physical volume size may not be negative");
		return 0;
	}
	pp->size = arg_uint64_value(cmd, physicalvolumesize_ARG, UINT64_C(0));

	if (arg_sign_value(cmd, dataalignment_ARG, 0) == SIGN_MINUS) {
		log_error("Physical volume data alignment may not be negative");
		return 0;
	}
	pp->data_alignment = arg_uint64_value(cmd, dataalignment_ARG, UINT64_C(0));

	if (pp->data_alignment > ULONG_MAX) {
		log_error("Physical volume data alignment is too big.");
		return 0;
	}

	if (pp->data_alignment && pp->pe_start) {
		if (pp->pe_start % pp->data_alignment)
			log_warn("WARNING: Ignoring data alignment %" PRIu64
				 " incompatible with --restorefile value (%"
				 PRIu64").", pp->data_alignment, pp->pe_start);
		pp->data_alignment = 0;
	}

	if (arg_sign_value(cmd, metadatasize_ARG, 0) == SIGN_MINUS) {
		log_error("Metadata size may not be negative");
		return 0;
	}

	pp->pvmetadatasize = arg_uint64_value(cmd, metadatasize_ARG, UINT64_C(0));
	if (!pp->pvmetadatasize)
		pp->pvmetadatasize = find_config_tree_int(cmd,
						 "metadata/pvmetadatasize",
						 DEFAULT_PVMETADATASIZE);

	pp->pvmetadatacopies = arg_int_value(cmd, metadatacopies_ARG, -1);
	if (pp->pvmetadatacopies < 0)
		pp->pvmetadatacopies = find_config_tree_int(cmd,
						   "metadata/pvmetadatacopies",
						   DEFAULT_PVMETADATACOPIES);

	return 1;
}

int pvcreate(struct cmd_context *cmd, int argc, char **argv)
{
	int i;
	int ret = ECMD_PROCESSED;
	struct pvcreate_params pp;

	if (!pvcreate_validate_params(cmd, argc, argv, &pp)) {
		return EINVALID_CMD_LINE;
	}

	for (i = 0; i < argc; i++) {
		if (!pvcreate_single(cmd, argv[i], &pp))
			ret = ECMD_FAILED;

		if (sigint_caught())
			return ret;
	}

	return ret;
}
