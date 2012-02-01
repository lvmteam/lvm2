/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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
#include "lv_alloc.h"

#include <fcntl.h>

struct lvcreate_cmdline_params {
	percent_type_t percent;
	uint64_t size;
	char **pvs;
	int pv_count;
};

static int _set_vg_name(struct lvcreate_params *lp, const char *vg_name)
{
	/* Can't do anything */
	if (!vg_name)
		return 1;

	/* If VG name already known, ensure this 2nd copy is identical */
	if (lp->vg_name && strcmp(lp->vg_name, vg_name)) {
		log_error("Inconsistent volume group names "
			  "given: \"%s\" and \"%s\"",
			  lp->vg_name, vg_name);
		return 0;
	}
	lp->vg_name = vg_name;

	return 1;
}

static int _lvcreate_name_params(struct lvcreate_params *lp,
				 struct cmd_context *cmd,
				 int *pargc, char ***pargv)
{
	int argc = *pargc;
	char **argv = *pargv, *ptr;
	const char *vg_name;

	lp->pool = arg_str_value(cmd, thinpool_ARG, NULL);

	/* If --thinpool contains VG name, extract it. */
	if (lp->pool && strchr(lp->pool, '/')) {
		if (!(lp->vg_name = extract_vgname(cmd, lp->pool)))
			return 0;
		/* Strip VG from pool */
		if ((ptr = strrchr(lp->pool, (int) '/')))
			lp->pool = ptr + 1;
	}

	lp->lv_name = arg_str_value(cmd, name_ARG, NULL);

	/* If --name contains VG name, extract it. */
	if (lp->lv_name && strchr(lp->lv_name, '/')) {
		if (!_set_vg_name(lp, extract_vgname(cmd, lp->lv_name)))
			return_0;

		/* Strip VG from lv_name */
		if ((ptr = strrchr(lp->lv_name, (int) '/')))
			lp->lv_name = ptr + 1;
	}

	/* Need an origin? */
	if (lp->snapshot && !arg_count(cmd, virtualsize_ARG)) {
		/* argv[0] might be origin or vg/origin */
		if (!argc) {
			log_error("Please specify a logical volume to act as "
				  "the snapshot origin.");
			return 0;
		}

		lp->origin = skip_dev_dir(cmd, argv[0], NULL);
		if (strrchr(lp->origin, '/')) {
			if (!_set_vg_name(lp, extract_vgname(cmd, lp->origin)))
				return_0;

			/* Strip the volume group from the origin */
			if ((ptr = strrchr(lp->origin, (int) '/')))
				lp->origin = ptr + 1;
		}

		if (!lp->vg_name)
			_set_vg_name(lp, extract_vgname(cmd, NULL));

		if (!lp->vg_name) {
			log_error("The origin name should include the "
				  "volume group.");
			return 0;
		}

		(*pargv)++, (*pargc)--;
	} else if (seg_is_thin(lp) && !lp->pool && argc) {
		/* argv[0] might be vg or vg/Pool */

		vg_name = skip_dev_dir(cmd, argv[0], NULL);
		if (!strrchr(vg_name, '/')) {
			if (!_set_vg_name(lp, vg_name))
				return_0;
		} else {
			lp->pool = vg_name;
			if (!_set_vg_name(lp, extract_vgname(cmd, lp->pool)))
				return_0;

			if (!lp->vg_name)
				_set_vg_name(lp, extract_vgname(cmd, NULL));

			if (!lp->vg_name) {
				log_error("The pool name should include the "
					  "volume group.");
				return 0;
			}

			/* Strip the volume group */
			if ((ptr = strrchr(lp->pool, (int) '/')))
				lp->pool = ptr + 1;
		}

		(*pargv)++, (*pargc)--;
	} else {
		/*
		 * If VG not on command line, try environment default.
		 */
		if (!argc) {
			if (!lp->vg_name && !(lp->vg_name = extract_vgname(cmd, NULL))) {
				log_error("Please provide a volume group name");
				return 0;
			}
		} else {
			vg_name = skip_dev_dir(cmd, argv[0], NULL);
			if (strrchr(vg_name, '/')) {
				log_error("Volume group name expected "
					  "(no slash)");
				return 0;
			}

			if (!_set_vg_name(lp, vg_name))
				return_0;

			(*pargv)++, (*pargc)--;
		}
	}

	if (!validate_name(lp->vg_name)) {
		log_error("Volume group name %s has invalid characters",
			  lp->vg_name);
		return 0;
	}

	if (lp->lv_name) {
		if (!apply_lvname_restrictions(lp->lv_name))
			return_0;

		if (!validate_name(lp->lv_name)) {
			log_error("Logical volume name \"%s\" is invalid",
				  lp->lv_name);
			return 0;
		}
	}

	if (lp->pool) {
		if (!apply_lvname_restrictions(lp->pool))
			return_0;

		if (!validate_name(lp->pool)) {
			log_error("Logical volume name \"%s\" is invalid",
				  lp->pool);
			return 0;
		}

		if (lp->lv_name && !strcmp(lp->lv_name, lp->pool)) {
			log_error("Logical volume name %s and pool name %s must be different.", 
				  lp->lv_name, lp->pool);
			return 0;
		}
	}

	return 1;
}

