/*
 * Copyright (C) 2001  Sistina Software
 *
 * This file is released under the GPL.
 *
 */

#include "tools.h"

#include <fcntl.h>

struct lvcreate_params {
	/* flags */
	int snapshot;
	int zero;
	int contiguous;
	int minor;

	char *origin;
	char *vg_name;
	char *lv_name;

	uint32_t stripes;
	uint32_t stripe_size;
	uint32_t chunk_size;

	/* size */
	uint32_t extents;
	uint64_t size;

	uint32_t permission;
	uint32_t read_ahead;

	int pv_count;
	char **pvs;
};


static int _read_name_params(struct lvcreate_params *lp,
			     struct cmd_context *cmd,
			     int *pargc, char ***pargv)
{
	int argc = *pargc;
	char **argv = *pargv, *ptr;

	if (arg_count(cmd, name_ARG))
		lp->lv_name = arg_value(cmd, name_ARG);

	if (arg_count(cmd, snapshot_ARG)) {
		lp->snapshot = 1;

		if (!argc) {
			log_err("Please specify a logical volume to act as "
				"the snapshot origin.");
			return 0;
		}

		lp->origin = argv[0];
		(*pargv)++, (*pargc)--;
		if (!(lp->vg_name = extract_vgname(cmd->fid, lp->origin))) {
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
			if (!(lp->vg_name =
			      extract_vgname(cmd->fid, lp->lv_name))) {
				log_err("Please provide a volume group name");
				return 0;
			}

		} else {
			/*
			 * Ensure lv_name doesn't contain a
			 * different VG.
			 */
			if (lp->lv_name && strchr(lp->lv_name, '/')) {
				if (!(lp->vg_name =
				      extract_vgname(cmd->fid, lp->lv_name)))
					return 0;

				if (strcmp(lp->vg_name, argv[0])) {
					log_error("Inconsistent volume group "
						  "names "
						  "given: \"%s\" and \"%s\"",
						  lp->vg_name, argv[0]);
					return 0;
				}
			}

			lp->vg_name = argv[0];
			(*pargv)++, (*pargc)--;
		}
	}

	if (lp->lv_name && (ptr = strrchr(lp->lv_name, '/')))
		lp->lv_name = ptr + 1;

	return 1;
}

static int _read_size_params(struct lvcreate_params *lp,
			     struct cmd_context *cmd,
			     int *pargc, char ***pargv)
{
	/*
	 * There are two mutually exclusive ways of specifying
	 * the size ...
	 */
	if (arg_count(cmd, extents_ARG) && arg_count(cmd, size_ARG)) {
		log_error("Invalid combination of arguments");
		return 0;
	}

	/*
	 * ... you must use one of them.
	 */
	if (arg_count(cmd, size_ARG) + arg_count(cmd, extents_ARG) == 0) {
		log_error("Please indicate size using option -l or -L");
		return 0;
	}

	if (arg_count(cmd, extents_ARG))
		lp->extents = arg_int_value(cmd, extents_ARG, 0);

	/* Size returned in kilobyte units; held in sectors */
	if (arg_count(cmd, size_ARG))
		lp->size = arg_int64_value(cmd, size_ARG, 0) * 2ull;

	return 1;
}

