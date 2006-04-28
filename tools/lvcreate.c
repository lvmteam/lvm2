/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved. 
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
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

#include <fcntl.h>

struct lvcreate_params {
	/* flags */
	int snapshot;
	int zero;
	int major;
	int minor;

	char *origin;
	const char *vg_name;
	const char *lv_name;

	uint32_t stripes;
	uint32_t stripe_size;
	uint32_t chunk_size;
	uint32_t region_size;

	uint32_t mirrors;

	struct segment_type *segtype;

	/* size */
	uint32_t extents;
	uint64_t size;

	uint32_t permission;
	uint32_t read_ahead;
	alloc_policy_t alloc;

	int pv_count;
	char **pvs;
};

static int _lvcreate_name_params(struct lvcreate_params *lp,
				 struct cmd_context *cmd,
				 int *pargc, char ***pargv)
{
	int argc = *pargc;
	char **argv = *pargv, *ptr;
	char *vg_name;

	if (arg_count(cmd, name_ARG))
		lp->lv_name = arg_value(cmd, name_ARG);

	if (lp->snapshot) {
		if (!argc) {
			log_err("Please specify a logical volume to act as "
				"the snapshot origin.");
			return 0;
		}

		lp->origin = argv[0];
		(*pargv)++, (*pargc)--;
		if (!(lp->vg_name = extract_vgname(cmd, lp->origin))) {
			log_err("The origin name should include the "
				"volume group.");
			return 0;
		}

		/* Strip the volume group from the origin */
		if ((ptr = strrchr(lp->origin, (int) '/')))
			lp->origin = ptr + 1;

	} else {
		/*
		 * If VG not on command line, try -n arg and then
		 * environment.
		 */
		if (!argc) {
			if (!(lp->vg_name = extract_vgname(cmd, lp->lv_name))) {
				log_err("Please provide a volume group name");
				return 0;
			}

		} else {
			vg_name = argv[0];
			/* Strip dev_dir (optional) */
			if (*vg_name == '/') {
				while (*vg_name == '/')
					vg_name++;
				vg_name--;
			}
			if (!strncmp(vg_name, cmd->dev_dir,
				     strlen(cmd->dev_dir))) {
				vg_name += strlen(cmd->dev_dir);
				while (*vg_name == '/')
					vg_name++;
			}
			if (strrchr(vg_name, '/')) {
				log_error("Volume group name expected "
					  "(no slash)");
				return 0;
			}

			/*
			 * Ensure lv_name doesn't contain a
			 * different VG.
			 */
			if (lp->lv_name && strchr(lp->lv_name, '/')) {
				if (!(lp->vg_name =
				      extract_vgname(cmd, lp->lv_name)))
					return 0;

				if (strcmp(lp->vg_name, vg_name)) {
					log_error("Inconsistent volume group "
						  "names "
						  "given: \"%s\" and \"%s\"",
						  lp->vg_name, vg_name);
					return 0;
				}
			}

			lp->vg_name = vg_name;
			(*pargv)++, (*pargc)--;
		}
	}

	if (lp->lv_name) {
		if ((ptr = strrchr(lp->lv_name, '/')))
			lp->lv_name = ptr + 1;

		if (!apply_lvname_restrictions(lp->lv_name)) {
			stack;
			return 0;
		}

		if (!validate_name(lp->lv_name)) {
			log_error("Logical volume name \"%s\" is invalid",
				  lp->lv_name);
			return 0;
		}
	}

	return 1;
}

static int _read_size_params(struct lvcreate_params *lp,
			     struct cmd_context *cmd, int *pargc, char ***pargv)
{
	if (arg_count(cmd, extents_ARG) + arg_count(cmd, size_ARG) != 1) {
		log_error("Please specify either size or extents (not both)");
		return 0;
	}

	if (arg_count(cmd, extents_ARG)) {
		if (arg_sign_value(cmd, extents_ARG, 0) == SIGN_MINUS) {
			log_error("Negative number of extents is invalid");
			return 0;
		}
		lp->extents = arg_uint_value(cmd, extents_ARG, 0);
	}

	/* Size returned in kilobyte units; held in sectors */
	if (arg_count(cmd, size_ARG)) {
		if (arg_sign_value(cmd, size_ARG, 0) == SIGN_MINUS) {
			log_error("Negative size is invalid");
			return 0;
		}
		lp->size = arg_uint64_value(cmd, size_ARG, UINT64_C(0)) * 2;
	}

