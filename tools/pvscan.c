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
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "tools.h"

#include "lvmetad.h"
#include "lvmcache.h"

extern int use_full_md_check;

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

struct pvscan_aa_params {
	int refresh_all;
	unsigned int activate_errors;
	struct dm_list changed_vgnames;
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
	if (arg_is_set(cmd, short_ARG)) {
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

	if (arg_is_set(cmd, uuid_ARG)) {
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

	if ((arg_is_set(cmd, exported_ARG) && !(pv_status(pv) & EXPORTED_VG)) ||
	    (arg_is_set(cmd, novolumegroup_ARG) && (!is_orphan(pv)))) {
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

static int _lvmetad_clear_dev(dev_t devno, int32_t major, int32_t minor)
{
	char buf[24];

	(void) dm_snprintf(buf, sizeof(buf), FMTd32 ":" FMTd32, major, minor);

	if (!lvmetad_pv_gone(devno, buf))
		return_0;

	log_print_unless_silent("Device %s not found. Cleared from lvmetad cache.", buf);

	return 1;
}

/*
 * pvscan --cache does not perform any lvmlockd locking, and
 * pvscan --cache -aay skips autoactivation in lockd VGs.
 *
 * pvscan --cache populates lvmetad with VG metadata from disk.
 * No lvmlockd locking is needed.  It is expected that lockd VG
 * metadata that is read by pvscan and populated in lvmetad may
 * be immediately stale due to changes to the VG from other hosts
 * during or after this pvscan.  This is normal and not a problem.
 * When a subsequent lvm command uses the VG, it will lock the VG
 * with lvmlockd, read the VG from lvmetad, and update the cached
 * copy from disk if necessary.
 *
 * pvscan --cache -aay does not activate LVs in lockd VGs because
 * activation requires locking, and a lock-start operation is needed
 * on a lockd VG before any locking can be performed in it.
 *
 * An equivalent of pvscan --cache -aay for lockd VGs is:
 * 1. pvscan --cache
 * 2. vgchange --lock-start
 * 3. vgchange -aay -S 'locktype=sanlock || locktype=dlm'
 *
 * [We could eventually add support for autoactivating lockd VGs
 * using pvscan by incorporating the lock start step (which can
 * take a long time), but there may be a better option than
 * continuing to overload pvscan.]
 * 
 * Stages of starting a lockd VG:
 *
 * . pvscan --cache populates lockd VGs in lvmetad without locks,
 *   and this initial cached copy may quickly become stale.
 *
 * . vgchange --lock-start VG reads the VG without the VG lock
 *   because no locks are available until the locking is started.
 *   It only uses the VG name and lock_type from the VG metadata,
 *   and then only uses it to start the VG lockspace in lvmlockd.
 *
 * . Further lvm commands, e.g. activation, can then lock the VG
 *   with lvmlockd and use current VG metdata.
 */

#define REFRESH_BEFORE_AUTOACTIVATION_RETRIES 5
#define REFRESH_BEFORE_AUTOACTIVATION_RETRY_USLEEP_DELAY 100000

static int _pvscan_autoactivate_single(struct cmd_context *cmd, const char *vg_name,
				       struct volume_group *vg, struct processing_handle *handle)
{
	struct pvscan_aa_params *pp = (struct pvscan_aa_params *)handle->custom_handle;
	unsigned int refresh_retries = REFRESH_BEFORE_AUTOACTIVATION_RETRIES;
	int refresh_done = 0;

	if (vg_is_clustered(vg))
		return ECMD_PROCESSED;

	if (vg_is_exported(vg))
		return ECMD_PROCESSED;

	if (is_lockd_type(vg->lock_type))
		return ECMD_PROCESSED;

	log_debug("pvscan autoactivating VG %s.", vg_name);

	/*
	 * Refresh LVs in a VG that has "changed" from finding a PV.
	 * The meaning of "changed" is determined in lvmetad, and is
	 * returned to the command as a flag.
	 *
	 * FIXME: There's a tiny race when suspending the device which is part
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
	if (pp->refresh_all || str_list_match_item(&pp->changed_vgnames, vg_name)) {
		while (refresh_retries--) {
			log_debug_activation("Refreshing VG %s before autoactivation.", vg_name);
			if (vg_refresh_visible(cmd, vg)) {
				refresh_done = 1;
				break;
			}
			usleep(REFRESH_BEFORE_AUTOACTIVATION_RETRY_USLEEP_DELAY);
		}

		if (!refresh_done)
			log_warn("%s: refresh before autoactivation failed.", vg->name);
	}

	log_debug_activation("Autoactivating VG %s.", vg_name);

	if (!vgchange_activate(cmd, vg, CHANGE_AAY)) {
		log_error("%s: autoactivation failed.", vg->name);
		pp->activate_errors++;
		goto out;
	}

	/*
	 * After sucessfull activation we need to initialise polling
	 * for all activated LVs in a VG. Possible enhancement would
	 * be adding --poll y|n cmdline option for pvscan and call
	 * init_background_polling routine in autoactivation handler.
	 */
	log_debug_activation("Starting background polling for VG %s.", vg_name);

	if (!(vgchange_background_polling(cmd, vg)))
		goto_out;
out:
	return ECMD_PROCESSED;
}

static int _pvscan_autoactivate(struct cmd_context *cmd, struct pvscan_aa_params *pp,
				int all_vgs, struct dm_list *vgnames)
{
	struct processing_handle *handle = NULL;
	int ret;

	if (!all_vgs && dm_list_empty(vgnames)) {
		log_debug("No VGs to autoactivate.");
		return ECMD_PROCESSED;
	}

	if (!lvmetad_used())
		log_warn("WARNING: Autoactivation reading from disk instead of lvmetad.");

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = pp;

	if (all_vgs) {
		cmd->cname->flags |= ALL_VGS_IS_DEFAULT;
		pp->refresh_all = 1;
	}

	ret = process_each_vg(cmd, 0, NULL, NULL, vgnames, 0, 0, handle, _pvscan_autoactivate_single);

	destroy_processing_handle(cmd, handle);

	return ret;
}

static int _pvscan_cache(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_aa_params pp = { 0 };
	struct dm_list single_devs;
	struct dm_list found_vgnames;
	struct device *dev;
	struct device_list *devl;
	struct dev_iter *iter;
	const char *pv_name;
	const char *reason = NULL;
	int32_t major = -1;
	int32_t minor = -1;
	int devno_args = 0;
	struct arg_value_group_list *current_group;
	dev_t devno;
	int do_activate;
	int all_vgs = 0;
	int remove_errors = 0;
	int add_errors = 0;
	int ret = ECMD_PROCESSED;

	dm_list_init(&found_vgnames);
	dm_list_init(&pp.changed_vgnames);

	if ((do_activate = arg_is_set(cmd, activate_ARG))) {
		if (arg_uint_value(cmd, activate_ARG, 0) != CHANGE_AAY) {
			log_error("Only --activate ay allowed with pvscan.");
			return EINVALID_CMD_LINE;
		}

		if (!lvmetad_used() &&
		    !find_config_tree_bool(cmd, global_use_lvmetad_CFG, NULL)) {
			log_verbose("Ignoring pvscan --cache -aay because lvmetad is not in use.");
			return ret;
		}
	} else {
		if (!lvmetad_used()) {
			log_verbose("Ignoring pvscan --cache because lvmetad is not in use.");
			return ret;
		}
	}

	if (arg_is_set(cmd, major_ARG) + arg_is_set(cmd, minor_ARG))
		devno_args = 1;

	if (devno_args && (!arg_is_set(cmd, major_ARG) || !arg_is_set(cmd, minor_ARG))) {
		log_error("Both --major and --minor required to identify devices.");
		return EINVALID_CMD_LINE;
	}
	
	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_READ, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	/*
	 * This a special case where use_lvmetad=1 in lvm.conf but pvscan
	 * cannot use lvmetad for some reason.  In this case pvscan should
	 * still activate LVs even though it's not updating the cache.
	 */
	if (do_activate && !lvmetad_used()) {
		log_verbose("Activating all VGs without lvmetad.");
		all_vgs = 1;
		devno_args = 0;
		goto activate;
	}

	/*
	 * Scan all devices when no args are given.
	 */
	if (!argc && !devno_args) {
		log_verbose("Scanning all devices.");

		if (!lvmetad_pvscan_all_devs(cmd, 1)) {
			log_warn("WARNING: Not using lvmetad because cache update failed.");
			lvmetad_make_unused(cmd);
		}
		if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
			log_warn("WARNING: Not using lvmetad because %s.", reason);
			lvmetad_make_unused(cmd);
		}
		all_vgs = 1;
		goto activate;
	}

	/*
	 * FIXME: when specific devs are named, we generally don't want to scan
	 * any other devs, but if lvmetad is not yet populated, the first
	 * 'pvscan --cache dev' does need to do a full scan.  We want to remove
	 * the need for this case so that 'pvscan --cache dev' is guaranteed to
	 * never scan any devices other than those specified.
	 */
	if (!lvmetad_token_matches(cmd)) {
		if (lvmetad_used() && !lvmetad_pvscan_all_devs(cmd, 0)) {
			log_warn("WARNING: Not updating lvmetad because cache update failed.");
			ret = ECMD_FAILED;
			goto out;
		}
		if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
			log_warn("WARNING: Not using lvmetad because %s.", reason);
			lvmetad_make_unused(cmd);
		}
		all_vgs = 1;
		goto activate;
	}

	/*
	 * When args are given, scan only those devices.  If lvmetad is already
	 * disabled, a full scan is required to reenable it, so there's no
	 * point in doing individual device scans, so go directly to
	 * autoactivation.  (FIXME: Should we also skip autoactivation in this
	 * case since that will read disks with lvmetad disabled?
	 * i.e. avoid disk access and not activate LVs, or or read from disk
	 * and activate LVs?)
	 */
	if (lvmetad_is_disabled(cmd, &reason)) {
		log_warn("WARNING: Not using lvmetad because %s.", reason);
		lvmetad_make_unused(cmd);
		all_vgs = 1;
		goto activate;
	}

	/*
	 * Step 1: for each device, if it's no longer found, then tell lvmetad
	 * to drop it.  If the device exists, read metadata from it and send
	 * that to lvmetad.
	 *
	 * When given a device name, check if the device is not visible to
	 * lvmetad, but still visible to the system, and if so, tell lvmetad to
	 * drop it (using the major:minor from the system).
	 *
	 * When given a major:minor which is not visible to the system, just
	 * tell lvmetad to drop it directly using that major:minor.
	 *
	 * When a device has left the system, it must be dropped using
	 * --major/--minor because we cannot map the device name to major:minor
	 *  after the device has left.  (A full rescan could of course be used
	 *  to drop any devices that have left.)
	 */

	if (argc || devno_args) {
		log_verbose("Scanning devices on command line.");
		cmd->pvscan_cache_single = 1;
	}

	/* Creates a list of dev names from /dev, sysfs, etc; does not read any. */
	dev_cache_scan();

	/* See the same check in label_scan() to handle md 0.9/1.0 components. */
	if (!(iter = dev_iter_create(cmd->full_filter, 0))) {
		log_error("Scanning failed to get devices.");
		return 0;
	}
	while ((dev = dev_iter_get(iter))) {
		if (dev_is_md_with_end_superblock(cmd->dev_types, dev)) {
			cmd->use_full_md_check = 1;
			use_full_md_check = 1;
			log_debug("Found md component in sysfs with end superblock %s", dev_name(dev));
			break;
		}
	}
	dev_iter_destroy(iter);
	if (!use_full_md_check)
		log_debug("No md devs with end superblock");

	dm_list_init(&single_devs);

	while (argc--) {
		pv_name = *argv++;
		if (pv_name[0] == '/') {
			if (!(dev = dev_cache_get(pv_name, cmd->lvmetad_filter))) {
				/* Remove device path from lvmetad. */
				log_debug("Removing dev %s from lvmetad cache.", pv_name);
				if ((dev = dev_cache_get(pv_name, NULL))) {
					if (!_lvmetad_clear_dev(dev->dev, MAJOR(dev->dev), MINOR(dev->dev)))
						remove_errors++;
				} else {
					log_error("Physical Volume %s not found.", pv_name);
					ret = ECMD_FAILED;
				}
			} else {
				/*
				 * Scan device.  This dev could still be
				 * removed from lvmetad below if it doesn't
				 * pass other filters.
				 */
				log_debug("Scanning dev %s for lvmetad cache.", pv_name);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&single_devs, &devl->list);
			}
		} else {
			if (sscanf(pv_name, "%d:%d", &major, &minor) != 2) {
				log_warn("WARNING: Failed to parse major:minor from %s, skipping.", pv_name);
				continue;
			}
			devno = MKDEV(major, minor);

			if (!(dev = dev_cache_get_by_devt(devno, cmd->lvmetad_filter))) {
				/* Remove major:minor from lvmetad. */
				log_debug("Removing dev %d:%d from lvmetad cache.", major, minor);
				if (!_lvmetad_clear_dev(devno, major, minor))
					remove_errors++;
			} else {
				/*
				 * Scan device.  This dev could still be
				 * removed from lvmetad below if it doesn't
				 * pass other filters.
				 */
				log_debug("Scanning dev %d:%d for lvmetad cache.", major, minor);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&single_devs, &devl->list);
			}
		}

		if (sigint_caught()) {
			ret = ECMD_FAILED;
			goto_out;
		}
	}

