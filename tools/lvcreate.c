/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
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

#include <fcntl.h>

struct lvcreate_cmdline_params {
	percent_type_t percent;
	uint64_t size;
	char **pvs;
	uint32_t pv_count;
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
	char **argv = *pargv;
	const char *vg_name;

	lp->lv_name = arg_str_value(cmd, name_ARG, NULL);
	if (!validate_restricted_lvname_param(cmd, &lp->vg_name, &lp->lv_name))
		return_0;

	lp->pool_name = arg_str_value(cmd, thinpool_ARG, NULL)
		? : arg_str_value(cmd, cachepool_ARG, NULL);
	if (!validate_lvname_param(cmd, &lp->vg_name, &lp->pool_name))
		return_0;

	if (seg_is_cache(lp)) {
		/*
		 * 2 ways of cache usage for lvcreate -H -l1 vg/lv
		 *
		 * vg/lv is existing cache pool:
		 *       cached LV is created using this cache pool
		 * vg/lv is not cache pool so it is cache origin
		 *       origin is cached with created cache pool
		 *
		 * We are looking for the vgname or cache pool or cache origin LV.
		 *
		 * lv name is stored in origin_name and pool_name and
		 * later with opened VG it's decided what should happen.
		 */
		if (!argc) {
			if (!lp->pool_name) {
				log_error("Please specify a logical volume to act as the cache pool or origin.");
				return 0;
			}
		} else {
			vg_name = skip_dev_dir(cmd, argv[0], NULL);
			if (!strchr(vg_name, '/')) {
				/* Lucky part - only vgname is here */
				if (!_set_vg_name(lp, vg_name))
					return_0;
			} else {
				/* Assume it's cache origin for now */
				lp->origin_name = vg_name;
				if (!validate_lvname_param(cmd, &lp->vg_name, &lp->origin_name))
					return_0;

				if (lp->pool_name) {
					if (strcmp(lp->pool_name, lp->origin_name)) {
						log_error("Unsupported syntax, cannot use cache origin %s and --cachepool %s.",
							  lp->origin_name, lp->pool_name);
						return 0;
					}
					lp->origin_name = NULL;
				} else {
					/*
					 * Gambling here, could be cache pool or cache origin,
					 * detection is possible after openning vg,
					 * yet we need to parse pool args
					 */
					lp->pool_name = lp->origin_name;
					lp->create_pool = 1;
				}
			}
			(*pargv)++, (*pargc)--;
		}

		if (!lp->vg_name &&
		    !_set_vg_name(lp, extract_vgname(cmd, NULL)))
			return_0;

		if (!lp->vg_name) {
			log_error("The cache pool or cache origin name should "
				  "include the volume group.");
			return 0;
		}

		if (!lp->pool_name) {
			log_error("Creation of cached volume and cache pool "
				  "in one command is not yet supported.");
			return 0;
		}

		lp->cache = 1;
	} else if (lp->snapshot && !arg_count(cmd, virtualsize_ARG)) {
		/* argv[0] might be [vg/]origin */
		if (!argc) {
			log_error("Please specify a logical volume to act as "
				  "the snapshot origin.");
			return 0;
		}

		lp->origin_name = argv[0];
		if (!validate_lvname_param(cmd, &lp->vg_name, &lp->origin_name))
			return_0;

		if (!lp->vg_name &&
		    !_set_vg_name(lp, extract_vgname(cmd, NULL)))
			return_0;

		if (!lp->vg_name) {
			log_error("The origin name should include the "
				  "volume group.");
			return 0;
		}

		(*pargv)++, (*pargc)--;
	} else if ((seg_is_thin(lp) || seg_is_pool(lp)) && argc) {
		/* argv[0] might be [/dev.../]vg or [/dev../]vg/pool */

		vg_name = skip_dev_dir(cmd, argv[0], NULL);
		if (!strchr(vg_name, '/')) {
			if (!_set_vg_name(lp, vg_name))
				return_0;
		} else {
			if (!validate_lvname_param(cmd, &lp->vg_name, &vg_name))
				return_0;

			if (lp->pool_name &&
			    (strcmp(vg_name, lp->pool_name) != 0)) {
				log_error("Ambiguous %s name specified, %s and %s.",
					  lp->segtype->name, vg_name, lp->pool_name);
				return 0;
			}
			lp->pool_name = vg_name;

			if (!lp->vg_name &&
			    !_set_vg_name(lp, extract_vgname(cmd, NULL)))
				return_0;

			if (!lp->vg_name) {
				log_error("The %s name should include the "
					  "volume group.", lp->segtype->name);
				return 0;
			}
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
			if (strchr(vg_name, '/')) {
				log_error("Volume group name expected "
					  "(no slash)");
				return 0;
			}

			if (!_set_vg_name(lp, vg_name))
				return_0;

			(*pargv)++, (*pargc)--;
		}
	}

