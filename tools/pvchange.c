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

static int _pvchange_single(struct cmd_context *cmd, struct volume_group *vg,
			    struct physical_volume *pv,
			    void *handle __attribute__((unused)))
{
	const char *pv_name = pv_dev_name(pv);
	char uuid[64] __attribute__((aligned(8)));

	int allocatable = 0;
	int tagargs = 0;
	int mda_ignore = 0;

	tagargs = arg_count(cmd, addtag_ARG) + arg_count(cmd, deltag_ARG);

	if (arg_count(cmd, allocatable_ARG))
		allocatable = !strcmp(arg_str_value(cmd, allocatable_ARG, "n"),
				      "y");
	if (arg_count(cmd, metadataignore_ARG))
		mda_ignore = !strcmp(arg_str_value(cmd, metadataignore_ARG, "n"),
				      "y");

	/* If in a VG, must change using volume group. */
	if (!is_orphan(pv)) {
		if (tagargs && !(vg->fid->fmt->features & FMT_TAGS)) {
			log_error("Volume group containing %s does not "
				  "support tags", pv_name);
			return 0;
		}
		if (arg_count(cmd, uuid_ARG) && lvs_in_vg_activated(vg)) {
			log_error("Volume group containing %s has active "
				  "logical volumes", pv_name);
			return 0;
		}
		if (!archive(vg))
			return 0;
	} else {
		if (tagargs) {
			log_error("Can't change tag on Physical Volume %s not "
				  "in volume group", pv_name);
			return 0;
		}
	}

	if (arg_count(cmd, allocatable_ARG)) {
		if (is_orphan(pv) &&
		    !(pv->fmt->features & FMT_ORPHAN_ALLOCATABLE)) {
			log_error("Allocatability not supported by orphan "
				  "%s format PV %s", pv->fmt->name, pv_name);
			return 0;
		}

		/* change allocatability for a PV */
		if (allocatable && (pv_status(pv) & ALLOCATABLE_PV)) {
			log_warn("Physical volume \"%s\" is already "
				 "allocatable.", pv_name);
			return 1;
		}

		if (!allocatable && !(pv_status(pv) & ALLOCATABLE_PV)) {
			log_warn("Physical volume \"%s\" is already "
				 "unallocatable.", pv_name);
			return 1;
		}

		if (allocatable) {
			log_verbose("Setting physical volume \"%s\" "
				    "allocatable", pv_name);
			pv->status |= ALLOCATABLE_PV;
		} else {
			log_verbose("Setting physical volume \"%s\" NOT "
				    "allocatable", pv_name);
			pv->status &= ~ALLOCATABLE_PV;
		}
	}

	if (tagargs) {
		/* tag or deltag */
		if (arg_count(cmd, addtag_ARG) && !change_tag(cmd, NULL, NULL, pv, addtag_ARG))
			return_0;

		if (arg_count(cmd, deltag_ARG) && !change_tag(cmd, NULL, NULL, pv, deltag_ARG))
			return_0;
 
	}

	if (arg_count(cmd, metadataignore_ARG)) {
		if ((vg_mda_copies(vg) != VGMETADATACOPIES_UNMANAGED) &&
		    (arg_count(cmd, force_ARG) == PROMPT) &&
		    yes_no_prompt("Override preferred number of copies "
				  "of VG %s metadata? [y/n]: ",
				  pv_vg_name(pv)) == 'n') {
			log_error("Physical volume %s not changed", pv_name);
			return 0;
		}
		if (!pv_change_metadataignore(pv, mda_ignore))
			return_0;
	} 

	if (arg_count(cmd, uuid_ARG)) {
		/* --uuid: Change PV ID randomly */
		memcpy(&pv->old_id, &pv->id, sizeof(pv->id));
		if (!id_create(&pv->id)) {
			log_error("Failed to generate new random UUID for %s.",
				  pv_name);
			return 0;
		}
		if (!id_write_format(&pv->id, uuid, sizeof(uuid)))
			return 0;
		log_verbose("Changing uuid of %s to %s.", pv_name, uuid);
		if (!is_orphan(pv) && (!pv_write(cmd, pv, 1))) {
			log_error("pv_write with new uuid failed "
				  "for %s.", pv_name);
			return 0;
		}
	}

	log_verbose("Updating physical volume \"%s\"", pv_name);
	if (!is_orphan(pv)) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Failed to store physical volume \"%s\" in "
				  "volume group \"%s\"", pv_name, vg->name);
			return 0;
		}
		backup(vg);
	} else if (!(pv_write(cmd, pv, 0))) {
		log_error("Failed to store physical volume \"%s\"",
			  pv_name);
		return 0;
	}

	log_print_unless_silent("Physical volume \"%s\" changed", pv_name);

	return 1;
}

