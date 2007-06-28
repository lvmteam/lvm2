/*
 * Copyright (C) 2005 Red Hat, Inc. All rights reserved.
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
#include "lv_alloc.h"

struct lvconvert_params {
	int snapshot;
	int zero;

	const char *origin;
	const char *lv_name;
	const char *vg_name;

	uint32_t chunk_size;
	uint32_t region_size;

	uint32_t mirrors;
	sign_t mirrors_sign;

	struct segment_type *segtype;

	alloc_policy_t alloc;

	int pv_count;
	char **pvs;
	struct list *pvh;
};

static int _lvconvert_name_params(struct lvconvert_params *lp,
				  struct cmd_context *cmd,
				  int *pargc, char ***pargv)
{
	char *ptr;
	const char *vg_name = NULL;

	if (lp->snapshot) {
		if (!*pargc) {
			log_error("Please specify a logical volume to act as "
				  "the snapshot origin.");
			return 0;
		}

		lp->origin = *pargv[0];
		(*pargv)++, (*pargc)--;
		if (!(lp->vg_name = extract_vgname(cmd, lp->origin))) {
			log_error("The origin name should include the "
				  "volume group.");
			return 0;
		}

		/* Strip the volume group from the origin */
		if ((ptr = strrchr(lp->origin, (int) '/')))
			lp->origin = ptr + 1;
	}

	if (!*pargc) {
		log_error("Please provide logical volume path");
		return 0;
	}

	lp->lv_name = (*pargv)[0];
	(*pargv)++, (*pargc)--;

	if (strchr(lp->lv_name, '/') &&
	    (vg_name = extract_vgname(cmd, lp->lv_name)) &&
	    lp->vg_name && strcmp(vg_name, lp->vg_name)) {
		log_error("Please use a single volume group name "
			  "(\"%s\" or \"%s\")", vg_name, lp->vg_name);
		return 0;
	}

	if (!lp->vg_name)
		lp->vg_name = vg_name;

	if (!validate_name(lp->vg_name)) {
		log_error("Please provide a valid volume group name");
		return 0;
	}

	if ((ptr = strrchr(lp->lv_name, '/')))
		lp->lv_name = ptr + 1;

	if (!apply_lvname_restrictions(lp->lv_name))
		return_0;

	return 1;
}

static int _read_params(struct lvconvert_params *lp, struct cmd_context *cmd,
			int argc, char **argv)
{
	int region_size;
	int pagesize = lvm_getpagesize();

	memset(lp, 0, sizeof(*lp));

	if (arg_count(cmd, mirrors_ARG) + arg_count(cmd, snapshot_ARG) != 1) {
		log_error("Exactly one of --mirrors or --snapshot arguments "
			  "required.");
		return 0;
	}

	if (arg_count(cmd, snapshot_ARG))
		lp->snapshot = 1;

	if (arg_count(cmd, mirrors_ARG)) {
		lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0);
		lp->mirrors_sign = arg_sign_value(cmd, mirrors_ARG, 0);
	}

	lp->alloc = ALLOC_INHERIT;
	if (arg_count(cmd, alloc_ARG))
		lp->alloc = arg_uint_value(cmd, alloc_ARG, lp->alloc);

	if (lp->snapshot) {
		if (arg_count(cmd, regionsize_ARG)) {
			log_error("--regionsize is only available with mirrors");
			return 0;
		}

		if (arg_sign_value(cmd, chunksize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative chunk size is invalid");
			return 0;
		}
		lp->chunk_size = 2 * arg_uint_value(cmd, chunksize_ARG, 8);
		if (lp->chunk_size < 8 || lp->chunk_size > 1024 ||
		    (lp->chunk_size & (lp->chunk_size - 1))) {
			log_error("Chunk size must be a power of 2 in the "
				  "range 4K to 512K");
			return 0;
		}
		log_verbose("Setting chunksize to %d sectors.", lp->chunk_size);

		if (!(lp->segtype = get_segtype_from_string(cmd, "snapshot")))
			return_0;

		lp->zero = strcmp(arg_str_value(cmd, zero_ARG,
						(lp->segtype->flags &
						 SEG_CANNOT_BE_ZEROED) ?
						"n" : "y"), "n");

	} else {	/* Mirrors */
		if (arg_count(cmd, chunksize_ARG)) {
			log_error("--chunksize is only available with "
				  "snapshots");
			return 0;
		}

		if (arg_count(cmd, zero_ARG)) {
			log_error("--zero is only available with snapshots");
			return 0;
		}

		/*
	 	 * --regionsize is only valid if converting an LV into a mirror.
	 	 * Checked when we know the state of the LV being converted.
	 	 */

		if (arg_count(cmd, regionsize_ARG)) {
			if (arg_sign_value(cmd, regionsize_ARG, 0) ==
				    SIGN_MINUS) {
				log_error("Negative regionsize is invalid");
				return 0;
			}
			lp->region_size = 2 * arg_uint_value(cmd,
							     regionsize_ARG, 0);
		} else {
			region_size = 2 * find_config_tree_int(cmd,
						"activation/mirror_region_size",
						DEFAULT_MIRROR_REGION_SIZE);
			if (region_size < 0) {
				log_error("Negative regionsize in "
					  "configuration file is invalid");
				return 0;
			}
			lp->region_size = region_size;
		}

		if (lp->region_size % (pagesize >> SECTOR_SHIFT)) {
			log_error("Region size (%" PRIu32 ") must be "
				  "a multiple of machine memory "
				  "page size (%d)",
				  lp->region_size, pagesize >> SECTOR_SHIFT);
			return 0;
		}

		if (lp->region_size & (lp->region_size - 1)) {
			log_error("Region size (%" PRIu32
				  ") must be a power of 2", lp->region_size);
			return 0;
		}

		if (!lp->region_size) {
			log_error("Non-zero region size must be supplied.");
			return 0;
		}

		if (!(lp->segtype = get_segtype_from_string(cmd, "striped")))
			return_0;
	}

	if (activation() && lp->segtype->ops->target_present &&
	    !lp->segtype->ops->target_present(NULL)) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel", lp->segtype->name);
		return 0;
	}

	if (!_lvconvert_name_params(lp, cmd, &argc, &argv))
		return_0;

	lp->pv_count = argc;
	lp->pvs = argv;

	return 1;
}