	/* support --name & --type {thin|cache}-pool */
	if (seg_is_pool(lp) && lp->lv_name) {
		if (lp->pool_name && (strcmp(lp->lv_name, lp->pool_name) != 0)) {
			log_error("Ambiguous %s name specified, %s and %s.",
				  lp->segtype->name, lp->lv_name, lp->pool_name);
			return 0;
		}
		lp->pool_name = lp->lv_name;
		lp->lv_name = NULL;
	}

	if (lp->pool_name && lp->lv_name && !strcmp(lp->pool_name, lp->lv_name)) {
		log_error("Logical volume name %s and pool name must be different.",
			  lp->lv_name);
		return 0;
	}

	if (!validate_name(lp->vg_name)) {
		log_error("Volume group name %s has invalid characters",
			  lp->vg_name);
		return 0;
	}

	return 1;
}

/*
 * Update extents parameters based on other parameters which affect the size
 * calculation.
 * NOTE: We must do this here because of the dm_percent_t typedef and because we
 * need the vg.
 */
static int _update_extents_params(struct volume_group *vg,
				  struct lvcreate_params *lp,
				  struct lvcreate_cmdline_params *lcp)
{
	uint32_t pv_extent_count;
	struct logical_volume *origin_lv = NULL;
	uint32_t size_rest;
	uint32_t stripesize_extents;
	uint32_t extents;

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

	switch (lcp->percent) {
		case PERCENT_VG:
			extents = percent_of_extents(lp->extents, vg->extent_count, 0);
			break;
		case PERCENT_FREE:
			extents = percent_of_extents(lp->extents, vg->free_count, 0);
			break;
		case PERCENT_PVS:
			if (lcp->pv_count) {
				pv_extent_count = pv_list_extents_free(lp->pvh);
				extents = percent_of_extents(lp->extents, pv_extent_count, 0);
			} else
				extents = percent_of_extents(lp->extents, vg->extent_count, 0);
			break;
		case PERCENT_LV:
			log_error("Please express size as %%FREE%s, %%PVS or %%VG.",
				  (lp->snapshot) ? ", %ORIGIN" : "");
			return 0;
		case PERCENT_ORIGIN:
			if (lp->snapshot && lp->origin_name &&
			    !(origin_lv = find_lv(vg, lp->origin_name))) {
				log_error("Couldn't find origin volume '%s'.",
					  lp->origin_name);
				return 0;
			}
			if (!origin_lv) {
				log_error(INTERNAL_ERROR "Couldn't find origin volume.");
				return 0;
			}
			/* Add whole metadata size estimation */
			extents = cow_max_extents(origin_lv, lp->chunk_size) - origin_lv->le_count +
				percent_of_extents(lp->extents, origin_lv->le_count, 1);
			break;
		case PERCENT_NONE:
			extents = lp->extents;
			break;
		default:
			log_error(INTERNAL_ERROR "Unsupported percent type %u.", lcp->percent);
			return 0;
	}

	if (lcp->percent) {
		/* FIXME Don't do the adjustment for parallel allocation with PERCENT_ORIGIN! */
		lp->approx_alloc = 1;
		log_verbose("Converted %" PRIu32 "%%%s into %" PRIu32 " extents.", lp->extents, get_percent_string(lcp->percent), extents);
		lp->extents = extents;
	}

	if (lp->snapshot && lp->origin_name && lp->extents) {
		if (!lp->chunk_size) {
			log_error(INTERNAL_ERROR "Missing snapshot chunk size.");
			return 0;
		}

		if (!origin_lv && !(origin_lv = find_lv(vg, lp->origin_name))) {
			log_error("Couldn't find origin volume '%s'.",
				  lp->origin_name);
			return 0;
		}

		extents = cow_max_extents(origin_lv, lp->chunk_size);

		if (extents < lp->extents) {
			log_print_unless_silent("Reducing COW size %s down to maximum usable size %s.",
						display_size(vg->cmd, (uint64_t) vg->extent_size * lp->extents),
						display_size(vg->cmd, (uint64_t) vg->extent_size * extents));
			lp->extents = extents;
		}
	}

	if (!(stripesize_extents = lp->stripe_size / vg->extent_size))
		stripesize_extents = 1;