	if (!dm_list_empty(&single_devs)) {
		label_scan_devs(cmd, cmd->lvmetad_filter, &single_devs);

		dm_list_iterate_items(devl, &single_devs) {
			dev = devl->dev;

			if (dev->flags & DEV_FILTER_OUT_SCAN) {
				log_debug("Removing dev %s from lvmetad cache after scan.", dev_name(dev));
				if (!_lvmetad_clear_dev(dev->dev, MAJOR(dev->dev), MINOR(dev->dev)))
					remove_errors++;
				continue;
			}

			/*
			 * Devices that exist and pass the lvmetad filter
			 * are added to lvmetad.
			 */
			if (!lvmetad_pvscan_single(cmd, dev, &found_vgnames, &pp.changed_vgnames))
				add_errors++;
		}
	}

	if (!devno_args)
		goto activate;

	dm_list_init(&single_devs);

	/* Process any grouped --major --minor args */
	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		major = grouped_arg_int_value(current_group->arg_values, major_ARG, major);
		minor = grouped_arg_int_value(current_group->arg_values, minor_ARG, minor);

		if (major < 0 || minor < 0)
			continue;

		devno = MKDEV(major, minor);

		if (!(dev = dev_cache_get_by_devt(devno, cmd->lvmetad_filter))) {
			/* Remove major:minor from lvmetad. */
			log_debug("Removing dev %d:%d from lvmetad cache.", major, minor);
			if (!_lvmetad_clear_dev(devno, major, minor))
				remove_errors++;
		} else {
			/* Add major:minor to lvmetad. */
			log_debug("Scanning dev %d:%d for lvmetad cache.", major, minor);

			if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
				return_0;
			devl->dev = dev;
			dm_list_add(&single_devs, &devl->list);
		}

