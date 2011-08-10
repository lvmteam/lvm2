/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2005 Red Hat, Inc. All rights reserved.
 * Copyright (C) 2005 Zak Kipling. All rights reserved.
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
#include "metadata.h"

struct pvresize_params {
	uint64_t new_size;

	unsigned done;
	unsigned total;
};

static int _pv_resize_single(struct cmd_context *cmd,
			     struct volume_group *vg,
			     struct physical_volume *pv,
			     const uint64_t new_size)
{
	struct pv_list *pvl;
	uint64_t size = 0;
	int r = 0;
	const char *pv_name = pv_dev_name(pv);
	const char *vg_name = pv_vg_name(pv);
	struct volume_group *old_vg = vg;
	int vg_needs_pv_write = 0;

	if (is_orphan_vg(vg_name)) {
		if (!lock_vol(cmd, vg_name, LCK_VG_WRITE)) {
			log_error("Can't get lock for orphans");
			return 0;
		}

		if (!(pv = pv_read(cmd, pv_name, 1, 0))) {
			unlock_vg(cmd, vg_name);
			log_error("Unable to read PV \"%s\"", pv_name);
			return 0;
		}
	} else {
		vg = vg_read_for_update(cmd, vg_name, NULL, 0);

		if (vg_read_error(vg)) {
			release_vg(vg);
			log_error("Unable to read volume group \"%s\".",
				  vg_name);
			return 0;
		}

		if (!(pvl = find_pv_in_vg(vg, pv_name))) {
			log_error("Unable to find \"%s\" in volume group \"%s\"",
				  pv_name, vg->name);
			goto out;
		}

		pv = pvl->pv;

		if (!archive(vg))
			goto out;
	}

	if (!(pv->fmt->features & FMT_RESIZE_PV)) {
		log_error("Physical volume %s format does not support resizing.",
			  pv_name);
		goto out;
	}

	/* Get new size */
	if (!dev_get_size(pv_dev(pv), &size)) {
		log_error("%s: Couldn't get size.", pv_name);
		goto out;
	}

	if (new_size) {
		if (new_size > size)
			log_warn("WARNING: %s: Overriding real size. "
				  "You could lose data.", pv_name);
		log_verbose("%s: Pretending size is %" PRIu64 " not %" PRIu64
			    " sectors.", pv_name, new_size, pv_size(pv));
		size = new_size;
	}

	log_verbose("Resizing volume \"%s\" to %" PRIu64 " sectors.",
		    pv_name, pv_size(pv));

	if (!pv_resize(pv, vg, size))
		goto_out;

	log_verbose("Updating physical volume \"%s\"", pv_name);

	/* Write PV label only if this an orphan PV or it has 2nd mda. */
	if ((is_orphan_vg(vg_name) ||
	     (vg_needs_pv_write = (fid_get_mda_indexed(vg->fid,
			(const char *) &pv->id, ID_LEN, 1) != NULL))) &&
	    !pv_write(cmd, pv, 1)) {
		log_error("Failed to store physical volume \"%s\"",
			  pv_name);
		goto out;
	}

	if (!is_orphan_vg(vg_name)) {
		if (!vg_write(vg) || !vg_commit(vg)) {
			log_error("Failed to store physical volume \"%s\" in "
				  "volume group \"%s\"", pv_name, vg_name);
			goto out;
		}
		backup(vg);
	}

	log_print("Physical volume \"%s\" changed", pv_name);
	r = 1;

out:
	if (!r && vg_needs_pv_write)
		log_error("Use pvcreate and vgcfgrestore "
			  "to repair from archived metadata.");
	unlock_vg(cmd, vg_name);
	if (is_orphan_vg(vg_name))
		free_pv_fid(pv);
	if (!old_vg)
		release_vg(vg);
	return r;
}

static int _pvresize_single(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct physical_volume *pv,
			    void *handle)
{
	struct pvresize_params *params = (struct pvresize_params *) handle;

	params->total++;

	if (!_pv_resize_single(cmd, vg, pv, params->new_size)) {
		stack;
		return ECMD_FAILED;
	}
	
	params->done++;

	return ECMD_PROCESSED;
}

int pvresize(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvresize_params params;
	int ret;

	if (!argc) {
		log_error("Please supply physical volume(s)");
		return EINVALID_CMD_LINE;
	}

	if (arg_sign_value(cmd, physicalvolumesize_ARG, 0) == SIGN_MINUS) {
		log_error("Physical volume size may not be negative");
		return 0;
	}

	params.new_size = arg_uint64_value(cmd, physicalvolumesize_ARG,
					   UINT64_C(0));

	params.done = 0;
	params.total = 0;

	ret = process_each_pv(cmd, argc, argv, NULL, READ_FOR_UPDATE, 0, &params,
			      _pvresize_single);

	log_print("%d physical volume(s) resized / %d physical volume(s) "
		  "not resized", params.done, params.total - params.done);

	return ret;
}
