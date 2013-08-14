/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
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

#include "lvmetad.h"
#include "lvmcache.h"

int pv_max_name_len = 0;
int vg_max_name_len = 0;

static void _pvscan_display_single(struct cmd_context *cmd,
				   struct physical_volume *pv,
				   void *handle __attribute__((unused)))
{
	char uuid[64] __attribute__((aligned(8)));
	unsigned vg_name_len = 0;

	char pv_tmp_name[NAME_LEN] = { 0 };
	char vg_tmp_name[NAME_LEN] = { 0 };
	char vg_name_this[NAME_LEN] = { 0 };

	/* short listing? */
	if (arg_count(cmd, short_ARG) > 0) {
		log_print_unless_silent("%s", pv_dev_name(pv));
		return;
	}

	if (arg_count(cmd, verbose_ARG) > 1) {
		/* FIXME As per pv_display! Drop through for now. */
		/* pv_show(pv); */

		/* FIXME - Moved to Volume Group structure */
		/* log_print("System Id             %s", pv->vg->system_id); */

		/* log_print(" "); */
		/* return; */
	}

	vg_name_len = strlen(pv_vg_name(pv)) + 1;

	if (arg_count(cmd, uuid_ARG)) {
		if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
			stack;
			return;
		}

		sprintf(pv_tmp_name, "%-*s with UUID %s",
			pv_max_name_len - 2, pv_dev_name(pv), uuid);
	} else {
		sprintf(pv_tmp_name, "%s", pv_dev_name(pv));
	}

	if (is_orphan(pv)) {
		log_print_unless_silent("PV %-*s    %-*s %s [%s]",
					pv_max_name_len, pv_tmp_name,
					vg_max_name_len, " ",
					pv->fmt ? pv->fmt->name : "    ",
					display_size(cmd, pv_size(pv)));
		return;
	}

	if (pv_status(pv) & EXPORTED_VG) {
		strncpy(vg_name_this, pv_vg_name(pv), vg_name_len);
		log_print_unless_silent("PV %-*s  is in exported VG %s "
					"[%s / %s free]",
					pv_max_name_len, pv_tmp_name,
					vg_name_this,
					display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
					display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
		return;
	}

	sprintf(vg_tmp_name, "%s", pv_vg_name(pv));
	log_print_unless_silent("PV %-*s VG %-*s %s [%s / %s free]", pv_max_name_len,
				pv_tmp_name, vg_max_name_len, vg_tmp_name,
				pv->fmt ? pv->fmt->name : "    ",
				display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
				display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
}

static int _auto_activation_handler(struct cmd_context *cmd,
				    const char *vgid, int partial,
				    activation_change_t activate)
{
	struct volume_group *vg;
	int consistent = 0;
	struct id vgid_raw;
	int r = 0;

	/* TODO: add support for partial and clustered VGs */
	if (partial)
		return 1;

	if (!id_read_format(&vgid_raw, vgid))
		return_0;

	/* NB. This is safe because we know lvmetad is running and we won't hit disk. */
	if (!(vg = vg_read_internal(cmd, NULL, (const char *) &vgid_raw, 0, &consistent)))
	    return 1;

	if (vg_is_clustered(vg)) {
		r = 1; goto out;
	}

	if (!vg_refresh_visible(vg->cmd, vg)) {
		log_error("%s: refresh before autoactivation failed.", vg->name);
		goto out;
	}

	if (!vgchange_activate(vg->cmd, vg, activate)) {
		log_error("%s: autoactivation failed.", vg->name);
		goto out;
	}

	r = 1;

out:
	release_vg(vg);
	return r;
}

static int _pvscan_lvmetad(struct cmd_context *cmd, int argc, char **argv)
{
	int ret = ECMD_PROCESSED;
	struct device *dev;
	const char *pv_name;
	int32_t major = -1;
	int32_t minor = -1;
	int devno_args = 0;
	struct arg_value_group_list *current_group;
	dev_t devno;
	char *buf;
	activation_handler handler = NULL;

	/*
	 * Return here immediately if lvmetad is not used.
	 * Also return if locking_type=3 (clustered) as we
	 * dont't support cluster + lvmetad yet.
	 *
	 * This is to avoid taking the global lock uselessly
	 * and to prevent hangs in clustered environment.
	 */
	/* TODO: Remove this once lvmetad + cluster supported! */
	if (find_config_tree_int(cmd, global_locking_type_CFG, NULL) == 3 ||
	    !find_config_tree_bool(cmd, global_use_lvmetad_CFG, NULL)) {
		log_debug_lvmetad("_pvscan_lvmetad: immediate return");
		return ret;
	}

	if (arg_count(cmd, activate_ARG)) {
		if (arg_uint_value(cmd, activate_ARG, CHANGE_AAY) != CHANGE_AAY) {
			log_error("Only --activate ay allowed with pvscan.");
			return 0;
		}
		handler = _auto_activation_handler;
	}

	if (arg_count(cmd, major_ARG) + arg_count(cmd, minor_ARG))
		devno_args = 1;

	if (devno_args && (!arg_count(cmd, major_ARG) || !arg_count(cmd, minor_ARG))) {
		log_error("Both --major and --minor required to identify devices.");
		return EINVALID_CMD_LINE;
	}
	
	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_READ, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	/* Scan everything? */
	if (!argc && !devno_args) {
		if (!lvmetad_pvscan_all_devs(cmd, handler))
			ret = ECMD_FAILED;
		goto out;
	}

	log_verbose("Using physical volume(s) on command line");

	/* Process any command line PVs first. */
	while (argc--) {
		pv_name = *argv++;
		dev = dev_cache_get(pv_name, cmd->lvmetad_filter);
		if (!dev) {
			log_error("Physical Volume %s not found.", pv_name);
			ret = ECMD_FAILED;
			continue;
		}
		if (sigint_caught()) {
			ret = ECMD_FAILED;
			stack;
			break;
		}
		if (!lvmetad_pvscan_single(cmd, dev, handler)) {
			ret = ECMD_FAILED;
			stack;
			break;
		}
	}

	if (!devno_args)
		goto out;

	/* Process any grouped --major --minor args */
	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		major = grouped_arg_int_value(current_group->arg_values, major_ARG, major);
		minor = grouped_arg_int_value(current_group->arg_values, minor_ARG, minor);

		if (major < 0 || minor < 0)
			continue;

		devno = MKDEV((dev_t)major, minor);

		if (!(dev = dev_cache_get_by_devt(devno, cmd->lvmetad_filter))) {
			if (!dm_asprintf(&buf, "%" PRIi32 ":%" PRIi32, major, minor))
				stack;
			if (!lvmetad_pv_gone(devno, buf ? : "", handler)) {
				ret = ECMD_FAILED;
				if (buf)
					dm_free(buf);
				break;
			}

			log_print_unless_silent("Device %s not found. "
						"Cleared from lvmetad cache.", buf ? : "");
			if (buf)
				dm_free(buf);
			continue;
		}
		if (sigint_caught()) {
			ret = ECMD_FAILED;
			stack;
			break;
		}
		if (!lvmetad_pvscan_single(cmd, dev, handler)) {
			ret = ECMD_FAILED;
			stack;
			break;
		}

	}

out:
	sync_local_dev_names(cmd);
	unlock_vg(cmd, VG_GLOBAL);

	return ret;
}