/*
 * Normal snapshot or thinly-provisioned snapshot?
 */
static int _determine_snapshot_type(struct volume_group *vg,
				  struct lvcreate_params *lp)
{
	struct lv_list *lvl;

	if (!(lvl = find_lv_in_vg(vg, lp->origin))) {
		log_error("Snapshot origin LV %s not found in Volume group %s.",
			  lp->origin, vg->name);
		return 0;
	}

	if (!arg_count(vg->cmd, extents_ARG) && !arg_count(vg->cmd, size_ARG)) {
		if (!lv_is_thin_volume(lvl->lv)) {
			log_error("Please specify either size or extents with snapshots.");
			return 0;
		}

		lp->thin = 1;
		if (!(lp->segtype = get_segtype_from_string(vg->cmd, "thin")))
			return_0;

		lp->pool = first_seg(lvl->lv)->pool_lv->name;
	}

	return 1;
}

/*
 * Update extents parameters based on other parameters which affect the size
 * calculation.
 * NOTE: We must do this here because of the percent_t typedef and because we
 * need the vg.
 */
static int _update_extents_params(struct volume_group *vg,
				  struct lvcreate_params *lp,
				  struct lvcreate_cmdline_params *lcp)
{
	uint32_t pv_extent_count;
	struct logical_volume *origin = NULL;

	if (lcp->size &&
	    !(lp->extents = extents_from_size(vg->cmd, lcp->size,
					       vg->extent_size)))
		return_0;

	if (lp->voriginsize &&
	    !(lp->voriginextents = extents_from_size(vg->cmd, lp->voriginsize,
						      vg->extent_size)))
		return_0;

	/*
	 * Create the pv list before we parse lcp->percent - might be
	 * PERCENT_PVSs
	 */
	if (lcp->pv_count) {
		if (!(lp->pvh = create_pv_list(vg->cmd->mem, vg,
					   lcp->pv_count, lcp->pvs, 1)))
			return_0;
	} else
		lp->pvh = &vg->pvs;

	switch(lcp->percent) {
		case PERCENT_VG:
			lp->extents = percent_of_extents(lp->extents, vg->extent_count, 0);
			break;
		case PERCENT_FREE:
			lp->extents = percent_of_extents(lp->extents, vg->free_count, 0);
			break;
		case PERCENT_PVS:
			if (!lcp->pv_count)
				lp->extents = percent_of_extents(lp->extents, vg->extent_count, 0);
			else {
				pv_extent_count = pv_list_extents_free(lp->pvh);
				lp->extents = percent_of_extents(lp->extents, pv_extent_count, 0);
			}
			break;
		case PERCENT_LV:
			log_error("Please express size as %%VG, %%PVS, or "
				  "%%FREE.");
			return 0;
		case PERCENT_ORIGIN:
			if (lp->snapshot && lp->origin &&
			    !(origin = find_lv(vg, lp->origin))) {
				log_error("Couldn't find origin volume '%s'.",
					  lp->origin);
				return 0;
			}
			if (!origin) {
				log_error(INTERNAL_ERROR "Couldn't find origin volume.");
				return 0;
			}
			lp->extents = percent_of_extents(lp->extents, origin->le_count, 0);
			break;
		case PERCENT_NONE:
			break;
	}

