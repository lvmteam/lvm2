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

struct pvscan_params {
	int new_pvs_found;
	int pvs_found;
	uint64_t size_total;
	uint64_t size_new;
	unsigned pv_max_name_len;
	unsigned vg_max_name_len;
	unsigned pv_tmp_namelen;
	char *pv_tmp_name;
};

static int _pvscan_display_single(struct cmd_context *cmd,
				  struct physical_volume *pv,
				  struct pvscan_params *params)
{
	/* XXXXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXXXX */
	char uuid[40] __attribute__((aligned(8)));
	const unsigned suffix_len = sizeof(uuid) + 10;
	unsigned pv_len;
	const char *pvdevname = pv_dev_name(pv);

	/* short listing? */
	if (arg_count(cmd, short_ARG) > 0) {
		log_print_unless_silent("%s", pvdevname);
		return ECMD_PROCESSED;
	}

	if (!params->pv_max_name_len) {
		lvmcache_get_max_name_lengths(cmd, &params->pv_max_name_len, &params->vg_max_name_len);

		params->pv_max_name_len += 2;
		params->vg_max_name_len += 2;
		params->pv_tmp_namelen = params->pv_max_name_len + suffix_len;

		if (!(params->pv_tmp_name = dm_pool_alloc(cmd->mem, params->pv_tmp_namelen)))
			return ECMD_FAILED;
	}

	pv_len = params->pv_max_name_len;
	memset(params->pv_tmp_name, 0, params->pv_tmp_namelen);

	if (arg_count(cmd, uuid_ARG)) {
		if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
			stack;
			return ECMD_FAILED;
		}

