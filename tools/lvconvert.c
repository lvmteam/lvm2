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

struct lvconvert_params {
	const char *lv_name;
	uint32_t mirrors;

	alloc_policy_t alloc;

	int pv_count;
	char **pvs;
	struct list *pvh;
};

static int _read_params(struct lvconvert_params *lp, struct cmd_context *cmd,
			int argc, char **argv)
{
	memset(lp, 0, sizeof(*lp));

	lp->alloc = ALLOC_INHERIT;
	if (arg_count(cmd, alloc_ARG))
		lp->alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG,
							    lp->alloc);

	if (!arg_count(cmd, mirrors_ARG)) {
		log_error("--mirrors argument required");
		return 0;
	}

	lp->mirrors = arg_uint_value(cmd, mirrors_ARG, 0) + 1;

	if (!argc) {
		log_error("Please give logical volume path");
		return 0;
	}

	lp->lv_name = argv[0];
	argv++, argc--;

	lp->pv_count = argc;
	lp->pvs = argv;

	return 1;
}

static int lvconvert_mirrors(struct cmd_context * cmd, struct logical_volume * lv,
			     struct lvconvert_params *lp)
{
	struct lv_segment *first_seg;
	uint32_t existing_mirrors;
	// struct alloc_handle *ah = NULL;
	// struct logical_volume *log_lv;

	if ((lp->mirrors == 1)) {
		if (!(lv->status & MIRRORED)) {
			log_error("Logical volume %s is already not mirrored.",
				  lv->name);
			return 1;
		}
		/* FIXME If allocatable_pvs supplied only remove those */
		if (!remove_all_mirror_images(lv)) {
			stack;
			return 0;
		}
	} else {		/* mirrors > 1 */
		if ((lv->status & MIRRORED)) {
			if (list_size(&lv->segments) != 1) {
				log_error("Logical volume %s has multiple "
					  "mirror segments.", lv->name);
				return 0;
			}
			list_iterate_items(first_seg, &lv->segments)
				break;
			existing_mirrors = first_seg->area_count;
			if (lp->mirrors == existing_mirrors) {
				log_error("Logical volume %s already has %"
					  PRIu32 " mirror(s).", lv->name,
					  lp->mirrors - 1);
				return 1;
			}
			if (lp->mirrors > existing_mirrors) {
				/* FIXME Unless anywhere, remove PV of log_lv 
				 * from allocatable_pvs & allocate 
				 * (mirrors - existing_mirrors) new areas
				 */
				/* FIXME Create mirror hierarchy to sync */
				log_error("Adding mirror images is not "
					  "supported yet.");
				return 0;
			} else {
				if (!remove_mirror_images(first_seg, lp->mirrors)) {
					stack;
					return 0;
				}
			}
		} else {
			/* FIXME Share code with lvcreate */
			/* region size, log_name, create log_lv, zero it */
			// Allocate (mirrors) new areas & log - replace mirrored_pv with mirrored_lv
			// Restructure as mirror - add existing param to create_mirror_layers
			log_error("Adding mirror images is not supported yet.");
			return 0;
		}
	}

	log_very_verbose("Updating logical volume \"%s\" on disk(s)", lv->name);

	if (!vg_write(lv->vg)) {
		stack;
		return 0;
	}

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
	}

	return ECMD_PROCESSED;
}

int lvconvert(struct cmd_context * cmd, int argc, char **argv)
{
	const char *vg_name;
	char *st;
	int consistent = 1;
	struct volume_group *vg;
	struct lv_list *lvl;
	struct lvconvert_params lp;
	int ret = ECMD_FAILED;

	if (!_read_params(&lp, cmd, argc, argv)) {
		stack;
		return EINVALID_CMD_LINE;
	}

	vg_name = extract_vgname(cmd, lp.lv_name);

	if (!validate_name(vg_name)) {
		log_error("Please provide a valid volume group name");
		return EINVALID_CMD_LINE;
	}

	if ((st = strrchr(lp.lv_name, '/')))
		lp.lv_name = st + 1;

	log_verbose("Checking for existing volume group \"%s\"", vg_name);

	if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
		log_error("Can't get lock for %s", vg_name);
		return ECMD_FAILED;
	}

	if (!(vg = vg_read(cmd, vg_name, &consistent))) {
		log_error("Volume group \"%s\" doesn't exist", vg_name);
		goto error;
	}

	if (vg->status & EXPORTED_VG) {
		log_error("Volume group \"%s\" is exported", vg_name);
		goto error;
	}

	if (!(vg->status & LVM_WRITE)) {
		log_error("Volume group \"%s\" is read-only", vg_name);
		goto error;
	}

	if (!(lvl = find_lv_in_vg(vg, lp.lv_name))) {
		log_error("Logical volume \"%s\" not found in "
			  "volume group \"%s\"", lp.lv_name, vg_name);
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
	unlock_vg(cmd, vg_name);
	return ret;
}