	if (lp->create_thin_pool && !lp->poolmetadatasize)
		/* Defaults to nr_pool_blocks * 64b */
		lp->poolmetadatasize =  (uint64_t) lp->extents * vg->extent_size /
			(uint64_t) lp->chunk_size * UINT64_C(64);

	if (lp->poolmetadatasize &&
	    !(lp->poolmetadataextents = extents_from_size(vg->cmd, lp->poolmetadatasize,
							  vg->extent_size)))
		return_0;

	return 1;
}

static int _read_size_params(struct lvcreate_params *lp,
			     struct lvcreate_cmdline_params *lcp,
			     struct cmd_context *cmd)
{
	if (arg_count(cmd, extents_ARG) && arg_count(cmd, size_ARG)) {
		log_error("Please specify either size or extents (not both)");
		return 0;
	}

	if (!lp->thin && !lp->snapshot && !arg_count(cmd, extents_ARG) && !arg_count(cmd, size_ARG)) {
		log_error("Please specify either size or extents");
		return 0;
	}

	if (arg_count(cmd, extents_ARG)) {
		if (arg_sign_value(cmd, extents_ARG, 0) == SIGN_MINUS) {
			log_error("Negative number of extents is invalid");
			return 0;
		}
		lp->extents = arg_uint_value(cmd, extents_ARG, 0);
		lcp->percent = arg_percent_value(cmd, extents_ARG, PERCENT_NONE);
	}

	/* Size returned in kilobyte units; held in sectors */
	if (arg_count(cmd, size_ARG)) {
		if (arg_sign_value(cmd, size_ARG, 0) == SIGN_MINUS) {
			log_error("Negative size is invalid");
			return 0;
		}
		lcp->size = arg_uint64_value(cmd, size_ARG, UINT64_C(0));
		lcp->percent = PERCENT_NONE;
	}

	/* If size/extents given with thin, then we are creating a thin pool */
	if (lp->thin && (arg_count(cmd, size_ARG) || arg_count(cmd, extents_ARG)))
		lp->create_thin_pool = 1;