static int lvconvert_mirrors(struct cmd_context * cmd, struct logical_volume * lv,
			     struct lvconvert_params *lp)
{
	struct lv_segment *seg;
	uint32_t existing_mirrors;
	struct alloc_handle *ah = NULL;
	struct logical_volume *log_lv;
	struct list *parallel_areas;
	struct segment_type *segtype;  /* FIXME: could I just use lp->segtype */
	float sync_percent;

	seg = first_seg(lv);
	existing_mirrors = seg->area_count;

	/* Adjust required number of mirrors */
	if (lp->mirrors_sign == SIGN_PLUS)
		lp->mirrors = existing_mirrors + lp->mirrors;
	else if (lp->mirrors_sign == SIGN_MINUS) {
		if (lp->mirrors >= existing_mirrors) {
			log_error("Logical volume %s only has %" PRIu32 " mirrors.",
				  lv->name, existing_mirrors);
			return 0;
		}
		lp->mirrors = existing_mirrors - lp->mirrors;
	} else
		lp->mirrors += 1;

	if (arg_count(cmd, regionsize_ARG) && (lv->status & MIRRORED) &&
	    (lp->region_size != seg->region_size)) {
		log_error("Mirror log region size cannot be changed on "
			  "an existing mirror.");
		return 0;
	}