		if (sigint_caught()) {
			ret = ECMD_FAILED;
			goto_out;
		}
	}

	if (!dm_list_empty(&single_devs)) {
		label_scan_devs(cmd, cmd->lvmetad_filter, &single_devs);

		dm_list_iterate_items(devl, &single_devs) {
			dev = devl->dev;

			if (dev->flags & DEV_FILTER_OUT_SCAN) {
				log_debug("Removing dev %s from lvmetad cache after scan.", dev_name(dev));
				if (!_lvmetad_clear_dev(dev->dev, MAJOR(dev->dev), MINOR(dev->dev)))
					remove_errors++;
				continue;
			}

			/*
			 * Devices that exist and pass the lvmetad filter
			 * are added to lvmetad.
			 */
			if (!lvmetad_pvscan_single(cmd, devl->dev, &found_vgnames, &pp.changed_vgnames))
				add_errors++;
		}
	}

	/*
	 * In the process of scanning devices, lvmetad may have become
	 * disabled.  If so, revert to scanning for the autoactivation step.
	 * Only autoactivate the VGs that were found during the dev scans.
	 */
	if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
		log_warn("WARNING: Not using lvmetad because %s.", reason);
		lvmetad_make_unused(cmd);
	}

activate:
	/*
	 * Step 2: when the PV was sent to lvmetad, the lvmetad reply
	 * indicated if all the PVs for the VG are now found.  If so,
	 * the vgname was added to the list, and we can attempt to
	 * autoactivate LVs in the VG.
	 */
	if (do_activate)
		ret = _pvscan_autoactivate(cmd, &pp, all_vgs, &found_vgnames);