	if (arg_count(cmd, poolmetadatasize_ARG)) {
		if (!seg_is_thin(lp)) {
			log_error("--poolmetadatasize may only be specified when allocating the thin pool.");
			return 0;
		}
		if (arg_sign_value(cmd, poolmetadatasize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative poolmetadatasize is invalid.");
			return 0;
		}
		lp->poolmetadatasize = arg_uint64_value(cmd, poolmetadatasize_ARG, UINT64_C(0));
	}

	/* Size returned in kilobyte units; held in sectors */
	if (arg_count(cmd, virtualsize_ARG)) {
		if (seg_is_thin_pool(lp)) {
			log_error("Virtual size in incompatible with thin_pool segment type.");
			return 0;
		}
		if (arg_sign_value(cmd, virtualsize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative virtual origin size is invalid");
			return 0;
		}
		lp->voriginsize = arg_uint64_value(cmd, virtualsize_ARG,
						   UINT64_C(0));
		if (!lp->voriginsize) {
			log_error("Virtual origin size may not be zero");
			return 0;
		}
	} else {
		/* No virtual size given, so no thin LV to create. */
		if (seg_is_thin_volume(lp) && !(lp->segtype = get_segtype_from_string(cmd, "thin-pool")))
			return_0;

		lp->thin = 0;
	}

	return 1;
}

/*
 * Generic mirror parameter checks.
 * FIXME: Should eventually be moved into lvm library.
 */
static int _validate_mirror_params(const struct cmd_context *cmd __attribute__((unused)),
				   const struct lvcreate_params *lp)
{
	int pagesize = lvm_getpagesize();

	if (lp->region_size & (lp->region_size - 1)) {
		log_error("Region size (%" PRIu32 ") must be a power of 2",
			  lp->region_size);
		return 0;
	}

	if (lp->region_size % (pagesize >> SECTOR_SHIFT)) {
		log_error("Region size (%" PRIu32 ") must be a multiple of "
			  "machine memory page size (%d)",
			  lp->region_size, pagesize >> SECTOR_SHIFT);
		return 0;
	}

	if (!lp->region_size) {
		log_error("Non-zero region size must be supplied.");
		return 0;
	}

	return 1;
}

static int _read_mirror_params(struct lvcreate_params *lp,
			       struct cmd_context *cmd)
{
	int region_size;
	const char *mirrorlog;
	int corelog = arg_count(cmd, corelog_ARG);

	mirrorlog = arg_str_value(cmd, mirrorlog_ARG,
				  corelog ? "core" : DEFAULT_MIRRORLOG);

	if (strcmp("core", mirrorlog) && corelog) {
		log_error("Please use only one of --mirrorlog or --corelog");
		return 0;
	}

	if (!strcmp("mirrored", mirrorlog)) {
		lp->log_count = 2;
	} else if (!strcmp("disk", mirrorlog)) {
		lp->log_count = 1;
	} else if (!strcmp("core", mirrorlog))
		lp->log_count = 0;
	else {
		log_error("Unknown mirrorlog type: %s", mirrorlog);
		return 0;
	}

	log_verbose("Setting logging type to %s", mirrorlog);

	lp->nosync = arg_is_set(cmd, nosync_ARG);

	if (arg_count(cmd, regionsize_ARG)) {
		if (arg_sign_value(cmd, regionsize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative regionsize is invalid");
			return 0;
		}
		lp->region_size = arg_uint_value(cmd, regionsize_ARG, 0);
	} else {
		region_size = 2 * find_config_tree_int(cmd,
					"activation/mirror_region_size",
					DEFAULT_MIRROR_REGION_SIZE);
		if (region_size < 0) {
			log_error("Negative regionsize in configuration file "
				  "is invalid");
			return 0;
		}
		lp->region_size = region_size;
	}

	if (!_validate_mirror_params(cmd, lp))
		return 0;

	return 1;
}

static int _read_raid_params(struct lvcreate_params *lp,
			     struct cmd_context *cmd)
{
	if (!segtype_is_raid(lp->segtype))
		return 1;

	if (arg_count(cmd, corelog_ARG) ||
	    arg_count(cmd, mirrorlog_ARG)) {
		log_error("Log options not applicable to %s segtype",
			  lp->segtype->name);
		return 0;
	}

	/*
	 * get_stripe_params is called before _read_raid_params
	 * and already sets:
	 *   lp->stripes
	 *   lp->stripe_size
	 *
	 * For RAID 4/5/6, these values must be set.
	 */
	if (!segtype_is_mirrored(lp->segtype) && (lp->stripes < 2)) {
		log_error("Number of stripes to %s not specified",
			  lp->segtype->name);
		return 0;
	}

	/*
	 * _read_mirror_params is called before _read_raid_params
	 * and already sets:
	 *   lp->nosync
	 *   lp->region_size
	 *
	 * But let's ensure that programmers don't reorder
	 * that by checking and warning if they aren't set.
	 */
	if (!lp->region_size) {
		log_error(INTERNAL_ERROR "region_size not set.");
		return 0;
	}

	return 1;
}

static int _read_thin_params(struct lvcreate_params *lp,
			     struct cmd_context *cmd)
{
	if (!seg_is_thin(lp)) {
		if (lp->poolmetadatasize) {
			log_error("Pool metadata size option is only for pool creation.");
			return 0;
		}
		return 1;
	}

	if (lp->create_thin_pool) {
		if (lp->poolmetadatasize > (2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE)) {
			log_warn("WARNING: Maximum supported pool metadata size is 16GB.");
			lp->poolmetadatasize = 2 * DEFAULT_THIN_POOL_MAX_METADATA_SIZE;
		} else if (lp->poolmetadatasize < (2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE))
			lp->poolmetadatasize = 2 * DEFAULT_THIN_POOL_MIN_METADATA_SIZE;
		log_verbose("Setting pool metadata size to %" PRIu64 " sectors.",
			    lp->poolmetadatasize);
	} else if (lp->poolmetadatasize) {
		log_error("Pool metadata size options is only for pool creation.");
		return 0;
	}

	return 1;
}

static int _read_activation_params(struct lvcreate_params *lp, struct cmd_context *cmd)
{
	unsigned pagesize;

	lp->activate = arg_uint_value(cmd, available_ARG, CHANGE_AY);

	if (lp->activate == CHANGE_AN || lp->activate == CHANGE_ALN) {
		if (lp->zero && !seg_is_thin(lp)) {
			log_error("--available n requires --zero n");
			return 0;
		}
	}

	/*
	 * Read ahead.
	 */
	lp->read_ahead = arg_uint_value(cmd, readahead_ARG,
					cmd->default_settings.read_ahead);
	pagesize = lvm_getpagesize() >> SECTOR_SHIFT;
	if (lp->read_ahead != DM_READ_AHEAD_AUTO &&
	    lp->read_ahead != DM_READ_AHEAD_NONE &&
	    lp->read_ahead % pagesize) {
		if (lp->read_ahead < pagesize)
			lp->read_ahead = pagesize;
		else
			lp->read_ahead = (lp->read_ahead / pagesize) * pagesize;
		log_warn("WARNING: Overriding readahead to %u sectors, a multiple "
			    "of %uK page size.", lp->read_ahead, pagesize >> 1);
	}

	/*
	 * Permissions.
	 */
	lp->permission = arg_uint_value(cmd, permission_ARG,
					LVM_READ | LVM_WRITE);

	if (lp->thin && !(lp->permission & LVM_WRITE)) {
		log_error("Read-only thin volumes are not currently supported.");
		return 0;
	}

	/* Must not zero read only volume */
	if (!(lp->permission & LVM_WRITE))
		lp->zero = 0;

	lp->minor = arg_int_value(cmd, minor_ARG, -1);
	lp->major = arg_int_value(cmd, major_ARG, -1);

	/* Persistent minor */
	if (arg_count(cmd, persistent_ARG)) {
		if (lp->create_thin_pool && !lp->thin) {
			log_error("--persistent is not permitted when creating a thin pool device.");
			return 0;
		}
		if (!strcmp(arg_str_value(cmd, persistent_ARG, "n"), "y")) {
			if (lp->minor == -1) {
				log_error("Please specify minor number with "
					  "--minor when using -My");
				return 0;
			}
			if (lp->major == -1) {
				log_error("Please specify major number with "
					  "--major when using -My");
				return 0;
			}
		} else {
			if ((lp->minor != -1) || (lp->major != -1)) {
				log_error("--major and --minor incompatible "
					  "with -Mn");
				return 0;
			}
		}
	} else if (arg_count(cmd, minor_ARG) || arg_count(cmd, major_ARG)) {
		log_error("--major and --minor require -My");
		return 0;
	}

	return 1;
}

static int _lvcreate_params(struct lvcreate_params *lp,
			    struct lvcreate_cmdline_params *lcp,
			    struct cmd_context *cmd,
			    int argc, char **argv)
{
	int contiguous;
	struct arg_value_group_list *current_group;
	const char *segtype_str;
	const char *tag;

	memset(lp, 0, sizeof(*lp));
	memset(lcp, 0, sizeof(*lcp));
	dm_list_init(&lp->tags);

	/*
	 * Check selected options are compatible and determine segtype
	 */
// FIXME -m0 implies *striped*
	if (arg_count(cmd, thin_ARG) && arg_count(cmd,mirrors_ARG)) {
		log_error("--thin and --mirrors are incompatible.");
		return 0;
	}

// FIXME -m0 implies *striped*

	/* Set default segtype */
	if (arg_count(cmd, mirrors_ARG))
		segtype_str = find_config_tree_str(cmd, "global/mirror_segtype_default", DEFAULT_MIRROR_SEGTYPE);
	else if (arg_count(cmd, thin_ARG) || arg_count(cmd, thinpool_ARG))
		segtype_str = "thin";
	else
		segtype_str = "striped";

	lp->segtype = get_segtype_from_string(cmd, arg_str_value(cmd, type_ARG, segtype_str));

	if (seg_unknown(lp)) {
		log_error("Unable to create LV with unknown segment type %s.", arg_str_value(cmd, type_ARG, segtype_str));
		return 0;
	}

	if (arg_count(cmd, snapshot_ARG) || seg_is_snapshot(lp) ||
	    (!seg_is_thin(lp) && arg_count(cmd, virtualsize_ARG)))
		lp->snapshot = 1;

	if (seg_is_thin_pool(lp)) {
		if (lp->snapshot) {
			log_error("Snapshots are incompatible with thin_pool segment_type.");
			return 0;
		}
		lp->create_thin_pool = 1;
	}

	if (seg_is_thin_volume(lp))
		lp->thin = 1;

	lp->mirrors = 1;

	/* Default to 2 mirrored areas if '--type mirror|raid1' */
	if (segtype_is_mirrored(lp->segtype))
		lp->mirrors = 2;

	if (arg_count(cmd, mirrors_ARG)) {
		lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0) + 1;
		if (lp->mirrors == 1) {
			if (segtype_is_mirrored(lp->segtype)) {
				log_error("--mirrors must be at least 1 with segment type %s.", lp->segtype->name);
				return 0;
			}
			log_print("Redundant mirrors argument: default is 0");
		}
		if (arg_sign_value(cmd, mirrors_ARG, 0) == SIGN_MINUS) {
			log_error("Mirrors argument may not be negative");
			return 0;
		}
	}

	if (lp->snapshot && arg_count(cmd, zero_ARG)) {
		log_error("-Z is incompatible with snapshots");
		return 0;
	}

	if (segtype_is_mirrored(lp->segtype) || segtype_is_raid(lp->segtype)) {
		if (lp->snapshot) {
			log_error("mirrors and snapshots are currently "
				  "incompatible");
			return 0;
		}
	} else {
		if (arg_count(cmd, corelog_ARG)) {
			log_error("--corelog is only available with mirrors");
			return 0;
		}

		if (arg_count(cmd, mirrorlog_ARG)) {
			log_error("--mirrorlog is only available with mirrors");
			return 0;
		}

		if (arg_count(cmd, nosync_ARG)) {
			log_error("--nosync is only available with mirrors");
			return 0;
		}
	}

	if (activation() && lp->segtype->ops->target_present &&
	    !lp->segtype->ops->target_present(cmd, NULL, NULL)) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel", lp->segtype->name);
		return 0;
	}

	if (!get_activation_monitoring_mode(cmd, NULL,
					    &lp->activation_monitoring))
		return_0;

	if (!_lvcreate_name_params(lp, cmd, &argc, &argv) ||
	    !_read_size_params(lp, lcp, cmd) ||
	    !get_stripe_params(cmd, &lp->stripes, &lp->stripe_size) ||
	    !_read_mirror_params(lp, cmd) ||
	    !_read_raid_params(lp, cmd) ||
	    !_read_thin_params(lp, cmd))
		return_0;

	if (lp->snapshot && lp->thin && arg_count(cmd, chunksize_ARG))
		log_warn("WARNING: Ignoring --chunksize with thin snapshots.");
	else if (lp->thin && !lp->create_thin_pool) {
		if (arg_count(cmd, chunksize_ARG))
			log_warn("WARNING: Ignoring --chunksize when using an existing pool.");
	} else if (lp->snapshot || lp->create_thin_pool) {
		if (arg_sign_value(cmd, chunksize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative chunk size is invalid");
			return 0;
		}
		if (lp->snapshot) {
			lp->chunk_size = arg_uint_value(cmd, chunksize_ARG, 8);
			if (lp->chunk_size < 8 || lp->chunk_size > 1024 ||
			    (lp->chunk_size & (lp->chunk_size - 1))) {
				log_error("Chunk size must be a power of 2 in the "
					  "range 4K to 512K");
				return 0;
			}
		} else {
			lp->chunk_size = arg_uint_value(cmd, chunksize_ARG, DM_THIN_MIN_DATA_BLOCK_SIZE);
			if ((lp->chunk_size < DM_THIN_MIN_DATA_BLOCK_SIZE) ||
			    (lp->chunk_size > DM_THIN_MAX_DATA_BLOCK_SIZE) ||
			    (lp->chunk_size & (lp->chunk_size - 1))) {
				log_error("Chunk size must be a power of 2 in the "
					  "range %uK to %uK", (DM_THIN_MIN_DATA_BLOCK_SIZE / 2),
					  (DM_THIN_MIN_DATA_BLOCK_SIZE / 2));
				return 0;
			}
		}
		log_verbose("Setting chunksize to %u sectors.", lp->chunk_size);

		if (!lp->thin && lp->snapshot && !(lp->segtype = get_segtype_from_string(cmd, "snapshot")))
			return_0;
	} else {
		if (arg_count(cmd, chunksize_ARG)) {
			log_error("-c is only available with snapshots and thin pools");
			return 0;
		}
	}

	/*
	 * Should we zero the lv.
	 */
	lp->zero = strcmp(arg_str_value(cmd, zero_ARG,
		(lp->segtype->flags & SEG_CANNOT_BE_ZEROED) ? "n" : "y"), "n");

	if (lp->mirrors > DEFAULT_MIRROR_MAX_IMAGES) {
		log_error("Only up to %d images in mirror supported currently.",
			  DEFAULT_MIRROR_MAX_IMAGES);
		return 0;
	}

	/*
	 * Allocation parameters
	 */
	contiguous = strcmp(arg_str_value(cmd, contiguous_ARG, "n"), "n");

	lp->alloc = contiguous ? ALLOC_CONTIGUOUS : ALLOC_INHERIT;

	lp->alloc = arg_uint_value(cmd, alloc_ARG, lp->alloc);

	if (contiguous && (lp->alloc != ALLOC_CONTIGUOUS)) {
		log_error("Conflicting contiguous and alloc arguments");
		return 0;
	}

	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(current_group->arg_values, addtag_ARG))
			continue;

		if (!(tag = grouped_arg_str_value(current_group->arg_values, addtag_ARG, NULL))) {
			log_error("Failed to get tag");
			return 0;
		}

		if (!str_list_add(cmd->mem, &lp->tags, tag)) {
			log_error("Unable to allocate memory for tag %s", tag);
			return 0;
		}
	}