	if ((lp->mirrors == 1)) {
		if (!(lv->status & MIRRORED)) {
			log_error("Logical volume %s is already not mirrored.",
				  lv->name);
			return 1;
		}

		if (!remove_mirror_images(seg, 1,
					  lp->pv_count ? lp->pvh : NULL, 1))
			return_0;
	} else {		/* mirrors > 1 */
		if ((lv->status & MIRRORED)) {
			if (list_size(&lv->segments) != 1) {
				log_error("Logical volume %s has multiple "
					  "mirror segments.", lv->name);
				return 0;
			}
			if (lp->mirrors == existing_mirrors) {
				if (!seg->log_lv && !arg_count(cmd, corelog_ARG)) {
					/* No disk log present, add one. */
					if (!(parallel_areas = build_parallel_areas_from_lv(cmd, lv)))
						return_0;
					if (!lv_mirror_percent(cmd, lv, 0, &sync_percent, NULL)) {
						log_error("Unable to determine mirror sync status.");
						return 0;
					}

					segtype = get_segtype_from_string(cmd, "striped");

					if (!(ah = allocate_extents(lv->vg, NULL, segtype, 0,
								    0, 1, 0,
								    NULL, 0, 0, lp->pvh,
								    lp->alloc,
								    parallel_areas))) {
						stack;
						return 0;
					}

					if (sync_percent >= 100.0)
						init_mirror_in_sync(1);
					else
						init_mirror_in_sync(0);

					if (!(log_lv = create_mirror_log(cmd, lv->vg, ah,
									 lp->alloc, lv->name,
									 (sync_percent >= 100.0) ?
									 1 : 0, &lv->tags))) {
						log_error("Failed to create mirror log.");
						return 0;
					}
					seg->log_lv = log_lv;
					log_lv->status |= MIRROR_LOG;
					first_seg(log_lv)->mirror_seg = seg;
				} else if (seg->log_lv && arg_count(cmd, corelog_ARG)) {
					/* Had disk log, switch to core. */
					if (!lv_mirror_percent(cmd, lv, 0, &sync_percent, NULL)) {
						log_error("Unable to determine mirror sync status.");
						return 0;
					}

					if (sync_percent >= 100.0)
						init_mirror_in_sync(1);
					else
						init_mirror_in_sync(0);

					if (!remove_mirror_images(seg, lp->mirrors,
								  lp->pv_count ?
								  lp->pvh : NULL, 1))
						return_0;
				} else {
					/* No change */
					log_error("Logical volume %s already has %"
						  PRIu32 " mirror(s).", lv->name,
						  lp->mirrors - 1);
					return 1;
				}
			} else if (lp->mirrors > existing_mirrors) {
				/* FIXME Unless anywhere, remove PV of log_lv 
				 * from allocatable_pvs & allocate 
				 * (mirrors - existing_mirrors) new areas
				 */
				/* FIXME Create mirror hierarchy to sync */
				log_error("Adding mirror images is not "
					  "supported yet.");
				return 0;
			} else {
				/* Reduce number of mirrors */
				if (!remove_mirror_images(seg, lp->mirrors,
							  lp->pv_count ?
							  lp->pvh : NULL, 0))
					return_0;
			}
		} else {
			/* Make existing LV into mirror set */
			/* FIXME Share code with lvcreate */

			/* FIXME Why is this restriction here?  Fix it! */
			list_iterate_items(seg, &lv->segments) {
				if (seg_is_striped(seg) && seg->area_count > 1) {
					log_error("Mirrors of striped volumes are not yet supported.");
					return 0;
				}
			}

			if (!(parallel_areas = build_parallel_areas_from_lv(cmd, lv)))
				return_0;

			if (!(ah = allocate_extents(lv->vg, NULL, lp->segtype,
						    1, lp->mirrors - 1,
						    arg_count(cmd, corelog_ARG) ? 0 : 1,
						    lv->le_count * (lp->mirrors - 1),
						    NULL, 0, 0, lp->pvh,
						    lp->alloc,
						    parallel_areas)))
				return_0;

			lp->region_size = adjusted_mirror_region_size(lv->vg->extent_size,
								      lv->le_count,
								      lp->region_size);

			log_lv = NULL;
			if (!arg_count(cmd, corelog_ARG) &&
			    !(log_lv = create_mirror_log(cmd, lv->vg, ah,
							 lp->alloc,
							 lv->name, 0, &lv->tags))) {
				log_error("Failed to create mirror log.");
				return 0;
			}

			if (!create_mirror_layers(ah, 1, lp->mirrors, lv,
						  lp->segtype, 0,
						  lp->region_size,
						  log_lv))
				return_0;
		}
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!vg_write(lv->vg))
		return_0;

	backup(lv->vg);

	if (!suspend_lv(cmd, lv)) {
		log_error("Failed to lock %s", lv->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg)) {
		resume_lv(cmd, lv);
		return 0;
	}

	log_very_verbose("Updating \"%s\" in kernel", lv->name);

	if (!resume_lv(cmd, lv)) {
		log_error("Problem reactivating %s", lv->name);
		return 0;
	}

	log_print("Logical volume %s converted.", lv->name);

	return 1;
}

static int lvconvert_snapshot(struct cmd_context *cmd,
			      struct logical_volume *lv,
			      struct lvconvert_params *lp)
{
	struct logical_volume *org;