out:
	if (remove_errors || add_errors || pp.activate_errors)
		ret = ECMD_FAILED;

	if (!sync_local_dev_names(cmd))
		stack;
	unlock_vg(cmd, NULL, VG_GLOBAL);
	return ret;
}

/*
 * Three main pvscan cases related to lvmetad usage:
 * 1. pvscan
 * 2. pvscan --cache
 * 3. pvscan --cache <dev>
 *
 * 1. The 'pvscan' command (without --cache) may or may not attempt to
 * repopulate the lvmetad cache, and may or may not use the lvmetad
 * cache to display PV info:
 *
 * i. If lvmetad is being used and is in a normal state, then 'pvscan'
 * will simply read and display PV info from the lvmetad cache.
 *
 * ii. If lvmetad is not being used, 'pvscan' will read all devices to
 * display the PV info.
 *
 * iii. If lvmetad is being used, but has been disabled (because of
 * duplicate devs), or has a non-matching token
 * (because the device filter is different from the device filter last
 * used to populate lvmetad), then 'pvscan' will begin by rescanning
 * devices to repopulate lvmetad.  If lvmetad is enabled after the
 * rescan, then 'pvscan' will simply read and display PV info from the
 * lvmetad cache (like case i).  If lvmetad is disabled after the
 * rescan, then 'pvscan' will read all devices to display PV info
 * (like case ii).
 *
 * 2. The 'pvscan --cache' command (without named devs) will always
 * attempt to repopulate the lvmetad cache by rescanning all devs
 * (regardless of whether lvmetad was previously disabled or had an
 * unmatching token.)  lvmetad may be enabled or disabled after the
 * rescan (depending on whether duplicate devs).
 *
 * 3. The 'pvscan --cache <dev>' command will attempt to repopulate the
 * lvmetad cache by rescanning all devs if lvmetad has a non-matching
 * token (e.g. because it has not yet been populated, see FIXME above).
 * Otherwise, the command will only rescan the named <dev> and send
 * their metadata to lvmetad.
 */