	lcp->pv_count = argc;
	lcp->pvs = argv;

	return 1;
}

static int _check_thin_parameters(struct volume_group *vg, struct lvcreate_params *lp,
				  struct lvcreate_cmdline_params *lcp)
{
	struct lv_list *lvl;

	if (!lp->thin && !lp->create_thin_pool) {
		log_error("Please specify device size(s).");
		return 0;
	}

	if (lp->thin && !lp->create_thin_pool) {
		if (arg_count(vg->cmd, chunksize_ARG)) {
			log_error("Only specify --chunksize when originally creating the thin pool.");
			return 0;
		}

		if (lcp->pv_count) {
			log_error("Only specify Physical volumes when allocating the thin pool.");
			return 0;
		}

		if (arg_count(vg->cmd, alloc_ARG)) {
			log_error("--alloc may only be specified when allocating the thin pool.");
			return 0;
		}

		if (arg_count(vg->cmd, poolmetadatasize_ARG)) {
			log_error("--poolmetadatasize may only be specified when allocating the thin pool.");
			return 0;
		}

		if (arg_count(vg->cmd, stripesize_ARG)) {
			log_error("--stripesize may only be specified when allocating the thin pool.");
			return 0;
		}

		if (arg_count(vg->cmd, stripes_ARG)) {
			log_error("--stripes may only be specified when allocating the thin pool.");
			return 0;
		}

		if (arg_count(vg->cmd, contiguous_ARG)) {
			log_error("--contiguous may only be specified when allocating the thin pool.");
			return 0;
		}

		if (arg_count(vg->cmd, zero_ARG)) {
			log_error("--zero may only be specified when allocating the thin pool.");
			return 0;
		}
	}

