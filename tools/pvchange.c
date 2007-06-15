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

/* FIXME Locking.  PVs in VG. */

static int _pvchange_single(struct cmd_context *cmd, struct physical_volume *pv,
			    void *handle __attribute((unused)))
{
	struct volume_group *vg = NULL;
	struct pv_list *pvl;
	struct list mdas;
	uint64_t sector;
	uint32_t orig_pe_alloc_count;

	const char *pv_name = dev_name(pv_dev(pv));
	const char *tag = NULL;
	const char *orig_vg_name;
	char uuid[64] __attribute((aligned(8)));

	int consistent = 1;
	int allocatable = 0;
	int tagarg = 0;

	list_init(&mdas);

	if (arg_count(cmd, addtag_ARG))
		tagarg = addtag_ARG;
	else if (arg_count(cmd, deltag_ARG))
		tagarg = deltag_ARG;

	if (arg_count(cmd, allocatable_ARG))
		allocatable = !strcmp(arg_str_value(cmd, allocatable_ARG, "n"),
				      "y");
	else if (tagarg && !(tag = arg_str_value(cmd, tagarg, NULL))) {
		log_error("Failed to get tag");
		return 0;
	}

	/* If in a VG, must change using volume group. */
	if (*pv_vg_name(pv)) {
		log_verbose("Finding volume group of physical volume \"%s\"",
			    pv_name);

		if (!lock_vol(cmd, pv_vg_name(pv), LCK_VG_WRITE)) {
			log_error("Can't get lock for %s", pv_vg_name(pv));
			return 0;
		}

		if (!(vg = vg_read(cmd, pv_vg_name(pv), NULL, &consistent))) {
			unlock_vg(cmd, pv_vg_name(pv));
			log_error("Unable to find volume group of \"%s\"",
				  pv_name);
			return 0;
		}

		if (!vg_check_status(vg,
				     CLUSTERED | EXPORTED_VG | LVM_WRITE)) {
			unlock_vg(cmd, pv_vg_name(pv));
			return 0;
		}

		if (!(pvl = find_pv_in_vg(vg, pv_name))) {
			unlock_vg(cmd, pv_vg_name(pv));
			log_error
			    ("Unable to find \"%s\" in volume group \"%s\"",
			     pv_name, vg->name);
			return 0;
		}
		if (tagarg && !(vg->fid->fmt->features & FMT_TAGS)) {
			unlock_vg(cmd, pv_vg_name(pv));
			log_error("Volume group containing %s does not "
				  "support tags", pv_name);
			return 0;
		}
		if (arg_count(cmd, uuid_ARG) && lvs_in_vg_activated(vg)) {
			unlock_vg(cmd, pv_vg_name(pv));
			log_error("Volume group containing %s has active "
				  "logical volumes", pv_name);
			return 0;
		}
		pv = pvl->pv;
		if (!archive(vg))
			return 0;
	} else {
		if (tagarg) {
			log_error("Can't change tag on Physical Volume %s not "
				  "in volume group", pv_name);
			return 0;
		}
		if (!lock_vol(cmd, ORPHAN, LCK_VG_WRITE)) {
			log_error("Can't get lock for orphans");
			return 0;
		}

		if (!(pv = pv_read(cmd, pv_name, &mdas, &sector, 1))) {
			unlock_vg(cmd, ORPHAN);
			log_error("Unable to read PV \"%s\"", pv_name);
			return 0;
		}

	}

	if (arg_count(cmd, allocatable_ARG)) {
		if (!*pv_vg_name(pv) &&
		    !(pv->fmt->features & FMT_ORPHAN_ALLOCATABLE)) {
			log_error("Allocatability not supported by orphan "
				  "%s format PV %s", pv->fmt->name, pv_name);
			unlock_vg(cmd, ORPHAN);
			return 0;
		}

		/* change allocatability for a PV */
		if (allocatable && (pv_status(pv) & ALLOCATABLE_PV)) {
			log_error("Physical volume \"%s\" is already "
				  "allocatable", pv_name);
			if (*pv_vg_name(pv))
				unlock_vg(cmd, pv_vg_name(pv));
			else
				unlock_vg(cmd, ORPHAN);
			return 1;
		}

		if (!allocatable && !(pv_status(pv) & ALLOCATABLE_PV)) {
			log_error("Physical volume \"%s\" is already "
				  "unallocatable", pv_name);
			if (*pv_vg_name(pv))
				unlock_vg(cmd, pv_vg_name(pv));
			else
				unlock_vg(cmd, ORPHAN);
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
	} else if (tagarg) {
		/* tag or deltag */
		if ((tagarg == addtag_ARG)) {
			if (!str_list_add(cmd->mem, &pv->tags, tag)) {
				log_error("Failed to add tag %s to physical "
					  "volume %s", tag, pv_name);
				return 0;
			}
		} else {
			if (!str_list_del(&pv->tags, tag)) {
				log_error("Failed to remove tag %s from "
					  "physical volume" "%s", tag, pv_name);
				return 0;
			}
		}
	} else {
		/* --uuid: Change PV ID randomly */
		if (!id_create(&pv->id)) {
			log_error("Failed to generate new random UUID for %s.",
				  pv_name);
			return 0;
		}
		if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
			stack;
			return 0;
		}
		log_verbose("Changing uuid of %s to %s.", pv_name, uuid);
		if (*pv_vg_name(pv)) {
			orig_vg_name = pv_vg_name(pv);
			orig_pe_alloc_count = pv_pe_alloc_count(pv);
			pv->vg_name = ORPHAN;
			pv->pe_alloc_count = 0;
			if (!(pv_write(cmd, pv, NULL, INT64_C(-1)))) {
				log_error("pv_write with new uuid failed "
					  "for %s.", pv_name);
				return 0;
			}
			pv->vg_name = orig_vg_name;
			pv->pe_alloc_count = orig_pe_alloc_count;
		}
	}

	log_verbose("Updating physical volume \"%s\"", pv_name);
	if (*pv_vg_name(pv)) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			unlock_vg(cmd, pv_vg_name(pv));
			log_error("Failed to store physical volume \"%s\" in "
				  "volume group \"%s\"", pv_name, vg->name);
			return 0;
		}
		backup(vg);
		unlock_vg(cmd, pv_vg_name(pv));
	} else {
		if (!(pv_write(cmd, pv, NULL, INT64_C(-1)))) {
			unlock_vg(cmd, ORPHAN);
			log_error("Failed to store physical volume \"%s\"",
				  pv_name);
			return 0;
		}
		unlock_vg(cmd, ORPHAN);
	}

	log_print("Physical volume \"%s\" changed", pv_name);

	return 1;
}

