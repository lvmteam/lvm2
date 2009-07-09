/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
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

int vgcreate(struct cmd_context *cmd, int argc, char **argv)
{
	struct vgcreate_params vp_new;
	struct vgcreate_params vp_def;
	struct volume_group *vg;
	const char *tag;
	const char *clustered_message = "";

	if (!argc) {
		log_error("Please provide volume group name and "
			  "physical volumes");
		return EINVALID_CMD_LINE;
	}

	if (argc == 1) {
		log_error("Please enter physical volume name(s)");
		return EINVALID_CMD_LINE;
	}

	vp_def.vg_name = NULL;
	vp_def.extent_size = DEFAULT_EXTENT_SIZE * 2;
	vp_def.max_pv = DEFAULT_MAX_PV;
	vp_def.max_lv = DEFAULT_MAX_LV;
	vp_def.alloc = DEFAULT_ALLOC_POLICY;
	vp_def.clustered = DEFAULT_CLUSTERED;
	if (fill_vg_create_params(cmd, argv[0], &vp_new, &vp_def))
		return EINVALID_CMD_LINE;

	if (validate_vg_create_params(cmd, &vp_new))
	    return EINVALID_CMD_LINE;

	/* FIXME: orphan lock needs tied to vg handle or inside library call */
	if (!lock_vol(cmd, VG_ORPHANS, LCK_VG_WRITE)) {
		log_error("Can't get lock for orphan PVs");
		return ECMD_FAILED;
	}

	/* Create the new VG */
	vg = vg_create(cmd, vp_new.vg_name);
	if (vg_read_error(vg))
		goto_bad;

	if (!vg_set_extent_size(vg, vp_new.extent_size) ||
	    !vg_set_max_lv(vg, vp_new.max_lv) ||
	    !vg_set_max_pv(vg, vp_new.max_pv) ||
	    !vg_set_alloc_policy(vg, vp_new.alloc))
		goto_bad;

	/* attach the pv's */
	if (!vg_extend(vg, argc - 1, argv + 1))
		goto_bad;

	if (vp_new.max_lv != vg->max_lv)
		log_warn("WARNING: Setting maxlogicalvolumes to %d "
			 "(0 means unlimited)", vg->max_lv);

	if (vp_new.max_pv != vg->max_pv)
		log_warn("WARNING: Setting maxphysicalvolumes to %d "
			 "(0 means unlimited)", vg->max_pv);

	if (arg_count(cmd, addtag_ARG)) {
		if (!(tag = arg_str_value(cmd, addtag_ARG, NULL))) {
			log_error("Failed to get tag");
			goto bad;
		}

		if (!(vg->fid->fmt->features & FMT_TAGS)) {
			log_error("Volume group format does not support tags");
			goto bad;
		}

		if (!str_list_add(cmd->mem, &vg->tags, tag)) {
			log_error("Failed to add tag %s to volume group %s",
				  tag, vp_new.vg_name);
			goto bad;
		}
	}

	/* FIXME: move this inside vg_create? */
	if (vp_new.clustered) {
		vg->status |= CLUSTERED;
		clustered_message = "Clustered ";
	} else {
		vg->status &= ~CLUSTERED;
		if (locking_is_clustered())
			clustered_message = "Non-clustered ";
	}

	if (!archive(vg)) {
		goto bad;
	}

	/* Store VG on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg)) {
		goto bad;
	}

	unlock_vg(cmd, vp_new.vg_name);
	unlock_vg(cmd, VG_ORPHANS);

	backup(vg);

	log_print("%s%colume group \"%s\" successfully created",
		  clustered_message, *clustered_message ? 'v' : 'V', vg->name);

	vg_release(vg);
	return ECMD_PROCESSED;

bad:
	vg_release(vg);
	unlock_vg(cmd, vp_new.vg_name);
	unlock_vg(cmd, VG_ORPHANS);
	return ECMD_FAILED;
}