	if (lp->create_thin_pool && lp->pool) {
		if (find_lv_in_vg(vg, lp->pool)) {
			log_error("Pool %s already exists in Volume group %s.", lp->pool, vg->name);
			return 0;
		}
	} else if (lp->pool) {
		if (!(lvl = find_lv_in_vg(vg, lp->pool))) {
			log_error("Pool %s not found in Volume group %s.", lp->pool, vg->name);
			return 0;
		}
		if (!lv_is_thin_pool(lvl->lv)) {
			log_error("Logical volume %s is not a thin pool.", lp->pool);
			return 0;
		}
	} else if (!lp->create_thin_pool) {
		log_error("Please specify name of existing pool.");
		return 0;
	}

	if (!lp->thin && lp->lv_name) {
		log_error("--name may only be given when creating a new thin Logical volume or snapshot.");
		return 0;
	}

	if (!lp->thin) {
		if (arg_count(vg->cmd, readahead_ARG)) {
			log_error("--readhead may only be given when creating a new thin Logical volume or snapshot.");
			return 0;
		}
		if (arg_count(vg->cmd, permission_ARG)) {
			log_error("--permission may only be given when creating a new thin Logical volume or snapshot.");
			return 0;
		}
		if (arg_count(vg->cmd, persistent_ARG)) {
			log_error("--persistent may only be given when creating a new thin Logical volume or snapshot.");
			return 0;
		}
	}

