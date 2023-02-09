/*
 * Copyright (C) 2016 Red Hat, Inc. All rights reserved.
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
#include "lib/cache/lvmcache.h"
#include "lib/device/device_id.h"

struct vgimportclone_params {
	struct dm_list new_devs;
	const char *base_vgname;
	const char *old_vgname;
	const char *new_vgname;
	unsigned import_devices:1;
	unsigned import_vg:1;
};

static int _update_vg(struct cmd_context *cmd, struct volume_group *vg,
		      struct vgimportclone_params *vp)
{
	char uuid[64] __attribute__((aligned(8)));
	struct pv_list *pvl, *new_pvl;
	struct lv_list *lvl;
	struct device_list *devl;
	struct dm_list tmp_devs;
	int devs_used_for_lv = 0;

	dm_list_init(&tmp_devs);

	if (vg_is_exported(vg) && !vp->import_vg) {
		log_error("VG %s is exported, use the --import option.", vg->name);
		goto bad;
	}

	if (vg_status(vg) & PARTIAL_VG) {
		log_error("VG %s is partial, it must be complete.", vg->name);
		goto bad;
	}

	/*
	 * N.B. lvs_in_vg_activated() is not smart enough to distinguish
	 * between LVs that are active in the original VG vs the cloned VG
	 * that's being imported, so check dev_is_used_by_active_lv.
	 */
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (is_missing_pv(pvl->pv) || !pvl->pv->dev) {
			log_error("VG is missing a device.");
			goto bad;
		}
		if (dev_is_used_by_active_lv(cmd, pvl->pv->dev, NULL, NULL, NULL, NULL)) {
			log_error("Device %s has active LVs, deactivate first.", dev_name(pvl->pv->dev));
			devs_used_for_lv++;
		}
	}

	if (devs_used_for_lv)
		goto_bad;

	/*
	 * The new_devs list must match the PVs in VG.
	 */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if ((devl = device_list_find_dev(&vp->new_devs, pvl->pv->dev))) {
			dm_list_del(&devl->list);
			dm_list_add(&tmp_devs, &devl->list);
		} else {
			if (!id_write_format(&pvl->pv->id, uuid, sizeof(uuid)))
				goto_bad;

			/* all PVs in the VG must be imported together, pvl is missing from args. */
			log_error("PV with UUID %s is part of VG %s, but is not included in the devices to import.",
				   uuid, vg->name);
			log_error("All PVs in the VG must be imported together.");
			goto bad;
		}
	}

	dm_list_iterate_items(devl, &vp->new_devs) {
		/* device arg is not in the VG. */
		log_error("Device %s was not found in VG %s.", dev_name(devl->dev), vg->name);
		log_error("The devices to import must match the devices in the VG.");
		goto bad;
	}

	dm_list_splice(&vp->new_devs, &tmp_devs);

	/*
	 * Write changes.
	 */

	if (vp->import_vg)
		vg->status &= ~EXPORTED_VG;

	if (!id_create(&vg->id))
		goto_bad;

	/* Low level vg_write code needs old_name to be set! */
	vg->old_name = vg->name;

	if (!(vg->name = dm_pool_strdup(vg->vgmem, vp->new_vgname)))
		goto_bad;

	/* A duplicate of a shared VG is imported as a new local VG. */
	vg->lock_type = NULL;
	vg->lock_args = NULL;
	vg->system_id = cmd->system_id ? dm_pool_strdup(vg->vgmem, cmd->system_id) : NULL;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(new_pvl = dm_pool_zalloc(vg->vgmem, sizeof(*new_pvl))))
			goto_bad;

		new_pvl->pv = pvl->pv;

		if (!(pvl->pv->vg_name = dm_pool_strdup(vg->vgmem, vp->new_vgname)))
			goto_bad;

		if (vp->import_vg)
			new_pvl->pv->status &= ~EXPORTED_VG;

		/* Low level pv_write code needs old_id to be set! */
		memcpy(&new_pvl->pv->old_id, &new_pvl->pv->id, sizeof(new_pvl->pv->id));

		if (!id_create(&new_pvl->pv->id))
			goto_bad;

		memcpy(&pvl->pv->dev->pvid, &new_pvl->pv->id.uuid, ID_LEN);

		dm_list_add(&vg->pv_write_list, &new_pvl->list);
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		memcpy(&lvl->lv->lvid, &vg->id, sizeof(vg->id));
		lvl->lv->lock_args = NULL;
	}

	/*
	 * Add the device id before writing the vg so that the device id
	 * will be included in the metadata.  The device file is written
	 * (with these additions) at the end of the command.
	 */
	if (vp->import_devices || cmd->enable_devices_file) {
		dm_list_iterate_items(devl, &vp->new_devs) {
			if (!device_id_add(cmd, devl->dev, devl->dev->pvid, NULL, NULL, 0)) {
				log_error("Failed to add device id for %s.", dev_name(devl->dev));
				goto bad;
			}
		}
	}

	if (!vg_write(vg) || !vg_commit(vg))
		goto_bad;

	return 1;