	return 1;
}

static int _read_stripe_params(struct lvcreate_params *lp,
			       struct cmd_context *cmd,
			       int *pargc, char ***pargv)
{
	int argc = *pargc;

	if (arg_count(cmd, stripesize_ARG)) {
		if (arg_sign_value(cmd, stripesize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative stripesize is invalid");
			return 0;
		}
		lp->stripe_size = 2 * arg_uint_value(cmd, stripesize_ARG, 0);
	}

	if (lp->stripes == 1 && lp->stripe_size) {
		log_print("Ignoring stripesize argument with single stripe");
		lp->stripe_size = 0;
	}

	if (lp->stripes > 1 && !lp->stripe_size) {
		lp->stripe_size = find_config_int(cmd->cft->root,
						  "metadata/stripesize",
						  DEFAULT_STRIPESIZE) * 2;
		log_print("Using default stripesize %dKB", lp->stripe_size / 2);
	}

	if (argc && (unsigned) argc < lp->stripes) {
		log_error("Too few physical volumes on "
			  "command line for %d-way striping", lp->stripes);
		return 0;
	}

	if (lp->stripes < 1 || lp->stripes > MAX_STRIPES) {
		log_error("Number of stripes (%d) must be between %d and %d",
			  lp->stripes, 1, MAX_STRIPES);
		return 0;
	}

	if (lp->stripes > 1 && (lp->stripe_size < STRIPE_SIZE_MIN ||
				lp->stripe_size > STRIPE_SIZE_MAX ||
				lp->stripe_size & (lp->stripe_size - 1))) {
		log_error("Invalid stripe size %d", lp->stripe_size);
		return 0;
	}

	return 1;
}

static int _read_mirror_params(struct lvcreate_params *lp,
			       struct cmd_context *cmd,
			       int *pargc, char ***pargv)
{
	int argc = *pargc;

	if (argc && (unsigned) argc < lp->mirrors) {
		log_error("Too few physical volumes on "
			  "command line for %d-way mirroring", lp->mirrors);
		return 0;
	}

	if (arg_count(cmd, regionsize_ARG)) {
		if (arg_sign_value(cmd, regionsize_ARG, 0) == SIGN_MINUS) {
			log_error("Negative regionsize is invalid");
			return 0;
		}
		lp->region_size = 2 * arg_uint_value(cmd, regionsize_ARG, 0);
	} else
		lp->region_size = 2 * find_config_int(cmd->cft->root,
					"activation/mirror_region_size",
					DEFAULT_MIRROR_REGION_SIZE);

	if (lp->region_size & (lp->region_size - 1)) {
		log_error("Region size (%" PRIu32 ") must be a power of 2",
			  lp->region_size);
		return 0;
	}

	if (!lp->region_size) {
		log_error("Non-zero region size must be supplied.");
		return 0;
	}

	return 1;
}

static int _lvcreate_params(struct lvcreate_params *lp, struct cmd_context *cmd,
			    int argc, char **argv)
{
	int contiguous;

	memset(lp, 0, sizeof(*lp));

	/*
	 * Check selected options are compatible and determine segtype
	 */
	lp->segtype = (struct segment_type *)
	    arg_ptr_value(cmd, type_ARG,
			  get_segtype_from_string(cmd, "striped"));

	lp->stripes = arg_uint_value(cmd, stripes_ARG, 1);
	if (arg_count(cmd, stripes_ARG) && lp->stripes == 1)
		log_print("Redundant stripes argument: default is 1");

	if (arg_count(cmd, snapshot_ARG) || seg_is_snapshot(lp))
		lp->snapshot = 1;

	lp->mirrors = 1;

	/* Default to 2 mirrored areas if --type mirror */
	if (seg_is_mirrored(lp))
		lp->mirrors = 2;

	if (arg_count(cmd, mirrors_ARG)) {
		lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0) + 1;
		if (lp->mirrors == 1)
			log_print("Redundant mirrors argument: default is 0");
		if (arg_sign_value(cmd, mirrors_ARG, 0) == SIGN_MINUS) {
			log_error("Mirrors argument may not be negative");
			return 0;
		}
	}

	if (lp->snapshot) {
		if (arg_count(cmd, zero_ARG)) {
			log_error("-Z is incompatible with snapshots");
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

		if (!(lp->segtype = get_segtype_from_string(cmd, "snapshot"))) {
			stack;
			return 0;
		}
	} else {
		if (arg_count(cmd, chunksize_ARG)) {
			log_error("-c is only available with snapshots");
			return 0;
		}
	}

	if (lp->mirrors > 1) {
		if (lp->snapshot) {
			log_error("mirrors and snapshots are currently "
				  "incompatible");
			return 0;
		}

		if (lp->stripes > 1) {
			log_error("mirrors and stripes are currently "
				  "incompatible");
			return 0;
		}

		if (!(lp->segtype = get_segtype_from_string(cmd, "mirror"))) {
			stack;
			return 0;
		}
	}

	if (activation() && lp->segtype->ops->target_present &&
	    !lp->segtype->ops->target_present()) {
		log_error("%s: Required device-mapper target(s) not "
			  "detected in your kernel", lp->segtype->name);
		return 0;
	}

	if (!_lvcreate_name_params(lp, cmd, &argc, &argv) ||
	    !_read_size_params(lp, cmd, &argc, &argv) ||
	    !_read_stripe_params(lp, cmd, &argc, &argv) ||
	    !_read_mirror_params(lp, cmd, &argc, &argv)) {
		stack;
		return 0;
	}

	/*
	 * Should we zero the lv.
	 */
	lp->zero = strcmp(arg_str_value(cmd, zero_ARG, 
		(lp->segtype->flags & SEG_CANNOT_BE_ZEROED) ? "n" : "y"), "n");

	/*
	 * Alloc policy
	 */
	contiguous = strcmp(arg_str_value(cmd, contiguous_ARG, "n"), "n");

	lp->alloc = contiguous ? ALLOC_CONTIGUOUS : ALLOC_INHERIT;

	lp->alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, lp->alloc);

	if (contiguous && (lp->alloc != ALLOC_CONTIGUOUS)) {
		log_error("Conflicting contiguous and alloc arguments");
		return 0;
	}

	/*
	 * Read ahead.
	 */
	if (arg_count(cmd, readahead_ARG))
		lp->read_ahead = arg_uint_value(cmd, readahead_ARG, 0);

	/*
	 * Permissions.
	 */
	if (arg_count(cmd, permission_ARG))
		lp->permission = arg_uint_value(cmd, permission_ARG, 0);
	else
		lp->permission = LVM_READ | LVM_WRITE;

	lp->minor = arg_int_value(cmd, minor_ARG, -1);
	lp->major = arg_int_value(cmd, major_ARG, -1);

	/* Persistent minor */
	if (arg_count(cmd, persistent_ARG)) {
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
	}

	lp->pv_count = argc;
	lp->pvs = argv;

	return 1;
}