	return 1;
}

/*
 * Ensure the set of thin parameters extracted from the command line is consistent.
 */
static int _validate_internal_thin_processing(const struct lvcreate_params *lp)
{
	int r = 1;

	/*
	   The final state should be one of:
	   thin  create_thin_pool  snapshot   origin   pool  
	     1          1             0          0      y/n    - create new pool and a thin LV in it
	     1          0             0          0      y      - create new thin LV in existing pool
	     0          1             0          0      y/n    - create new pool only
	     1          0             1          1      y      - create thin snapshot of existing thin LV
	*/

	if (!lp->create_thin_pool && !lp->pool) {
		log_error(INTERNAL_ERROR "--thinpool not identified.");
		r = 0;
	}

	if ((lp->snapshot && !lp->origin) || (!lp->snapshot && lp->origin)) {
		log_error(INTERNAL_ERROR "Inconsistent snapshot and origin parameters identified.");
		r = 0;
	}

	if (lp->snapshot && (lp->create_thin_pool || !lp->thin)) {
		log_error(INTERNAL_ERROR "Inconsistent thin and snapshot parameters identified.");
		r = 0;
	}

	if (!lp->thin && !lp->create_thin_pool) {
		log_error(INTERNAL_ERROR "Failed to identify what type of thin target to use.");
		r = 0;
	}

	if (seg_is_thin_pool(lp) && lp->thin) {
		log_error(INTERNAL_ERROR "Thin volume cannot be created with thin pool segment type.");
		r = 0;
	}

	return r;
}