int pvchange(struct cmd_context *cmd, int argc, char **argv)
{
	int opt = 0;
	int done = 0;
	int total = 0;

	struct physical_volume *pv;
	char *pv_name;

	struct pv_list *pvl;
	struct list *pvslist;
	struct list mdas;

	list_init(&mdas);

	if (arg_count(cmd, allocatable_ARG) + arg_count(cmd, addtag_ARG) +
	    arg_count(cmd, deltag_ARG) + arg_count(cmd, uuid_ARG) != 1) {
		log_error("Please give exactly one option of -x, -uuid, "
			  "--addtag or --deltag");
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
			/* FIXME Read VG instead - pv_read will fail */
			if (!(pv = pv_read(cmd, pv_name, &mdas, NULL, 1))) {
				log_error("Failed to read physical volume %s",
					  pv_name);
				continue;
			}
			total++;
			done += _pvchange_single(cmd, pv, NULL);
		}
	} else {
		log_verbose("Scanning for physical volume names");
		if (!(pvslist = get_pvs(cmd))) {
			return ECMD_FAILED;
		}

		list_iterate_items(pvl, pvslist) {
			total++;
			done += _pvchange_single(cmd, pvl->pv, NULL);
		}
	}

	log_print("%d physical volume%s changed / %d physical volume%s "
		  "not changed",
		  done, done == 1 ? "" : "s",
		  total - done, (total - done) == 1 ? "" : "s");

	return (total == done) ? ECMD_PROCESSED : ECMD_FAILED;
}