static int _lvcreate(struct cmd_context *cmd, struct lvcreate_params *lp)
{
	uint32_t size_rest;
	uint32_t status = 0;
	uint64_t tmp_size;
	struct volume_group *vg;
	struct logical_volume *lv, *org = NULL, *log_lv = NULL;
	struct list *pvh;
	const char *tag;
	int consistent = 1;
	struct alloc_handle *ah = NULL;
	char lv_name_buf[128];
	const char *lv_name;

	status |= lp->permission | VISIBLE_LV;

	/* does VG exist? */
	log_verbose("Finding volume group \"%s\"", lp->vg_name);

	if (!(vg = vg_read(cmd, lp->vg_name, NULL, &consistent))) {
		log_error("Volume group \"%s\" doesn't exist", lp->vg_name);
		return 0;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", lp->vg_name);
		return 0;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", lp->vg_name);
		return 0;
	}

	if (lp->lv_name && find_lv_in_vg(vg, lp->lv_name)) {
		log_error("Logical volume \"%s\" already exists in "
			  "volume group \"%s\"", lp->lv_name, lp->vg_name);
		return 0;
	}

	if (lp->mirrors > 1 && !(vg->fid->fmt->features & FMT_SEGMENTS)) {
		log_error("Metadata does not support mirroring.");
		return 0;
	}

	/*
	 * Create the pv list.
	 */
	if (lp->pv_count) {
		if (!(pvh = create_pv_list(cmd->mem, vg,
					   lp->pv_count, lp->pvs, 1))) {
			stack;
			return 0;
		}
	} else
		pvh = &vg->pvs;

	if (lp->stripe_size > vg->extent_size) {
		log_error("Setting stripe size %d KB to physical extent "
			  "size %u KB", lp->stripe_size / 2,
			  vg->extent_size / 2);
		lp->stripe_size = vg->extent_size;
	}

	if (lp->size) {
		/* No of 512-byte sectors */
		tmp_size = lp->size;

		if (tmp_size % vg->extent_size) {
			tmp_size += vg->extent_size - tmp_size %
			    vg->extent_size;
			log_print("Rounding up size to full physical extent %s",
				  display_size(cmd, tmp_size, SIZE_SHORT));
		}

		lp->extents = tmp_size / vg->extent_size;
	}

	if ((size_rest = lp->extents % lp->stripes)) {
		log_print("Rounding size (%d extents) up to stripe boundary "
			  "size (%d extents)", lp->extents,
			  lp->extents - size_rest + lp->stripes);
		lp->extents = lp->extents - size_rest + lp->stripes;
	}

	if (lp->snapshot) {
		if (!activation()) {
			log_error("Can't create snapshot without using "
				  "device-mapper kernel driver");
			return 0;
		}
		/* FIXME Allow exclusive activation. */
		if (vg->status & CLUSTERED) {
			log_error("Clustered snapshots are not yet supported.");
			return 0;
		}
		if (!(org = find_lv(vg, lp->origin))) {
			log_err("Couldn't find origin volume '%s'.",
				lp->origin);
			return 0;
		}
		if (lv_is_cow(org)) {
			log_error("Snapshots of snapshots are not supported "
				  "yet.");
			return 0;
		}
		if (org->status & LOCKED) {
			log_error("Snapshots of locked devices are not "
				  "supported yet");
			return 0;
		}
		/* Must zero cow */
		status |= LVM_WRITE;
	}

	if (!lp->extents) {
		log_error("Unable to create logical volume %s with no extents",
			  lp->lv_name);
		return 0;
	}

	if (!seg_is_virtual(lp) &&
	    vg->free_count < lp->extents) {
		log_error("Insufficient free extents (%u) in volume group %s: "
			  "%u required", vg->free_count, vg->name, lp->extents);
		return 0;
	}

	if (lp->stripes > list_size(pvh) && lp->alloc != ALLOC_ANYWHERE) {
		log_error("Number of stripes (%u) must not exceed "
			  "number of physical volumes (%d)", lp->stripes,
			  list_size(pvh));
		return 0;
	}

	if (lp->mirrors > 1 && !activation()) {
		log_error("Can't create mirror without using "
			  "device-mapper kernel driver.");
		return 0;
	}

	/* The snapshot segment gets created later */
	if (lp->snapshot &&
	    !(lp->segtype = get_segtype_from_string(cmd, "striped"))) {
		stack;
		return 0;
	}

	if (!archive(vg))
		return 0;

	if (lp->lv_name)
		lv_name = lp->lv_name;
	else {
		if (!generate_lv_name(vg, "lvol%d", lv_name_buf, sizeof(lv_name_buf))) {
			log_error("Failed to generate LV name.");
			return 0;
		}
		lv_name = &lv_name_buf[0];
	}

	if (lp->mirrors > 1) {
		/* FIXME Calculate how many extents needed for the log */

		if (!(ah = allocate_extents(vg, NULL, lp->segtype, lp->stripes,
					    lp->mirrors, 1, lp->extents,
					    NULL, 0, 0, pvh, lp->alloc,
					    NULL))) {
			stack;
			return 0;
		}

		lp->region_size = adjusted_mirror_region_size(vg->extent_size,
							      lp->extents,
							      lp->region_size);

		if (!(log_lv = create_mirror_log(cmd, vg, ah, lp->alloc,
						 lv_name))) {
			log_error("Failed to create mirror log.");
			return 0;
		}
	}

	if (!(lv = lv_create_empty(vg->fid, lv_name ? lv_name : "lvol%d", NULL,
				   status, lp->alloc, 0, vg))) {
		stack;
		goto error;
	}

	if (lp->read_ahead) {
		log_verbose("Setting read ahead sectors");
		lv->read_ahead = lp->read_ahead;
	}

	if (lp->minor >= 0) {
		lv->major = lp->major;
		lv->minor = lp->minor;
		lv->status |= FIXED_MINOR;
		log_verbose("Setting device number to (%d, %d)", lv->major,
			    lv->minor);
	}

	if (arg_count(cmd, addtag_ARG)) {
		if (!(tag = arg_str_value(cmd, addtag_ARG, NULL))) {
			log_error("Failed to get tag");
			goto error;
		}

		if (!(lv->vg->fid->fmt->features & FMT_TAGS)) {
			log_error("Volume group %s does not support tags",
				  lv->vg->name);
			goto error;
		}

		if (!str_list_add(cmd->mem, &lv->tags, tag)) {
			log_error("Failed to add tag %s to %s/%s",
				  tag, lv->vg->name, lv->name);
			goto error;
		}
	}

	if (lp->mirrors > 1) {
		if (!create_mirror_layers(ah, 0, lp->mirrors, lv,
					  lp->segtype, 0,
					  lp->region_size, log_lv)) {
			stack;
			goto error;
		}

		alloc_destroy(ah);
		ah = NULL;
	} else if (!lv_extend(lv, lp->segtype, lp->stripes, lp->stripe_size,
		       lp->mirrors, lp->extents, NULL, 0u, 0u, pvh, lp->alloc)) {
		stack;
		return 0;
	}

	/* store vg on disk(s) */
	if (!vg_write(vg)) {
		stack;
		return 0;
	}

	backup(vg);

	if (!vg_commit(vg)) {
		stack;
		return 0;
	}

	if (!activate_lv(cmd, lv)) {
		if (lp->snapshot)
			/* FIXME Remove the failed lv we just added */
			log_error("Aborting. Failed to activate snapshot "
				  "exception store. Remove new LV and retry.");
		else
			log_error("Failed to activate new LV.");
		return 0;
	}

	if ((lp->zero || lp->snapshot) && activation()) {
		if (!zero_lv(cmd, lv) && lp->snapshot) {
			/* FIXME Remove the failed lv we just added */
			log_error("Aborting. Failed to wipe snapshot "
				  "exception store. Remove new LV and retry.");
			return 0;
		}
	} else {
		log_error("WARNING: \"%s\" not zeroed", lv->name);
		/* FIXME Remove the failed lv we just added */
	}

	if (lp->snapshot) {
		/* Reset permission after zeroing */
		if (!(lp->permission & LVM_WRITE))
			lv->status &= ~LVM_WRITE;
		if (!deactivate_lv(cmd, lv)) {
			log_err("Couldn't deactivate new snapshot.");
			return 0;
		}

		if (!vg_add_snapshot(vg->fid, NULL, org, lv, NULL,
				     org->le_count, lp->chunk_size)) {
			log_err("Couldn't create snapshot.");
			return 0;
		}

		/* store vg on disk(s) */
		if (!vg_write(vg))
			return 0;

		if (!suspend_lv(cmd, org)) {
			log_error("Failed to suspend origin %s", org->name);
			vg_revert(vg);
			return 0;
		}

		if (!vg_commit(vg))
			return 0;

		if (!resume_lv(cmd, org)) {
			log_error("Problem reactivating origin %s", org->name);
			return 0;
		}
	}
	/* FIXME out of sequence */
	backup(vg);

	log_print("Logical volume \"%s\" created", lv->name);

	/*
	 * FIXME: as a sanity check we could try reading the
	 * last block of the device ?
	 */

	return 1;

error:
	if (ah)
		alloc_destroy(ah);
	return 0;
}

int lvcreate(struct cmd_context *cmd, int argc, char **argv)
{
	int r = ECMD_FAILED;
	struct lvcreate_params lp;

	memset(&lp, 0, sizeof(lp));

	if (!_lvcreate_params(&lp, cmd, argc, argv))
		return EINVALID_CMD_LINE;

	if (!lock_vol(cmd, lp.vg_name, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", lp.vg_name);
		return ECMD_FAILED;
	}

	if (!_lvcreate(cmd, &lp)) {
		stack;
		goto out;
	}

	r = ECMD_PROCESSED;

      out:
	unlock_vg(cmd, lp.vg_name);
	return r;
}