int lvcreate(struct cmd_context *cmd, int argc, char **argv)
{
	int r = ECMD_PROCESSED;
	struct lvcreate_params lp;
	struct lvcreate_cmdline_params lcp;
	struct volume_group *vg;

	if (!_lvcreate_params(&lp, &lcp, cmd, argc, argv))
		return EINVALID_CMD_LINE;

	log_verbose("Finding volume group \"%s\"", lp.vg_name);
	vg = vg_read_for_update(cmd, lp.vg_name, NULL, 0);
	if (vg_read_error(vg)) {
		release_vg(vg);
		stack;
		return ECMD_FAILED;
	}

	if (lp.snapshot && lp.origin && !_determine_snapshot_type(vg, &lp)) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (seg_is_thin(&lp) && !_check_thin_parameters(vg, &lp, &lcp)) {
		r = ECMD_FAILED;
		goto_out;
	}

	/*
	 * Check activation parameters to support inactive thin snapshot creation
	 * FIXME: anything else needs to be moved past _determine_snapshot_type()?
	 */
	if (!_read_activation_params(&lp, cmd)) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (!_update_extents_params(vg, &lp, &lcp)) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (seg_is_thin(&lp) && !_validate_internal_thin_processing(&lp)) {
		r = ECMD_FAILED;
		goto_out;
	}

	if (lp.create_thin_pool)
		log_verbose("Making thin pool %s in VG %s using segtype %s",
			    lp.pool ? : "with generated name", lp.vg_name, lp.segtype->name);

	if (lp.thin)
		log_verbose("Making thin LV %s in pool %s in VG %s%s%s using segtype %s",
			    lp.lv_name ? : "with generated name",
			    lp.pool ? : "with generated name", lp.vg_name,
			    lp.snapshot ? " as snapshot of " : "",
			    lp.snapshot ? lp.origin : "", lp.segtype->name);

	if (!lv_create_single(vg, &lp)) {
		stack;
		r = ECMD_FAILED;
	}
out:
	unlock_and_release_vg(cmd, vg, lp.vg_name);
	return r;
}