bad:
	return 0;
}

/*
 * Create a list of devices that label_scan would scan excluding devs in
 * new_devs.
 */
static int _get_other_devs(struct cmd_context *cmd, struct dm_list *new_devs, struct dm_list *other_devs)
{
	struct dev_iter *iter;
	struct device *dev;
	struct device_list *devl;
	int r = 1;

	if (!(iter = dev_iter_create(cmd->filter, 0)))
		return_0;

	while ((dev = dev_iter_get(cmd, iter))) {
		if (device_list_find_dev(new_devs, dev))
			continue;
		if (!(devl = zalloc(sizeof(*devl)))) {
			r = 0;
			goto_bad;
		}
		devl->dev = dev;
		dm_list_add(other_devs, &devl->list);
	}
bad:
	dev_iter_destroy(iter);
	return r;
}

int vgimportclone(struct cmd_context *cmd, int argc, char **argv)
{
	struct vgimportclone_params vp;
	struct dm_list vgnames;
	struct vgnameid_list *vgnl;
	struct device *dev;
	struct device_list *devl;
	struct dm_list other_devs;
	struct volume_group *vg, *error_vg = NULL;
	const char *vgname;
	char base_vgname[NAME_LEN] = { 0 };
	char tmp_vgname[NAME_LEN] = { 0 };
	uint32_t lockd_state = 0;
	uint32_t error_flags = 0;
	unsigned int vgname_count;
	int ret = ECMD_FAILED;
	int i;

	dm_list_init(&vgnames);
	dm_list_init(&other_devs);

	set_pv_notify(cmd);

	memset(&vp, 0, sizeof(vp));
	dm_list_init(&vp.new_devs);
	vp.import_devices = arg_is_set(cmd, importdevices_ARG);
	vp.import_vg = arg_is_set(cmd, import_ARG);

	if (!lock_global(cmd, "ex"))
		return ECMD_FAILED;

	clear_hint_file(cmd);

	cmd->edit_devices_file = 1;

	if (!setup_devices(cmd)) {
		log_error("Failed to set up devices.");
		return ECMD_FAILED;
	}

	/*
	 * When importing devices not in the devices file
	 * we cannot use the device id filter when looking
	 * for the devs.
	 */
	if (vp.import_devices) {
		if (!cmd->enable_devices_file) {
			log_print("Devices file not enabled, ignoring importdevices.");
			vp.import_devices = 0;
		} else if (!devices_file_exists(cmd)) {
			log_print("Devices file does not exist, ignoring importdevices.");
			vp.import_devices = 0;
		} else {
			cmd->filter_deviceid_skip = 1;
		}
	}

	/*
	 * For each device arg, get the dev from dev-cache.
	 * Only apply nodata filters when getting the devs
	 * from dev cache.  The data filters will be applied
	 * next when label scan is done on them.
	 */
	cmd->filter_nodata_only = 1;

	for (i = 0; i < argc; i++) {
		if (!(dev = dev_cache_get(cmd, argv[i], cmd->filter))) {
			/* FIXME: if filtered print which */
			log_error("Failed to find device %s.", argv[i]);
			goto out;
		}

		if (!(devl = zalloc(sizeof(*devl))))
			goto_out;

		devl->dev = dev;
		dm_list_add(&vp.new_devs, &devl->list);
	}

	/*
	 * Clear the result of nodata filtering so all
	 * filters will be applied in label_scan.
	 */
	dm_list_iterate_items(devl, &vp.new_devs)
		cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);

	/*
	 * Scan lvm info from each new dev, and apply the filters
	 * again, this time applying filters that use data.
	 */
	log_debug("scan new devs");

	label_scan_setup_bcache();

	cmd->filter_nodata_only = 0;

	label_scan_devs(cmd, cmd->filter, &vp.new_devs);

	/*
	 * Check if any new devs were excluded by filters
	 * in label scan, where all filters were applied.
	 * (incl those that need data.)
	 */
	dm_list_iterate_items(devl, &vp.new_devs) {
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, "persistent")) {
			log_error("Device %s is excluded: %s.",
				  dev_name(devl->dev), dev_filtered_reason(devl->dev));
			goto out;
		}
	}

	/*
	 * Look up vg info in lvmcache for each new_devs entry.  This info was
	 * found by label scan.  Verify all the new devs are from the same vg.
	 * The lvmcache at this point only reflects a label scan, not a vg_read
	 * which would assign PV info's for PVs without metadata.  So this
	 * check is incomplete, and the same vg for devs is verified again
	 * later.
	 */
	dm_list_iterate_items(devl, &vp.new_devs) {
		struct lvmcache_info *info;

		if (!(info = lvmcache_info_from_pvid(devl->dev->pvid, devl->dev, 0))) {
			log_error("Failed to find PVID for device %s.", dev_name(devl->dev));
			goto out;
		}

		if (!(vgname = lvmcache_vgname_from_info(info)) || is_orphan_vg(vgname)) {
			/* The PV may not have metadata, this will be resolved in
			   the process_each_vg/vg_read at the end. */
			continue;
		}

		if (!vp.old_vgname) {
			if (!(vp.old_vgname = dm_pool_strdup(cmd->mem, vgname)))
				goto_out;
		} else if (strcmp(vp.old_vgname, vgname)) {
			log_error("Devices must be from the same VG.");
			goto out;
		}
	}

	if (!vp.old_vgname) {
		log_error("No VG found on devices.");
		goto out;
	}

	/*
	 * Get rid of lvmcache info from the new devs because we are going to
	 * read the other devs next (which conflict with the new devs because
	 * of the duplicated info.)
	 */
	dm_list_iterate_items(devl, &vp.new_devs)
		label_scan_invalidate(devl->dev);
	lvmcache_destroy(cmd, 1, 0);

	/*
	 * Now processing other devs instead of new devs, so return to using
	 * the deviceid filter.  (wiping filters not needed since these other
	 * devs have not been filtered yet.)
	 */
	cmd->filter_deviceid_skip = 0;

	/*
	 * Scan all other devs (devs that would normally be seen excluding new
	 * devs).  This is necessary to check if the new vgname conflicts with
	 * an existing vgname on other devices.  We don't need to actually
	 * process any existing VGs, we only process the VG on the new devs
	 * being imported after this.
	 *
	 * This only requires a label_scan of the other devs which is enough to
	 * see what the other vgnames are.
	 *
	 * Only apply nodata filters when creating the other_devs list.
	 * Then apply all filters when label_scan_devs processes the label.
	 */

	log_debug("get other devices");

	cmd->filter_nodata_only = 1;

	if (!_get_other_devs(cmd, &vp.new_devs, &other_devs))
		goto_out;

	log_debug("scan other devices");

	cmd->filter_nodata_only = 0;

	/*
	 * Clear the result of nodata filtering so all
	 * filters will be applied in label_scan.
	 */
	dm_list_iterate_items(devl, &other_devs)
		cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);

	label_scan_devs(cmd, cmd->filter, &other_devs);

	if (!lvmcache_get_vgnameids(cmd, &vgnames, NULL, 0))
		goto_out;

	/*
	 * Pick a new VG name, save as new_vgname.  The new name begins with
	 * the basevgname or old_vgname, plus a $i suffix, if necessary, to
	 * make it unique.
	 */

	if (arg_is_set(cmd, basevgname_ARG)) {
		vgname = arg_str_value(cmd, basevgname_ARG, "");
		if (dm_snprintf(base_vgname, sizeof(base_vgname), "%s", vgname) < 0) {
			log_error("Base vg name %s is too long.", vgname);
			goto out;
		}
		if (strcmp(vgname, vp.old_vgname)) {
			(void) dm_strncpy(tmp_vgname, base_vgname, NAME_LEN);
			vgname_count = 0;
		} else {
			/* Needed when basename matches old name, and PV is not a duplicate
			   which means old name is not found on other devs, and is not seen
			   in the vgnames search below, causing old and new names to match. */
			if (dm_snprintf(tmp_vgname, sizeof(tmp_vgname), "%s1", vp.old_vgname) < 0) {
				log_error("Temporary vg name %s1 is too long.", vp.old_vgname);
				goto out;
			}
			vgname_count = 1;
		}
	} else {
		if (dm_snprintf(base_vgname, sizeof(base_vgname), "%s", vp.old_vgname) < 0) {
			log_error(INTERNAL_ERROR "Old vg name %s is too long.", vp.old_vgname);
			goto out;
		}
		if (dm_snprintf(tmp_vgname, sizeof(tmp_vgname), "%s1", vp.old_vgname) < 0) {
			log_error("Temporary vg name %s1 is too long.", vp.old_vgname);
			goto out;
		}
		vgname_count = 1;
	}