int pvscan(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_params params = { 0 };
	struct processing_handle *handle = NULL;
	const char *reason = NULL;
	int ret;

	if (arg_is_set(cmd, cache_long_ARG))
		return _pvscan_cache(cmd, argc, argv);

	if (argc) {
		log_error("Too many parameters on command line.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, activate_ARG)) {
		log_error("--activate is only valid with --cache.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, major_ARG) || arg_is_set(cmd, minor_ARG)) {
		log_error("--major and --minor are only valid with --cache.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, novolumegroup_ARG) && arg_is_set(cmd, exported_ARG)) {
		log_error("Options -e and -n are incompatible");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, exported_ARG) || arg_is_set(cmd, novolumegroup_ARG))
		log_warn("WARNING: only considering physical volumes %s",
			  arg_is_set(cmd, exported_ARG) ?
			  "of exported volume group(s)" : "in no volume group");

	/* Needed because this command has NO_LVMETAD_AUTOSCAN. */
	if (lvmetad_used() && (!lvmetad_token_matches(cmd) || lvmetad_is_disabled(cmd, &reason))) {
		if (lvmetad_used() && !lvmetad_pvscan_all_devs(cmd, 0)) {
			log_warn("WARNING: Not using lvmetad because cache update failed.");
			lvmetad_make_unused(cmd);
		}

		if (lvmetad_used() && lvmetad_is_disabled(cmd, &reason)) {
			log_warn("WARNING: Not using lvmetad because %s.", reason);
			lvmetad_make_unused(cmd);
		}
	}

	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
	}

	handle->custom_handle = &params;

	ret = process_each_pv(cmd, argc, argv, NULL, 0, 0, handle, _pvscan_single);

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
	unlock_vg(cmd, NULL, VG_GLOBAL);
	destroy_processing_handle(cmd, handle);

	return ret;
}