int pvscan(struct cmd_context *cmd, int argc, char **argv)
{
	int new_pvs_found = 0;
	int pvs_found = 0;

	struct dm_list *pvslist;
	struct pv_list *pvl;
	struct physical_volume *pv;

	uint64_t size_total = 0;
	uint64_t size_new = 0;

	int len = 0;
	pv_max_name_len = 0;
	vg_max_name_len = 0;

	if (arg_count(cmd, cache_ARG))
		return _pvscan_lvmetad(cmd, argc, argv);

	if (arg_count(cmd, activate_ARG)) {
		log_error("--activate is only valid with --cache.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, major_ARG) + arg_count(cmd, minor_ARG)) {
		log_error("--major and --minor are only valid with --cache.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, novolumegroup_ARG) && arg_count(cmd, exported_ARG)) {
		log_error("Options -e and -n are incompatible");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, exported_ARG) || arg_count(cmd, novolumegroup_ARG))
		log_warn("WARNING: only considering physical volumes %s",
			  arg_count(cmd, exported_ARG) ?
			  "of exported volume group(s)" : "in no volume group");

	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	if (cmd->filter->wipe)
		cmd->filter->wipe(cmd->filter);
	lvmcache_destroy(cmd, 1);

	/* populate lvmcache */
	if (!lvmetad_vg_list_to_lvmcache(cmd))
		stack;

	log_verbose("Walking through all physical volumes");
	if (!(pvslist = get_pvs(cmd))) {
		unlock_vg(cmd, VG_GLOBAL);
		return_ECMD_FAILED;
	}

	/* eliminate exported/new if required */
	dm_list_iterate_items(pvl, pvslist) {
		pv = pvl->pv;

		if ((arg_count(cmd, exported_ARG)
		     && !(pv_status(pv) & EXPORTED_VG)) ||
		    (arg_count(cmd, novolumegroup_ARG) && (!is_orphan(pv)))) {
			dm_list_del(&pvl->list);
			free_pv_fid(pv);
			continue;
		}

		/* Also check for MD use? */
/*******
		if (MAJOR(pv_create_kdev_t(pv[p]->pv_name)) != MD_MAJOR) {
			log_warn
			    ("WARNING: physical volume \"%s\" belongs to a meta device",
			     pv[p]->pv_name);
		}
		if (MAJOR(pv[p]->pv_dev) != MD_MAJOR)
			continue;
********/
		pvs_found++;

		if (is_orphan(pv)) {
			new_pvs_found++;
			size_new += pv_size(pv);
			size_total += pv_size(pv);
		} else
			size_total += (uint64_t) pv_pe_count(pv) * pv_pe_size(pv);
	}

	/* find maximum pv name length */
	pv_max_name_len = vg_max_name_len = 0;
	dm_list_iterate_items(pvl, pvslist) {
		pv = pvl->pv;
		len = strlen(pv_dev_name(pv));
		if (pv_max_name_len < len)
			pv_max_name_len = len;
		len = strlen(pv_vg_name(pv));
		if (vg_max_name_len < len)
			vg_max_name_len = len;
	}
	pv_max_name_len += 2;
	vg_max_name_len += 2;

	dm_list_iterate_items(pvl, pvslist) {
		_pvscan_display_single(cmd, pvl->pv, NULL);
		free_pv_fid(pvl->pv);
	}

	if (!pvs_found) {
		log_print_unless_silent("No matching physical volumes found");
		unlock_vg(cmd, VG_GLOBAL);
		return ECMD_PROCESSED;
	}

	log_print_unless_silent("Total: %d [%s] / in use: %d [%s] / in no VG: %d [%s]",
				pvs_found,
				display_size(cmd, size_total),
				pvs_found - new_pvs_found,
				display_size(cmd, (size_total - size_new)),
				new_pvs_found, display_size(cmd, size_new));

	unlock_vg(cmd, VG_GLOBAL);

	return ECMD_PROCESSED;
}