	if (!(org = find_lv(lv->vg, lp->origin))) {
		log_error("Couldn't find origin volume '%s'.", lp->origin);
		return 0;
	}

	if (org->status & (LOCKED|PVMOVE) || lv_is_cow(org)) {
		log_error("Unable to create a snapshot of a %s LV.",
			  org->status & LOCKED ? "locked" :
			  org->status & PVMOVE ? "pvmove" : "snapshot");
		return 0;
	}

	if (!lp->zero || !(lv->status & LVM_WRITE))
		log_warn("WARNING: \"%s\" not zeroed", lv->name);
	else if (!set_lv(cmd, lv, 0, 0)) {
			log_error("Aborting. Failed to wipe snapshot "
				  "exception store.");
			return 0;
	}

	if (!deactivate_lv(cmd, lv)) {
		log_error("Couldn't deactivate LV %s.", lv->name);
		return 0;
	}

	if (!vg_add_snapshot(lv->vg->fid, NULL, org, lv, NULL, org->le_count,
			     lp->chunk_size)) {
		log_error("Couldn't create snapshot.");
		return 0;
	}

	/* store vg on disk(s) */
	if (!vg_write(lv->vg))
		return_0;

	backup(lv->vg);

	if (!suspend_lv(cmd, org)) {
		log_error("Failed to suspend origin %s", org->name);
		vg_revert(lv->vg);
		return 0;
	}

	if (!vg_commit(lv->vg))
		return_0;

	if (!resume_lv(cmd, org)) {
		log_error("Problem reactivating origin %s", org->name);
		return 0;
	}

	log_print("Logical volume %s converted to snapshot.", lv->name);

	return 1;
}

static int lvconvert_single(struct cmd_context *cmd, struct logical_volume *lv,
			    void *handle)
{
	struct lvconvert_params *lp = handle;

	if (lv->status & LOCKED) {
		log_error("Cannot convert locked LV %s", lv->name);
		return ECMD_FAILED;
	}

	if (lv_is_origin(lv)) {
		log_error("Can't convert logical volume \"%s\" under snapshot",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv_is_cow(lv)) {
		log_error("Can't convert snapshot logical volume \"%s\"",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & PVMOVE) {
		log_error("Unable to convert pvmove LV %s", lv->name);
		return ECMD_FAILED;
	}

	if (arg_count(cmd, mirrors_ARG)) {
		if (!archive(lv->vg))
			return ECMD_FAILED;
		if (!lvconvert_mirrors(cmd, lv, lp))
			return ECMD_FAILED;
	} else if (lp->snapshot) {
		if (!archive(lv->vg))
			return ECMD_FAILED;
		if (!lvconvert_snapshot(cmd, lv, lp))
			return ECMD_FAILED;
	}

	return ECMD_PROCESSED;
}

int lvconvert(struct cmd_context * cmd, int argc, char **argv)
{
	int consistent = 1;
	struct volume_group *vg;
	struct lv_list *lvl;
	struct lvconvert_params lp;
	int ret = ECMD_FAILED;

	if (!_read_params(&lp, cmd, argc, argv)) {
		stack;
		return EINVALID_CMD_LINE;
	}

	log_verbose("Checking for existing volume group \"%s\"", lp.vg_name);

	if (!lock_vol(cmd, lp.vg_name, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", lp.vg_name);
		return ECMD_FAILED;
	}

	if (!(vg = vg_read(cmd, lp.vg_name, NULL, &consistent))) {
		log_error("Volume group \"%s\" doesn't exist", lp.vg_name);
		goto error;
	}

	if (!vg_check_status(vg, CLUSTERED | EXPORTED_VG | LVM_WRITE))
		goto error;

	if (!(lvl = find_lv_in_vg(vg, lp.lv_name))) {
		log_error("Logical volume \"%s\" not found in "
			  "volume group \"%s\"", lp.lv_name, lp.vg_name);
		goto error;
	}

	if (lp.pv_count) {
		if (!(lp.pvh = create_pv_list(cmd->mem, vg, lp.pv_count,
					      lp.pvs, 1))) {
			stack;
			goto error;
		}
	} else
		lp.pvh = &vg->pvs;

	ret = lvconvert_single(cmd, lvl->lv, &lp);

error:
	unlock_vg(cmd, lp.vg_name);
	return ret;
}