static int _read_stripe_params(struct lvcreate_params *lp,
			       struct cmd_context *cmd,
			       int *pargc, char ***pargv)
{
	int argc = *pargc;

	lp->stripes = 1;

	if (arg_count(cmd, stripes_ARG)) {
		lp->stripes = arg_int_value(cmd, stripes_ARG, 1);
		if (lp->stripes == 1)
			log_print("Redundant stripes argument: default is 1");
	}

	if (arg_count(cmd, stripesize_ARG))
		lp->stripe_size = 2 * arg_int_value(cmd, stripesize_ARG, 0);

	if (lp->stripes == 1 && lp->stripe_size) {
		log_print("Ignoring stripesize argument with single stripe");
		lp->stripe_size = 0;
	}

	if (lp->stripes > 1 && !lp->stripe_size) {
		lp->stripe_size = 2 * STRIPE_SIZE_DEFAULT;
		log_print("Using default stripesize %dKB",
			  lp->stripe_size / 2);
	}

	if (argc && argc < lp->stripes) {
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

static int _read_params(struct lvcreate_params *lp, struct cmd_context *cmd,
			int argc, char **argv)
{
	/*
	 * Set the defaults.
	 */
	memset(lp, 0, sizeof(*lp));
	lp->chunk_size = 256;

	if (!_read_name_params(lp, cmd, &argc, &argv) ||
	    !_read_size_params(lp, cmd, &argc, &argv) ||
	    !_read_stripe_params(lp, cmd, &argc, &argv))
		return 0;

	/*
	 * Should we zero the lv.
	 */
	lp->zero = strcmp(arg_str_value(cmd, zero_ARG, "y"), "n");

	/*
	 * Contiguous ?
	 */
	lp->contiguous = strcmp(arg_str_value(cmd, contiguous_ARG, "n"), "n");

	/*
	 * Read ahead.
	 */
	if (arg_count(cmd, readahead_ARG))
		lp->read_ahead = arg_int_value(cmd, readahead_ARG, 0);

	/*
	 * Permissions.
	 */
	if (arg_count(cmd, permission_ARG))
		lp->permission = arg_int_value(cmd, permission_ARG, 0);
	else
		lp->permission = LVM_READ | LVM_WRITE;

	lp->minor = arg_int_value(cmd, minor_ARG, -1);

	/* Persistent minor */
	if (arg_count(cmd, persistent_ARG)) {
		if (!strcmp(arg_str_value(cmd, persistent_ARG, "n"), "y")) {
			if (lp->minor == -1) {
				log_error("Please specify minor number with "
					  "--minor when using -My");
				return 0;
			}
		} else {
			if (lp->minor != -1) {
				log_error("--minor not possible with -Mn");
				return 0;
			}
		}
	}

	lp->pv_count = argc;
	lp->pvs = argv;

	return 1;
}

static int _lvcreate(struct cmd_context *cmd, struct lvcreate_params *lp,
		     struct logical_volume **plv)
{
	uint32_t size_rest;
	uint32_t status = 0;
	struct volume_group *vg;
	struct logical_volume *lv, *org;
	struct list *pvh;


	*plv = NULL;

	if (lp->contiguous)
		status |= ALLOC_CONTIGUOUS;
	else
		status |= ALLOC_SIMPLE;


	status |= lp->permission;

	/* does VG exist? */
	log_verbose("Finding volume group \"%s\"", lp->vg_name);

	if (!(vg = cmd->fid->ops->vg_read(cmd->fid, lp->vg_name))) {
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


	/*
	 * Create the pv list.
	 */
	if (lp->pv_count) {
		if (!(pvh = create_pv_list(cmd->mem, vg,
					   lp->pv_count, lp->pvs))) {
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
		lp->extents = lp->size;

		if (lp->extents % vg->extent_size) {
			char *s1;

			lp->extents += vg->extent_size - lp->extents %
				vg->extent_size;
			log_print("Rounding up size to full physical "
				  "extent %s",
				  (s1 = display_size(lp->extents / 2,
						     SIZE_SHORT)));
			dbg_free(s1);
		}

		lp->extents /= vg->extent_size;
	}

	if ((size_rest = lp->extents % lp->stripes)) {
		log_print("Rounding size (%d extents) up to stripe boundary "
			  "size (%d extents)", lp->extents,
			  lp->extents - size_rest + lp->stripes);
		lp->extents = lp->extents - size_rest + lp->stripes;
	}

	if (lp->snapshot && !(org = find_lv(vg, lp->origin))) {
		log_err("Couldn't find origin volume '%s'.", lp->origin);
		return 0;
	}

	if (!archive(vg))
		return 0;

	if (!(lv = lv_create(cmd->fid, lp->lv_name, status,
			     lp->stripes, lp->stripe_size, lp->extents,
			     vg, pvh)))
		return 0;

	if (lp->snapshot && !vg_add_snapshot(org, lv, 1, lp->chunk_size)) {
		log_err("Couldn't create snapshot.");
		return 0;
	}

	if (lp->read_ahead) {
		log_verbose("Setting read ahead sectors");
		lv->read_ahead = lp->read_ahead;
	}

	if (lp->minor >= 0) {
		lv->minor = lp->minor;
		lv->status |= FIXED_MINOR;
		log_verbose("Setting minor number to %d", lv->minor);
	}

	/* store vg on disk(s) */
	if (!cmd->fid->ops->vg_write(cmd->fid, vg))
		return 0;

	backup(vg);
	log_print("Logical volume \"%s\" created", lv->name);

	*plv = lv;
	return 1;
}

/*
 * Non-snapshot volumes may be zeroed to remove old filesystems.
 */
static int _zero(struct cmd_context *cmd, struct logical_volume *lv)
{
	struct device *dev;
	char *name;

	/*
	 * FIXME:
	 * <clausen> also, more than 4k
	 * <clausen> say, reiserfs puts it's superblock 32k in, IIRC
	 * <ejt_> k, I'll drop a fixme to that effect
	 *           (I know the device is at least 4k, but not 32k)
	 */
	if (!(name = pool_alloc(cmd->mem, PATH_MAX))) {
		log_error("Name allocation failed - device not zeroed");
		return 0;
	}

	if (lvm_snprintf(name, PATH_MAX, "%s%s/%s", cmd->dev_dir,
			 lv->vg->name, lv->name) < 0) {
		log_error("Name too long - device not zeroed (%s)",
			  lv->name);
		return 0;
	}

	log_verbose("Zeroing start of logical volume \"%s\"", lv->name);

	if (!(dev = dev_cache_get(name, NULL))) {
		log_error("\"%s\" not found: device not zeroed", name);
		return 0;
	}

	if (!(dev_open(dev, O_WRONLY)))
		return 0;

	dev_zero(dev, 0, 4096);
	dev_close(dev);

	return 1;
}

int lvcreate(struct cmd_context *cmd, int argc, char **argv)
{
	int r = ECMD_FAILED;
	struct lvcreate_params lp;
	struct logical_volume *lv;

	memset(&lp, 0, sizeof(lp));

	if (!_read_params(&lp, cmd, argc, argv))
		return -EINVALID_CMD_LINE;

	if (!lock_vol(cmd, lp.vg_name, LCK_VG | LCK_WRITE)) {
		log_error("Can't get lock for %s", lp.vg_name);
		return 0;
	}

	if (!_lvcreate(cmd, &lp, &lv)) {
		stack;
		goto out;
	}

	if (!lp.snapshot && !lv_setup_cow_store(lv)) {
		stack;
		goto out;
	}

	if (!lv_activate(lv))
		goto out;

	if (!lp.snapshot) {
		if (!lp.zero)
			log_print("WARNING: \"%s\" not zeroed", lv->name);

		else if (!_zero(cmd, lv)) {
			stack;
			goto out;
		}
	}

	/*
	 * FIXME: as a sanity check we could try reading the
	 * last block of the device ?
	 */

	r = 0;

 out:
	lock_vol(cmd, lp.vg_name, LCK_VG | LCK_NONE);
	return r;
}