	if ((lcp->percent != PERCENT_NONE) && lp->stripes &&
	    (size_rest = lp->extents % (lp->stripes * stripesize_extents)) &&
	    (vg->free_count < lp->extents - size_rest + (lp->stripes * stripesize_extents))) {
		log_print_unless_silent("Rounding size (%d extents) down to stripe boundary "
					"size (%d extents)", lp->extents,
			  lp->extents - size_rest);
		lp->extents = lp->extents - size_rest;
	}

	if (lp->create_pool) {
		if (!update_pool_params(lp->segtype, vg, lp->target_attr,
					lp->passed_args, lp->extents,
					&lp->pool_metadata_size,
					&lp->thin_chunk_size_calc_policy, &lp->chunk_size,
					&lp->discards, &lp->zero))
			return_0;

		if (!(lp->pool_metadata_extents =
		      extents_from_size(vg->cmd, lp->pool_metadata_size, vg->extent_size)))
			return_0;
		if (lcp->percent == PERCENT_FREE) {
			if (lp->extents <= (2 * lp->pool_metadata_extents)) {
				log_error("Not enough space for thin pool creation.");
				return 0;
			}
			/* FIXME: persistent hidden space in VG wanted */
			lp->extents -= (2 * lp->pool_metadata_extents);
		}
	}

	return 1;
}

