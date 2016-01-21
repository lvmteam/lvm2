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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"

struct lvrename_params {
	const char *lv_name_old;
	const char *lv_name_new;
};

static int _lvrename_single(struct cmd_context *cmd, const char *vg_name,
			    struct volume_group *vg, struct processing_handle *handle)
{
	struct lvrename_params *lp = (struct lvrename_params *) handle->custom_handle;
	struct lv_list *lvl;
	int ret = ECMD_FAILED;

	if (!(lvl = find_lv_in_vg(vg, lp->lv_name_old))) {
		log_error("Existing logical volume \"%s\" not found in "
			  "volume group \"%s\"", lp->lv_name_old, vg_name);
		goto bad;
	}

	if (lv_is_raid_image(lvl->lv) || lv_is_raid_metadata(lvl->lv)) {
		log_error("Cannot rename a RAID %s directly",
			  lv_is_raid_image(lvl->lv) ? "image" :
			  "metadata area");
		goto bad;
	}

	if (lv_is_raid_with_tracking(lvl->lv)) {
		log_error("Cannot rename %s while it is tracking a split image",
			  lvl->lv->name);
		goto bad;
	}

	/*
	 * The lvmlockd LV lock is only acquired here to ensure the LV is not
	 * active on another host.  This requests a transient LV lock.
	 * If the LV is active, a persistent LV lock already exists in
	 * lvmlockd, and the transient lock request does nothing.
	 * If the LV is not active, then no LV lock exists and the transient
	 * lock request acquires the LV lock (or fails).  The transient lock
	 * is automatically released when the command exits.
	 */
	if (!lockd_lv(cmd, lvl->lv, "ex", 0))
		goto_bad;

	if (!lv_rename(cmd, lvl->lv, lp->lv_name_new))
		goto_bad;

	log_print_unless_silent("Renamed \"%s\" to \"%s\" in volume group \"%s\"",
				lp->lv_name_old, lp->lv_name_new, vg_name);

	ret = ECMD_PROCESSED;
bad:
	return ret;
}

/*
 * lvrename command implementation.
 * Check arguments and call lv_rename() to execute the request.
 */
int lvrename(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle = NULL;
	struct lvrename_params lp = { 0 };
	size_t maxlen;
	char *lv_name_old, *lv_name_new;
	const char *vg_name, *vg_name_new, *vg_name_old;
	char *st;
	int ret = ECMD_FAILED;

	if (argc == 3) {
		vg_name = skip_dev_dir(cmd, argv[0], NULL);
		lv_name_old = argv[1];
		lv_name_new = argv[2];
		if (strchr(lv_name_old, '/') &&
		    (vg_name_old = extract_vgname(cmd, lv_name_old)) &&
		    strcmp(vg_name_old, vg_name)) {
			log_error("Please use a single volume group name "
				  "(\"%s\" or \"%s\")", vg_name, vg_name_old);
			return EINVALID_CMD_LINE;
		}
	} else if (argc == 2) {
		lv_name_old = argv[0];
		lv_name_new = argv[1];
		vg_name = extract_vgname(cmd, lv_name_old);
	} else {
		log_error("Old and new logical volume names required");
		return EINVALID_CMD_LINE;
	}

	if (!validate_name(vg_name)) {
		log_error("Please provide a valid volume group name");
		return EINVALID_CMD_LINE;
	}

	if (strchr(lv_name_new, '/') &&
	    (vg_name_new = extract_vgname(cmd, lv_name_new)) &&
	    strcmp(vg_name, vg_name_new)) {
		log_error("Logical volume names must "
			  "have the same volume group (\"%s\" or \"%s\")",
			  vg_name, vg_name_new);
		return EINVALID_CMD_LINE;
	}

	if ((st = strrchr(lv_name_old, '/')))
		lv_name_old = st + 1;

	if ((st = strrchr(lv_name_new, '/')))
		lv_name_new = st + 1;

	/* Check sanity of new name */
	maxlen = NAME_LEN - strlen(vg_name) - 3;
	if (strlen(lv_name_new) > maxlen) {
		log_error("New logical volume name \"%s\" may not exceed %"
			  PRIsize_t " characters.", lv_name_new, maxlen);
		return EINVALID_CMD_LINE;
	}

	if (!*lv_name_new) {
		log_error("New logical volume name may not be blank");
		return EINVALID_CMD_LINE;
	}

	if (!apply_lvname_restrictions(lv_name_new)) {
		stack;
		return EINVALID_CMD_LINE;
	}

	if (!validate_name(lv_name_new)) {
		log_error("New logical volume name \"%s\" is invalid",
			  lv_name_new);
		return EINVALID_CMD_LINE;
	}

	if (!strcmp(lv_name_old, lv_name_new)) {
		log_error("Old and new logical volume names must differ");
		return EINVALID_CMD_LINE;
	}

	if (!(lp.lv_name_old = dm_pool_strdup(cmd->mem, lv_name_old)))
		return ECMD_FAILED;

	if (!(lp.lv_name_new = dm_pool_strdup(cmd->mem, lv_name_new)))
		return ECMD_FAILED;

	if (!(handle = init_processing_handle(cmd))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &lp;

	ret = process_each_vg(cmd, 0, NULL, vg_name, READ_FOR_UPDATE, handle,
			      _lvrename_single);

	destroy_processing_handle(cmd, handle);
	return ret;

}