int pvchange(struct cmd_context *cmd, int argc, char **argv)
{
	int opt = 0;
	int done = 0;
	int total = 0;

	struct volume_group *vg;
	const char *vg_name;
	char *pv_name;

	struct pv_list *pvl;
	struct dm_list *vgnames;
	struct str_list *sll;

	if (!(arg_count(cmd, allocatable_ARG) + arg_is_set(cmd, addtag_ARG) +
	    arg_is_set(cmd, deltag_ARG) + arg_count(cmd, uuid_ARG) +
	    arg_count(cmd, metadataignore_ARG))) {
		log_error("Please give one or more of -x, -uuid, "
			  "--addtag, --deltag or --metadataignore");
		return EINVALID_CMD_LINE;
	}

	if (!(arg_count(cmd, all_ARG)) && !argc) {
		log_error("Please give a physical volume path");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, all_ARG) && argc) {
		log_error("Option a and PhysicalVolumePath are exclusive");
		return EINVALID_CMD_LINE;
	}

	if (argc) {
		log_verbose("Using physical volume(s) on command line");
		for (; opt < argc; opt++) {
			pv_name = argv[opt];
			dm_unescape_colons_and_at_signs(pv_name, NULL, NULL);
			vg_name = find_vgname_from_pvname(cmd, pv_name);
			if (!vg_name) {
				log_error("Failed to read physical volume %s",
					  pv_name);
				continue;
			}
			vg = vg_read_for_update(cmd, vg_name, NULL, 0);
			if (vg_read_error(vg)) {
				release_vg(vg);
				stack;
				continue;
			}
			pvl = find_pv_in_vg(vg, pv_name);
			if (!pvl || !pvl->pv) {
				log_error("Unable to find %s in %s",
					  pv_name, vg_name);
				continue;
			}

			total++;
			done += _pvchange_single(cmd, vg,
						 pvl->pv, NULL);
			unlock_and_release_vg(cmd, vg, vg_name);
		}
	} else {
		log_verbose("Scanning for physical volume names");
		/* FIXME: share code with toollib */
		/*
		 * Take the global lock here so the lvmcache remains
		 * consistent across orphan/non-orphan vg locks.  If we don't
		 * take the lock here, pvs with 0 mdas in a non-orphan VG will
		 * be processed twice.
		 */
		if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
			log_error("Unable to obtain global lock.");
			return ECMD_FAILED;
		}

		if ((vgnames = get_vgnames(cmd, 1)) &&
		    !dm_list_empty(vgnames)) {
			dm_list_iterate_items(sll, vgnames) {
				vg = vg_read_for_update(cmd, sll->str, NULL, 0);
				if (vg_read_error(vg)) {
					release_vg(vg);
					stack;
					continue;
				}
				dm_list_iterate_items(pvl, &vg->pvs) {
					total++;
					done += _pvchange_single(cmd, vg,
								 pvl->pv,
								 NULL);
				}
				unlock_and_release_vg(cmd, vg, sll->str);
			}
		}
		unlock_vg(cmd, VG_GLOBAL);
	}

	log_print_unless_silent("%d physical volume%s changed / %d physical volume%s "
				"not changed",
				done, done == 1 ? "" : "s",
				total - done, (total - done) == 1 ? "" : "s");

	return (total == done) ? ECMD_PROCESSED : ECMD_FAILED;
}