/*
 * Validate various common size arguments
 *
 * Note: at this place all volume types needs to be already
 *       identified, do not change them here.
 */
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

	/* poolmetadatasize_ARG is parsed in get_pool_params() */
	if (!lp->create_pool && arg_count(cmd, poolmetadatasize_ARG)) {
		log_error("--poolmetadatasize may only be specified when creating pools.");
		return 0;
	}

	if (arg_count(cmd, extents_ARG)) {
		if (arg_sign_value(cmd, extents_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Negative number of extents is invalid.");
			return 0;
		}
		if (!(lp->extents = arg_uint_value(cmd, extents_ARG, 0))) {
			log_error("Number of extents may not be zero.");
			return 0;
		}
		lcp->percent = arg_percent_value(cmd, extents_ARG, PERCENT_NONE);
	}

	/* Size returned in kilobyte units; held in sectors */
	if (arg_count(cmd, size_ARG)) {
		if (arg_sign_value(cmd, size_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Negative size is invalid.");
			return 0;
		}
		if (!(lcp->size = arg_uint64_value(cmd, size_ARG, UINT64_C(0)))) {
			log_error("Size may not be zero.");
			return 0;
		}
		lcp->percent = PERCENT_NONE;
	}

	if (arg_count(cmd, virtualsize_ARG)) {
		if (!lp->thin && !lp->snapshot) {
			log_error("--virtualsize may only be specified when creating thins or virtual snapshots.");
			return 0;
		}
		if (arg_sign_value(cmd, virtualsize_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Negative virtual origin size is invalid.");
			return 0;
		}
		/* Size returned in kilobyte units; held in sectors */
		if (!(lp->voriginsize = arg_uint64_value(cmd, virtualsize_ARG,
							 UINT64_C(0)))) {
			log_error("Virtual origin size may not be zero.");
			return 0;
		}
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
		if (arg_sign_value(cmd, regionsize_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Negative regionsize is invalid");
			return 0;
		}
		lp->region_size = arg_uint_value(cmd, regionsize_ARG, 0);
	} else {
		region_size = get_default_region_size(cmd);
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

	if (!strcmp(lp->segtype->name, SEG_TYPE_NAME_RAID10) && (lp->stripes < 2)) {
		if (arg_count(cmd, stripes_ARG)) {
			/* User supplied the bad argument */
			log_error("Segment type 'raid10' requires 2 or more stripes.");
			return 0;
		}
		/* No stripe argument was given - default to 2 */
		lp->stripes = 2;
		lp->stripe_size = find_config_tree_int(cmd, metadata_stripesize_CFG, NULL) * 2;
	}

	/*
	 * RAID types without a mirror component do not take '-m' arg
	 */
	if (!segtype_is_mirrored(lp->segtype) &&
	    arg_count(cmd, mirrors_ARG)) {
		log_error("Mirror argument cannot be used with segment type, %s",
			  lp->segtype->name);
		return 0;
	}

	/*
	 * RAID1 does not take a stripe arg
	 */
	if ((lp->stripes > 1) && seg_is_mirrored(lp) &&
	    strcmp(lp->segtype->name, SEG_TYPE_NAME_RAID10)) {
		log_error("Stripe argument cannot be used with segment type, %s",
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

	if (arg_count(cmd, minrecoveryrate_ARG))
		lp->min_recovery_rate = arg_uint_value(cmd,
						       minrecoveryrate_ARG, 0);
	if (arg_count(cmd, maxrecoveryrate_ARG))
		lp->max_recovery_rate = arg_uint_value(cmd,
						       maxrecoveryrate_ARG, 0);

	/* Rates are recorded in kiB/sec/disk, not sectors/sec/disk */
	lp->min_recovery_rate /= 2;
	lp->max_recovery_rate /= 2;

	if (lp->max_recovery_rate &&
	    (lp->max_recovery_rate < lp->min_recovery_rate)) {
		log_error("Minimum recovery rate cannot be higher than maximum.");
		return 0;
	}
	return 1;
}

static int _read_cache_pool_params(struct lvcreate_params *lp,
				  struct cmd_context *cmd)
{
	const char *cachemode;

	if (!seg_is_cache(lp) && !seg_is_cache_pool(lp))
		return 1;

	if (!(cachemode = arg_str_value(cmd, cachemode_ARG, NULL)))
		cachemode = find_config_tree_str(cmd, allocation_cache_pool_cachemode_CFG, NULL);

	if (!get_cache_mode(cachemode, &lp->feature_flags))
		return_0;

	return 1;
}

static int _read_activation_params(struct lvcreate_params *lp,
				   struct cmd_context *cmd,
				   struct volume_group *vg)
{
	unsigned pagesize;

	lp->activate = (activation_change_t)
		arg_uint_value(cmd, activate_ARG, CHANGE_AY);

	if (!is_change_activating(lp->activate)) {
		if (lp->zero && !seg_is_thin(lp)) {
			log_error("--activate n requires --zero n");
			return 0;
		}
	} else if (lp->activate == CHANGE_AAY) {
		if (arg_count(cmd, zero_ARG) || arg_count(cmd, wipesignatures_ARG)) {
			log_error("-Z and -W is incompatible with --activate a");
			return 0;
		}
		lp->zero = 0;
	}

	/* Read ahead */
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

	/* Permissions */
	lp->permission = arg_uint_value(cmd, permission_ARG,
					LVM_READ | LVM_WRITE);

	if (lp->snapshot) {
		/* Snapshot has to zero COW header */
		lp->zero = 1;
		lp->wipe_signatures = 0;
	} else if (!(lp->permission & LVM_WRITE)) {
		/* Must not zero/wipe read only volume */
		lp->zero = 0;
		lp->wipe_signatures = 0;
	}

	/* Persistent minor (and major) */
	if (arg_is_set(cmd, persistent_ARG)) {
		if (lp->create_pool && !lp->thin) {
			log_error("--persistent is not permitted when creating a thin pool device.");
			return 0;
		}

		if (!get_and_validate_major_minor(cmd, vg->fid->fmt,
						  &lp->major, &lp->minor))
			return_0;
	} else if (arg_is_set(cmd, major_ARG) || arg_is_set(cmd, minor_ARG)) {
		log_error("--major and --minor require -My.");
		return 0;
	}

	if (arg_is_set(cmd, setactivationskip_ARG)) {
		lp->activation_skip |= ACTIVATION_SKIP_SET;
		if (arg_int_value(cmd, setactivationskip_ARG, 0))
			lp->activation_skip |= ACTIVATION_SKIP_SET_ENABLED;
	}

	if (arg_is_set(cmd, ignoreactivationskip_ARG))
		lp->activation_skip |= ACTIVATION_SKIP_IGNORE;

	if (lp->zero && (lp->activation_skip & ACTIVATION_SKIP_SET_ENABLED)
	    && !(lp->activation_skip & ACTIVATION_SKIP_IGNORE)) {
		log_error("--setactivationskip y requires either --zero n "
			  "or --ignoreactivationskip");
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

	memset(lcp, 0, sizeof(*lcp));
	dm_list_init(&lp->tags);
	lp->target_attr = ~0;
	lp->yes = arg_count(cmd, yes_ARG);
	lp->force = (force_t) arg_count(cmd, force_ARG);

	/* Set default segtype - remember, '-m 0' implies stripe. */
	if (arg_count(cmd, mirrors_ARG) &&
	    arg_uint_value(cmd, mirrors_ARG, 0))
		if (arg_uint_value(cmd, arg_count(cmd, stripes_long_ARG) ?
				   stripes_long_ARG : stripes_ARG, 1) > 1) {
			segtype_str = find_config_tree_str(cmd, global_raid10_segtype_default_CFG, NULL);;
		} else {
			segtype_str = find_config_tree_str(cmd, global_mirror_segtype_default_CFG, NULL);
		}
	else if (arg_count(cmd, snapshot_ARG))
		segtype_str = "snapshot";
	else if (arg_count(cmd, cache_ARG))
		segtype_str = "cache";
	else if (arg_count(cmd, thin_ARG))
		segtype_str = "thin";
	/* estimations after valid shortcuts */
	else if (arg_count(cmd, cachepool_ARG))
		segtype_str = "cache";
	else if (arg_count(cmd, thinpool_ARG))
		segtype_str = "thin";
	else
		segtype_str = "striped";

	segtype_str = arg_str_value(cmd, type_ARG, segtype_str);

	if (!(lp->segtype = get_segtype_from_string(cmd, segtype_str)))
		return_0;

	if (seg_unknown(lp)) {
		log_error("Unable to create LV with unknown segment type %s.", segtype_str);
		return 0;
	}

	if (seg_is_snapshot(lp) ||
	    (!seg_is_thin(lp) && arg_count(cmd, virtualsize_ARG))) {
		if (arg_from_list_is_set(cmd, "is unsupported with snapshots",
					 cache_ARG, cachepool_ARG,
					 mirrors_ARG,
					 thin_ARG,
					 zero_ARG,
					 -1))
			return_0;

		if (seg_is_pool(lp)) {
			log_error("Snapshots are incompatible with pool segment types.");
			return 0;
		}

		if (arg_is_set(cmd, thinpool_ARG) &&
		    (arg_is_set(cmd, extents_ARG) || arg_is_set(cmd, size_ARG))) {
			log_error("Cannot specify snapshot size and thin pool.");
			return 0;
		}

		lp->snapshot = 1;
	}

	if (seg_is_thin_volume(lp)) {
		if (arg_is_set(cmd, virtualsize_ARG))
			lp->thin = 1;
		else if (arg_is_set(cmd, name_ARG)) {
			log_error("Missing --virtualsize when specified --name of thin volume.");
			return 0;
		/* When no virtual size -> create thin-pool only */
		} else if (!(lp->segtype = get_segtype_from_string(cmd, "thin-pool")))
			return_0;

		/* If size/extents given with thin, then we are creating a thin pool */
		if (arg_count(cmd, size_ARG) || arg_count(cmd, extents_ARG)) {
			if (lp->snapshot &&
			    (arg_is_set(cmd, cachepool_ARG) || arg_is_set(cmd, thinpool_ARG))) {
				log_error("Size or extents cannot be specified with pool name.");
				return 0;
			}
			lp->create_pool = 1;
		}
	} else if (seg_is_pool(lp)) {
		if (arg_from_list_is_set(cmd, "is unsupported with pool segment type",
					 cache_ARG, mirrors_ARG, snapshot_ARG, thin_ARG,
					 virtualsize_ARG,
					 -1))
			return_0;
		lp->create_pool = 1;
	}

	if (seg_is_mirrored(lp) || seg_is_raid(lp)) {
		/* Note: snapshots have snapshot_ARG or virtualsize_ARG */
		if (arg_from_list_is_set(cmd, "is unsupported with mirrored segment type",
					 cache_ARG, cachepool_ARG,
					 snapshot_ARG,
					 thin_ARG, thinpool_ARG,
					 virtualsize_ARG,
					 -1))
			return_0;
	} else if (arg_from_list_is_set(cmd, "is unsupported without mirrored segment type",
					corelog_ARG, mirrorlog_ARG, nosync_ARG,
					-1))
		return_0;

	/* Default to 2 mirrored areas if '--type mirror|raid1|raid10' */
	lp->mirrors = segtype_is_mirrored(lp->segtype) ? 2 : 1;

	if (arg_count(cmd, mirrors_ARG)) {
		if (arg_sign_value(cmd, mirrors_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Mirrors argument may not be negative");
			return 0;
		}

		lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0) + 1;

		if (lp->mirrors > DEFAULT_MIRROR_MAX_IMAGES) {
			log_error("Only up to " DM_TO_STRING(DEFAULT_MIRROR_MAX_IMAGES)
				  " images in mirror supported currently.");
			return 0;
		}

		if (lp->mirrors == 1) {
			if (segtype_is_mirrored(lp->segtype)) {
				log_error("--mirrors must be at least 1 with segment type %s.", lp->segtype->name);
				return 0;
			}
			log_print_unless_silent("Redundant mirrors argument: default is 0");
		}

		if ((lp->mirrors > 2) && !strcmp(lp->segtype->name, SEG_TYPE_NAME_RAID10)) {
			/*
			 * FIXME: When RAID10 is no longer limited to
			 *        2-way mirror, 'lv_mirror_count()'
			 *        must also change for RAID10.
			 */
			log_error("RAID10 currently supports "
				  "only 2-way mirroring (i.e. '-m 1')");
			return 0;
		}
	}

	if (activation() && lp->segtype->ops->target_present) {
		if (!lp->segtype->ops->target_present(cmd, NULL, &lp->target_attr)) {
			log_error("%s: Required device-mapper target(s) not detected in your kernel.",
				  lp->segtype->name);
			return 0;
		}

		if (!strcmp(lp->segtype->name, SEG_TYPE_NAME_RAID10) &&
		    !(lp->target_attr & RAID_FEATURE_RAID10)) {
			log_error("RAID module does not support RAID10.");
			return 0;
		}
	}

	/*
	 * Should we zero/wipe signatures on the lv.
	 */
	lp->zero = (lp->segtype->flags & SEG_CANNOT_BE_ZEROED)
		? 0 : arg_int_value(cmd, zero_ARG, 1);

	if (arg_count(cmd, wipesignatures_ARG)) {
		/* If -W/--wipesignatures is given on command line directly, respect it. */
		lp->wipe_signatures = (lp->segtype->flags & SEG_CANNOT_BE_ZEROED)
			? 0 : arg_int_value(cmd, wipesignatures_ARG, 1);
	} else {
		/*
		 * If -W/--wipesignatures is not given on command line,
		 * look at the allocation/wipe_signatures_when_zeroing_new_lvs
		 * to decide what should be done exactly.
		 */
		if (find_config_tree_bool(cmd, allocation_wipe_signatures_when_zeroing_new_lvs_CFG, NULL))
			lp->wipe_signatures = lp->zero;
		else
			lp->wipe_signatures = 0;
	}

	if (arg_count(cmd, chunksize_ARG) &&
	    (arg_sign_value(cmd, chunksize_ARG, SIGN_NONE) == SIGN_MINUS)) {
		log_error("Negative chunk size is invalid.");
		return 0;
	}

	if (!_lvcreate_name_params(lp, cmd, &argc, &argv) ||
	    !_read_size_params(lp, lcp, cmd) ||
	    !get_stripe_params(cmd, &lp->stripes, &lp->stripe_size) ||
	    (lp->create_pool &&
	     !get_pool_params(cmd, lp->segtype, &lp->passed_args,
			      &lp->pool_metadata_size, &lp->pool_metadata_spare,
			      &lp->chunk_size, &lp->discards, &lp->zero)) ||
	    !_read_mirror_params(lp, cmd) ||
	    !_read_raid_params(lp, cmd) ||
	    !_read_cache_pool_params(lp, cmd))
		return_0;

	if (lp->snapshot && (lp->extents || lcp->size)) {
		lp->chunk_size = arg_uint_value(cmd, chunksize_ARG, 8);
		if (lp->chunk_size < 8 || lp->chunk_size > 1024 ||
		    (lp->chunk_size & (lp->chunk_size - 1))) {
			log_error("Chunk size must be a power of 2 in the "
				  "range 4K to 512K.");
			return 0;
		}
		log_verbose("Setting chunksize to %s.", display_size(cmd, lp->chunk_size));

		if (!(lp->segtype = get_segtype_from_string(cmd, "snapshot")))
			return_0;
	} else if (!lp->create_pool && arg_count(cmd, chunksize_ARG)) {
		log_error("--chunksize is only available with snapshots and pools.");
		return 0;
	}

	if (!lp->create_pool &&
	    arg_count(cmd, poolmetadataspare_ARG)) {
		log_error("--poolmetadataspare is only available with pool creation.");
		return 0;
	}
	/*
	 * Allocation parameters
	 */
	contiguous = arg_int_value(cmd, contiguous_ARG, 0);
	lp->alloc = contiguous ? ALLOC_CONTIGUOUS : ALLOC_INHERIT;
	lp->alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, lp->alloc);

	if (contiguous && (lp->alloc != ALLOC_CONTIGUOUS)) {
		log_error("Conflicting contiguous and alloc arguments.");
		return 0;
	}

	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(current_group->arg_values, addtag_ARG))
			continue;

		if (!(tag = grouped_arg_str_value(current_group->arg_values, addtag_ARG, NULL))) {
			log_error("Failed to get tag.");
			return 0;
		}

		if (!str_list_add(cmd->mem, &lp->tags, tag)) {
			log_error("Unable to allocate memory for tag %s.", tag);
			return 0;
		}
	}

	lcp->pv_count = argc;
	lcp->pvs = argv;

	return 1;
}

/*
 * _determine_cache_argument
 * @vg
 * @lp
 *
 * 'lp->pool_name' is set with an LV that could be either the cache_pool name
 * or the origin name of the cached LV which is being created.
 * This function determines which it is and sets 'lp->origin_name' or
 * 'lp->pool_name' appropriately.
 */
static int _determine_cache_argument(struct volume_group *vg,
				     struct lvcreate_params *lp)
{
	struct logical_volume *lv;

	if (!seg_is_cache(lp)) {
		log_error(INTERNAL_ERROR
			  "Unable to determine cache argument on %s segtype",
			  lp->segtype->name);
		return 0;
	}

	if (!lp->pool_name) {
		lp->pool_name = lp->lv_name;
	} else if (lp->pool_name == lp->origin_name) {
		if (!(lv = find_lv(vg, lp->pool_name))) {
			/* Cache pool nor origin volume exists */
			lp->cache = 0;
			lp->origin_name = NULL;
			if (!(lp->segtype = get_segtype_from_string(vg->cmd, "cache-pool")))
				return_0;
		} else if (!lv_is_cache_pool(lv)) {
			/* Name arg in this case is for pool name */
			lp->pool_name = lp->lv_name;
			/* We were given origin for caching */
		} else {
			/* FIXME error on pool args */
			lp->create_pool = 0;
			lp->origin_name = NULL;
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
	struct logical_volume *lv, *pool_lv = NULL;

	if (!(lv = find_lv(vg, lp->origin_name))) {
		log_error("Snapshot origin LV %s not found in Volume group %s.",
			  lp->origin_name, vg->name);
		return 0;
	}

	if (lv_is_cache(lv)) {
		log_error("Snapshot of cache LV is not yet supported.");
		return 0;
	}

	if (lp->pool_name) {
		if (!(pool_lv = find_lv(vg, lp->pool_name))) {
			log_error("Thin pool volume %s not found in Volume group %s.",
				  lp->pool_name, vg->name);
			return 0;
		}

		if (!lv_is_thin_pool(pool_lv)) {
			log_error("Logical volume %s is not a thin pool volume.",
				  display_lvname(pool_lv));
			return 0;
		}
	}

	if (!arg_count(vg->cmd, extents_ARG) && !arg_count(vg->cmd, size_ARG)) {
		if (lv_is_thin_volume(lv) && !lp->pool_name)
			lp->pool_name = first_seg(lv)->pool_lv->name;

		if (seg_is_thin(lp) || lp->pool_name) {
			if (!(lp->segtype = get_segtype_from_string(vg->cmd, "thin")))
				return_0;
			return 1;
		}

		log_error("Please specify either size or extents with snapshots.");
		return 0;
	} else if (lp->pool_name) {
		log_error("Cannot specify size with thin pool snapshot.");
		return 0;
	}

	return 1;
}

static int _check_thin_parameters(struct volume_group *vg, struct lvcreate_params *lp,
				  struct lvcreate_cmdline_params *lcp)
{
	struct logical_volume *pool_lv = NULL;

	if (!lp->thin && !lp->create_pool && !lp->snapshot) {
		log_error("Please specify device size(s).");
		return 0;
	}

	if (lp->thin && lp->snapshot) {
		log_error("Please either create snapshot or thin volume.");
		return 0;
	}

	if (lp->pool_name)
		pool_lv = find_lv(vg, lp->pool_name);

	if (!lp->create_pool) {
		if (arg_from_list_is_set(vg->cmd, "is only available with thin pool creation",
					 alloc_ARG,
					 chunksize_ARG,
					 contiguous_ARG,
					 discards_ARG,
					 poolmetadatasize_ARG,
					 poolmetadataspare_ARG,
					 stripes_ARG,
					 stripesize_ARG,
					 zero_ARG,
					 -1))
			return_0;

		if (lcp->pv_count) {
			log_error("Only specify Physical volumes when allocating the thin pool.");
			return 0;
		}

		if (!lp->pool_name) {
			log_error("Please specify name of existing thin pool.");
			return 0;
		}

		if (!pool_lv) {
			log_error("Thin pool %s not found in Volume group %s.", lp->pool_name, vg->name);
			return 0;
		}

		if (!lv_is_thin_pool(pool_lv)) {
			log_error("Logical volume %s is not a thin pool.", display_lvname(pool_lv));
			return 0;
		}
	} else if (pool_lv) {
		log_error("Logical volume %s already exists in Volume group %s.", lp->pool_name, vg->name);
		return 0;
	}

	if (!lp->thin && !lp->snapshot) {
		if (lp->lv_name) {
			log_error("--name may only be given when creating a new thin Logical volume or snapshot.");
			return 0;
		}
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

static int _check_raid_parameters(struct volume_group *vg,
				  struct lvcreate_params *lp,
				  struct lvcreate_cmdline_params *lcp)
{
	unsigned devs = lcp->pv_count ? : dm_list_size(&vg->pvs);
	struct cmd_context *cmd = vg->cmd;

	/*
	 * If number of devices was not supplied, we can infer from
	 * the PVs given.
	 */
	if (!seg_is_mirrored(lp)) {
		if (!arg_count(cmd, stripes_ARG) &&
		    (devs > 2 * lp->segtype->parity_devs))
			lp->stripes = devs - lp->segtype->parity_devs;

		if (!lp->stripe_size)
			lp->stripe_size = find_config_tree_int(cmd, metadata_stripesize_CFG, NULL) * 2;

		if (lp->stripes <= lp->segtype->parity_devs) {
			log_error("Number of stripes must be at least %d for %s",
				  lp->segtype->parity_devs + 1,
				  lp->segtype->name);
			return 0;
		}
	} else if (!strcmp(lp->segtype->name, SEG_TYPE_NAME_RAID10)) {
		if (!arg_count(cmd, stripes_ARG))
			lp->stripes = devs / lp->mirrors;
		if (lp->stripes < 2) {
			log_error("Unable to create RAID10 LV,"
				  " insufficient number of devices.");
			return 0;
		}
	}
	/* 'mirrors' defaults to 2 - not the number of PVs supplied */

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
	   thin  create_pool  snapshot   origin   pool
	     1        1           0         0      y/n    - create new pool and a thin LV in it
	     1        0           0         0      y      - create new thin LV in existing pool
	     0        1           0         0      y/n    - create new pool only
	     1        0           1         1      y      - create thin snapshot of existing thin LV
	*/

	if (!lp->create_pool && !lp->pool_name) {
		log_error(INTERNAL_ERROR "--thinpool not identified.");
		r = 0;
	}

	if ((lp->snapshot && !lp->origin_name) || (!lp->snapshot && lp->origin_name)) {
		log_error(INTERNAL_ERROR "Inconsistent snapshot and origin parameters identified.");
		r = 0;
	}

	if (!lp->thin && !lp->create_pool && !lp->snapshot) {
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
	int r = ECMD_FAILED;
	struct lvcreate_params lp = {
		.major = -1,
		.minor = -1,
	};
	struct lvcreate_cmdline_params lcp;
	struct volume_group *vg;

	if (!_lvcreate_params(&lp, &lcp, cmd, argc, argv))
		return EINVALID_CMD_LINE;

	log_verbose("Finding volume group \"%s\"", lp.vg_name);
	vg = vg_read_for_update(cmd, lp.vg_name, NULL, 0);
	if (vg_read_error(vg)) {
		release_vg(vg);
		return_ECMD_FAILED;
	}

	if (lp.snapshot && lp.origin_name && !_determine_snapshot_type(vg, &lp))
		goto_out;

	if (seg_is_thin(&lp) && !_check_thin_parameters(vg, &lp, &lcp))
		goto_out;

	if (seg_is_cache(&lp) && !_determine_cache_argument(vg, &lp))
		goto_out;

	if (seg_is_raid(&lp) && !_check_raid_parameters(vg, &lp, &lcp))
		goto_out;

	/*
	 * Check activation parameters to support inactive thin snapshot creation
	 * FIXME: anything else needs to be moved past _determine_snapshot_type()?
	 */
	if (!_read_activation_params(&lp, cmd, vg))
		goto_out;

	if (!_update_extents_params(vg, &lp, &lcp))
		goto_out;

	if (seg_is_thin(&lp) && !_validate_internal_thin_processing(&lp))
		goto_out;

	if (lp.create_pool) {
		if (!handle_pool_metadata_spare(vg, lp.pool_metadata_extents,
						lp.pvh, lp.pool_metadata_spare))
			goto_out;

		log_verbose("Making pool %s in VG %s using segtype %s",
			    lp.pool_name ? : "with generated name", lp.vg_name, lp.segtype->name);
	}

	if (lp.thin)
		log_verbose("Making thin LV %s in pool %s in VG %s%s%s using segtype %s",
			    lp.lv_name ? : "with generated name",
			    lp.pool_name ? : "with generated name", lp.vg_name,
			    lp.snapshot ? " as snapshot of " : "",
			    lp.snapshot ? lp.origin_name : "", lp.segtype->name);

	if (!lv_create_single(vg, &lp))
		goto_out;

	r = ECMD_PROCESSED;
out:
	unlock_and_release_vg(cmd, vg, lp.vg_name);
	return r;
}