retry_name:
	dm_list_iterate_items(vgnl, &vgnames) {
		if (!strcmp(vgnl->vg_name, tmp_vgname)) {
			vgname_count++;
			if (dm_snprintf(tmp_vgname, sizeof(tmp_vgname), "%s%u", base_vgname, vgname_count) < 0) {
				log_error("Failed to generated temporary vg name, %s%u is too long.", base_vgname, vgname_count);
				goto out;
			}
			goto retry_name;
		}
	}

	if (!(vp.new_vgname = dm_pool_strdup(cmd->mem, tmp_vgname)))
		goto_out;
	log_debug("Using new VG name %s.", vp.new_vgname);

	/*
	 * Get rid of lvmcache info from the other devs because we are going to
	 * read the new devs again, now to update them.
	 */
	dm_list_iterate_items(devl, &other_devs)
		label_scan_invalidate(devl->dev);
	lvmcache_destroy(cmd, 1, 0);

	log_debug("import vg on new devices");

	if (!lock_vol(cmd, vp.new_vgname, LCK_VG_WRITE, NULL)) {
		log_error("Can't get lock for new VG name %s", vp.new_vgname);
		goto out;
	}

	if (!lock_vol(cmd, vp.old_vgname, LCK_VG_WRITE, NULL)) {
		log_error("Can't get lock for VG name %s", vp.old_vgname);
		goto out;
	}

	/* No filter used since these devs have already been filtered above. */
	label_scan_devs_rw(cmd, NULL, &vp.new_devs);

	cmd->can_use_one_scan = 1;
	cmd->include_exported_vgs = 1;

	vg = vg_read(cmd, vp.old_vgname, NULL, READ_WITHOUT_LOCK | READ_FOR_UPDATE, lockd_state, &error_flags, &error_vg);
	if (!vg) {
		log_error("Failed to read VG %s from devices being imported.", vp.old_vgname);
		unlock_vg(cmd, NULL, vp.old_vgname);
		unlock_vg(cmd, NULL, vp.new_vgname);
		goto out;
	}

	if (error_flags) {
		log_error("Error reading VG %s from devices being imported.", vp.old_vgname);
		release_vg(vg);
		unlock_vg(cmd, NULL, vp.old_vgname);
		unlock_vg(cmd, NULL, vp.new_vgname);
		goto out;
	}

	if (!_update_vg(cmd, vg, &vp)) {
		log_error("Failed to update VG on devices being imported.");
		release_vg(vg);
		unlock_vg(cmd, NULL, vp.old_vgname);
		unlock_vg(cmd, NULL, vp.new_vgname);
		goto out;
	}

	release_vg(vg);
	unlock_vg(cmd, NULL, vp.old_vgname);
	unlock_vg(cmd, NULL, vp.new_vgname);

	/*
	 * Should we be using device_ids_validate to check/fix other
	 * devs in the devices file?
	 */
	if (vp.import_devices || cmd->enable_devices_file) {
		if (!device_ids_write(cmd)) {
			log_error("Failed to write devices file.");
			goto out;
		}
	}
	ret = ECMD_PROCESSED;
out:
	if (error_vg)
		release_vg(error_vg);
	unlock_devices_file(cmd);
	return ret;
}
