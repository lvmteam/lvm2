/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
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

struct pvchange_params {
	unsigned done;
	unsigned total;
};

static int _pvchange_single(struct cmd_context *cmd, struct volume_group *vg,
			    struct physical_volume *pv, struct processing_handle *handle)
{
	struct pvchange_params *params = (struct pvchange_params *) handle->custom_handle;
	const char *pv_name = pv_dev_name(pv);
	char uuid[64] __attribute__((aligned(8)));
	unsigned done = 0;

	int allocatable = arg_int_value(cmd, allocatable_ARG, 0);
	int mda_ignore = arg_int_value(cmd, metadataignore_ARG, 0);
	int tagargs = arg_count(cmd, addtag_ARG) + arg_count(cmd, deltag_ARG);

	params->total++;

	/* If in a VG, must change using volume group. */
	if (!is_orphan(pv)) {
		if (tagargs && !(vg->fid->fmt->features & FMT_TAGS)) {
			log_error("Volume group containing %s does not "
				  "support tags", pv_name);
			goto bad;
		}
		if (arg_count(cmd, uuid_ARG) && lvs_in_vg_activated(vg)) {
			log_error("Volume group containing %s has active "
				  "logical volumes", pv_name);
			goto bad;
		}
		if (!archive(vg))
			goto_bad;
	} else if (tagargs) {
		log_error("Can't change tag on Physical Volume %s not "
			  "in volume group", pv_name);
		goto bad;
	}

	if (arg_count(cmd, allocatable_ARG)) {
		if (is_orphan(pv) &&
		    !(pv->fmt->features & FMT_ORPHAN_ALLOCATABLE)) {
			log_error("Allocatability not supported by orphan "
				  "%s format PV %s", pv->fmt->name, pv_name);
			goto bad;
		}

		/* change allocatability for a PV */
		if (allocatable && (pv_status(pv) & ALLOCATABLE_PV)) {
			log_warn("Physical volume \"%s\" is already "
				 "allocatable.", pv_name);
		} else if (!allocatable && !(pv_status(pv) & ALLOCATABLE_PV)) {
			log_warn("Physical volume \"%s\" is already "
				 "unallocatable.", pv_name);
		} else if (allocatable) {
			log_verbose("Setting physical volume \"%s\" "
				    "allocatable", pv_name);
			pv->status |= ALLOCATABLE_PV;
			done = 1;
		} else {
			log_verbose("Setting physical volume \"%s\" NOT "
				    "allocatable", pv_name);
			pv->status &= ~ALLOCATABLE_PV;
			done = 1;
		}
	}

	if (tagargs) {
		/* tag or deltag */
		if (arg_count(cmd, addtag_ARG) && !change_tag(cmd, NULL, NULL, pv, addtag_ARG))
			goto_bad;

		if (arg_count(cmd, deltag_ARG) && !change_tag(cmd, NULL, NULL, pv, deltag_ARG))
			goto_bad;
	
		done = 1;
	}

	if (arg_count(cmd, metadataignore_ARG)) {
		if ((vg_mda_copies(vg) != VGMETADATACOPIES_UNMANAGED) &&
		    (arg_count(cmd, force_ARG) == PROMPT) &&
		    yes_no_prompt("Override preferred number of copies "
				  "of VG %s metadata? [y/n]: ",
				  pv_vg_name(pv)) == 'n')
			goto_bad;
		if (!pv_change_metadataignore(pv, mda_ignore))
			goto_bad;

		done = 1;
	} 

	if (arg_count(cmd, uuid_ARG)) {
		/* --uuid: Change PV ID randomly */
		memcpy(&pv->old_id, &pv->id, sizeof(pv->id));
		if (!id_create(&pv->id)) {
			log_error("Failed to generate new random UUID for %s.",
				  pv_name);
			goto bad;
		}
		if (!id_write_format(&pv->id, uuid, sizeof(uuid)))
			goto_bad;
		log_verbose("Changing uuid of %s to %s.", pv_name, uuid);
		if (!is_orphan(pv) && (!pv_write(cmd, pv, 1))) {
			log_error("pv_write with new uuid failed "
				  "for %s.", pv_name);
			goto bad;
		}

		done = 1;
	}

	if (!done) {
		log_print_unless_silent("Physical volume %s not changed", pv_name);
		return ECMD_PROCESSED;
	}

	log_verbose("Updating physical volume \"%s\"", pv_name);
	if (!is_orphan(pv)) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Failed to store physical volume \"%s\" in "
				  "volume group \"%s\"", pv_name, vg->name);
			goto bad;
		}
		backup(vg);
	} else if (!(pv_write(cmd, pv, 0))) {
		log_error("Failed to store physical volume \"%s\"",
			  pv_name);
		goto bad;
	}

	log_print_unless_silent("Physical volume \"%s\" changed", pv_name);

	params->done++;
	return ECMD_PROCESSED;

bad:
	log_error("Physical volume %s not changed", pv_name);

	return ECMD_FAILED;
}

int pvchange(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvchange_params params = { 0 };
	struct processing_handle *handle = NULL;
	int ret;

	if (!(arg_count(cmd, allocatable_ARG) + arg_is_set(cmd, addtag_ARG) +
	    arg_is_set(cmd, deltag_ARG) + arg_count(cmd, uuid_ARG) +
	    arg_count(cmd, metadataignore_ARG))) {
		log_error("Please give one or more of -x, -uuid, "
			  "--addtag, --deltag or --metadataignore");
		ret = EINVALID_CMD_LINE;
		goto out;
	}

	if (!(handle = init_processing_handle(cmd))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
	}

	handle->custom_handle = &params;

	if (!(arg_count(cmd, all_ARG)) && !argc && !handle->internal_report_for_select) {
		log_error("Please give a physical volume path or use --select for selection.");
		ret = EINVALID_CMD_LINE;
		goto out;
	}

	if (arg_count(cmd, all_ARG) && argc) {
		log_error("Option --all and PhysicalVolumePath are exclusive.");
		ret = EINVALID_CMD_LINE;
		goto out;
	}

	if (!argc) {
		/*
		 * Take the global lock here so the lvmcache remains
		 * consistent across orphan/non-orphan vg locks.  If we don't
		 * take the lock here, pvs with 0 mdas in a non-orphan VG will
		 * be processed twice.
		 */
		if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
			log_error("Unable to obtain global lock.");
			ret = ECMD_FAILED;
			goto out;
		}
	}

	ret = process_each_pv(cmd, argc, argv, NULL, READ_FOR_UPDATE, handle, _pvchange_single);

	if (!argc)
		unlock_vg(cmd, VG_GLOBAL);

	log_print_unless_silent("%d physical volume%s changed / %d physical volume%s not changed",
				params.done, params.done == 1 ? "" : "s",
				params.total - params.done, (params.total - params.done) == 1 ? "" : "s");

out:
	destroy_processing_handle(cmd, handle);
	return ret;
}