		if (dm_snprintf(params->pv_tmp_name, params->pv_tmp_namelen, "%-*s with UUID %s",
				params->pv_max_name_len - 2, pvdevname, uuid) < 0) {
			log_error("Invalid PV name with uuid.");
			return ECMD_FAILED;
		}
		pvdevname = params->pv_tmp_name;
		pv_len += suffix_len;
	}

	if (is_orphan(pv))
		log_print_unless_silent("PV %-*s    %-*s %s [%s]",
					pv_len, pvdevname,
					params->vg_max_name_len, " ",
					pv->fmt ? pv->fmt->name : "    ",
					display_size(cmd, pv_size(pv)));
	else if (pv_status(pv) & EXPORTED_VG)
		log_print_unless_silent("PV %-*s  is in exported VG %s [%s / %s free]",
					pv_len, pvdevname, pv_vg_name(pv),
					display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
					display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
	else
		log_print_unless_silent("PV %-*s VG %-*s %s [%s / %s free]",
					pv_len, pvdevname,
					params->vg_max_name_len, pv_vg_name(pv),
					pv->fmt ? pv->fmt->name : "    ",
					display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
					display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
	return ECMD_PROCESSED;
}

static int _pvscan_single(struct cmd_context *cmd, struct volume_group *vg,
			  struct physical_volume *pv, struct processing_handle *handle)
{
	struct pvscan_params *params = (struct pvscan_params *)handle->custom_handle;

	if ((arg_count(cmd, exported_ARG) && !(pv_status(pv) & EXPORTED_VG)) ||
	    (arg_count(cmd, novolumegroup_ARG) && (!is_orphan(pv)))) {
		return ECMD_PROCESSED;

	}

	params->pvs_found++;

	if (is_orphan(pv)) {
		params->new_pvs_found++;
		params->size_new += pv_size(pv);
		params->size_total += pv_size(pv);
	} else {
		params->size_total += (uint64_t) pv_pe_count(pv) * pv_pe_size(pv);
	}

	_pvscan_display_single(cmd, pv, params);
	return ECMD_PROCESSED;
}

#define REFRESH_BEFORE_AUTOACTIVATION_RETRIES 5
#define REFRESH_BEFORE_AUTOACTIVATION_RETRY_USLEEP_DELAY 100000

static int _auto_activation_handler(struct cmd_context *cmd,
				    const char *vgname, const char *vgid,
				    int partial, int changed,
				    activation_change_t activate)
{
	unsigned int refresh_retries = REFRESH_BEFORE_AUTOACTIVATION_RETRIES;
	int refresh_done = 0;
	struct volume_group *vg;
	struct id vgid_raw;
	int r = 0;

	/* TODO: add support for partial and clustered VGs */
	if (partial)
		return 1;

	if (!id_read_format(&vgid_raw, vgid))
		return_0;

	/* NB. This is safe because we know lvmetad is running and we won't hit disk. */
	vg = vg_read(cmd, vgname, (const char *)&vgid_raw, 0, 0);
	if (vg_read_error(vg)) {
		log_error("Failed to read Volume Group \"%s\" (%s) during autoactivation.", vgname, vgid);
		release_vg(vg);
		return 0;
	}

	if (vg_is_clustered(vg)) {
		r = 1; goto out;
	}

	/* FIXME: There's a tiny race when suspending the device which is part
	 * of the refresh because when suspend ioctl is performed, the dm
	 * kernel driver executes (do_suspend and dm_suspend kernel fn):
	 *
	 *          step 1: a check whether the dev is already suspended and
	 *                  if yes it returns success immediately as there's
	 *                  nothing to do
	 *          step 2: it grabs the suspend lock
	 *          step 3: another check whether the dev is already suspended
	 *                  and if found suspended, it exits with -EINVAL now
	 *
	 * The race can occur in between step 1 and step 2. To prevent premature
	 * autoactivation failure, we're using a simple retry logic here before
	 * we fail completely. For a complete solution, we need to fix the
	 * locking so there's no possibility for suspend calls to interleave
	 * each other to cause this kind of race.
	 *
	 * Remove this workaround with "refresh_retries" once we have proper locking in!
	 */
	if (changed) {
		while (refresh_retries--) {
			if (vg_refresh_visible(vg->cmd, vg)) {
				refresh_done = 1;
				break;
			}
			usleep(REFRESH_BEFORE_AUTOACTIVATION_RETRY_USLEEP_DELAY);
		}

		if (!refresh_done)
			log_warn("%s: refresh before autoactivation failed.", vg->name);
	}

	if (!vgchange_activate(vg->cmd, vg, activate)) {
		log_error("%s: autoactivation failed.", vg->name);
		goto out;
	}

	r = 1;

out:
	unlock_and_release_vg(cmd, vg, vgname);
	return r;
}

static int _clear_dev_from_lvmetad_cache(dev_t devno, int32_t major, int32_t minor,
					 activation_handler handler)
{
	char buf[24];

	(void) dm_snprintf(buf, sizeof(buf), FMTi32 ":" FMTi32, major, minor);

	if (!lvmetad_pv_gone(devno, buf, handler))
		return_0;

	log_print_unless_silent("Device %s not found. "
				"Cleared from lvmetad cache.", buf);

	return 1;
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
	activation_handler handler = NULL;

	cmd->include_foreign_vgs = 1;

	/*
	 * Return here immediately if lvmetad is not used.
	 * Also return if locking_type=3 (clustered) as we
	 * dont't support cluster + lvmetad yet.
	 *
	 * This is to avoid taking the global lock uselessly
	 * and to prevent hangs in clustered environment.
	 */
	/* TODO: Remove this once lvmetad + cluster supported! */
	if (!lvmetad_used()) {
		log_verbose("Ignoring pvscan --cache command because lvmetad is not in use.");
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
		if (pv_name[0] == '/') {
			/* device path */
			if (!(dev = dev_cache_get(pv_name, cmd->lvmetad_filter))) {
				if ((dev = dev_cache_get(pv_name, NULL))) {
					if (!_clear_dev_from_lvmetad_cache(dev->dev, MAJOR(dev->dev), MINOR(dev->dev), handler)) {
						stack;
						ret = ECMD_FAILED;
						break;
					}
				} else {
					log_error("Physical Volume %s not found.", pv_name);
					ret = ECMD_FAILED;
					break;
				}
				continue;
			}
		}
		else {
			/* device major:minor */
			if (sscanf(pv_name, "%d:%d", &major, &minor) != 2) {
				log_error("Failed to parse major:minor from %s", pv_name);
				ret = ECMD_FAILED;
				continue;
			}
			devno = MKDEV((dev_t)major, (dev_t)minor);
			if (!(dev = dev_cache_get_by_devt(devno, cmd->lvmetad_filter))) {
				if (!(_clear_dev_from_lvmetad_cache(devno, major, minor, handler))) {
					stack;
					ret = ECMD_FAILED;
					break;
				}
				continue;
			}
		}
		if (sigint_caught()) {
			ret = ECMD_FAILED;
			stack;
			break;
		}
		if (!lvmetad_pvscan_single(cmd, dev, handler, 0)) {
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

		devno = MKDEV((dev_t)major, (dev_t)minor);

		if (!(dev = dev_cache_get_by_devt(devno, cmd->lvmetad_filter))) {
			if (!(_clear_dev_from_lvmetad_cache(devno, major, minor, handler))) {
				stack;
				ret = ECMD_FAILED;
				break;
			}
			continue;
		}
		if (sigint_caught()) {
			ret = ECMD_FAILED;
			stack;
			break;
		}
		if (!lvmetad_pvscan_single(cmd, dev, handler, 0)) {
			ret = ECMD_FAILED;
			stack;
			break;
		}

	}

out:
	if (!sync_local_dev_names(cmd))
		stack;
	unlock_vg(cmd, VG_GLOBAL);
	return ret;
}

int pvscan(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_params params = { 0 };
	struct processing_handle *handle = NULL;
	int ret;

	if (arg_count(cmd, cache_long_ARG))
		return _pvscan_lvmetad(cmd, argc, argv);

	if (argc) {
		log_error("Too many parameters on command line.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, activate_ARG)) {
		log_error("--activate is only valid with --cache.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, major_ARG) || arg_count(cmd, minor_ARG)) {
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

	/* Needed for a current listing of the global VG namespace. */
	if (!lockd_gl(cmd, "sh", 0))
		return_ECMD_FAILED;

	if (cmd->full_filter->wipe)
		cmd->full_filter->wipe(cmd->full_filter);

	lvmcache_destroy(cmd, 1, 0);

	if (!(handle = init_processing_handle(cmd))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
	}

	handle->custom_handle = &params;

	ret = process_each_pv(cmd, argc, argv, NULL, 0, handle, _pvscan_single);

	if (!params.pvs_found)
		log_print_unless_silent("No matching physical volumes found");
	else
		log_print_unless_silent("Total: %d [%s] / in use: %d [%s] / in no VG: %d [%s]",
					params.pvs_found,
					display_size(cmd, params.size_total),
					params.pvs_found - params.new_pvs_found,
					display_size(cmd, (params.size_total - params.size_new)),
					params.new_pvs_found, display_size(cmd, params.size_new));

out:
	unlock_vg(cmd, VG_GLOBAL);

	return ret;
}
