/*
 * Copyright (C) 2020 Red Hat, Inc. All rights reserved.
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
#include "lib/device/dev-type.h"
#include "lib/filters/filter.h"

/* coverity[unnecessary_header] needed for MuslC */
#include <sys/file.h>

static void _search_devs_for_pvids(struct cmd_context *cmd, struct dm_list *search_pvids, struct dm_list *found_devs)
{
	struct dev_iter *iter;
	struct device *dev;
	struct device_list *devl, *devl2;
	struct device_id_list *dil, *dil2;
	struct dm_list devs;
	int found;

	dm_list_init(&devs);

	/*
	 * Create a list of all devices on the system, without applying
	 * any filters, since we do not want filters to read any of the
	 * devices yet.
	 */
	if (!(iter = dev_iter_create(NULL, 0)))
		return;
	while ((dev = dev_iter_get(cmd, iter))) {
		/* Skip devs with a valid match to a du. */
		if (get_du_for_dev(cmd, dev))
			continue;

		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			continue;
		devl->dev = dev;
		dm_list_add(&devs, &devl->list);
	}
	dev_iter_destroy(iter);

	/*
	 * Apply the filters that do not require reading the devices
	 */
	log_debug("Filtering devices (no data) for pvid search");
	cmd->filter_nodata_only = 1;
	cmd->filter_deviceid_skip = 1;
	dm_list_iterate_items_safe(devl, devl2, &devs) {
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL))
			dm_list_del(&devl->list);
	}

	/*
	 * Read header from each dev to see if it has one of the pvids we're
	 * searching for.
	 */
	dm_list_iterate_items_safe(devl, devl2, &devs) {
		int has_pvid;

		/* sets dev->pvid if an lvm label with pvid is found */
		if (!label_read_pvid(devl->dev, &has_pvid))
			continue;
		if (!has_pvid)
			continue;

		found = 0;
		dm_list_iterate_items_safe(dil, dil2, search_pvids) {
			if (!strcmp(devl->dev->pvid, dil->pvid)) {
				dm_list_del(&devl->list);
				dm_list_del(&dil->list);
				dm_list_add(found_devs, &devl->list);
				log_print("Found PVID %s on %s.", dil->pvid, dev_name(devl->dev));
				found = 1;
				break;
			}
		}
		if (!found)
			label_scan_invalidate(devl->dev);

		/*
		 * FIXME: search all devs in case pvid is duplicated on multiple devs.
		 */
		if (dm_list_empty(search_pvids))
			break;
	}

	dm_list_iterate_items(dil, search_pvids)
		log_error("PVID %s not found on any devices.", dil->pvid);

	/*
	 * Now that the device has been read, apply the filters again
	 * which will now include filters that read data from the device.
	 * N.B. we've already skipped devs that were excluded by the
	 * no-data filters, so if the PVID exists on one of those devices
	 * no warning is printed.
	 */
	log_debug("Filtering devices (with data) for pvid search");
	cmd->filter_nodata_only = 0;
	cmd->filter_deviceid_skip = 1;
	dm_list_iterate_items_safe(devl, devl2, found_devs) {
		dev = devl->dev;
		cmd->filter->wipe(cmd, cmd->filter, dev, NULL);
		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			log_warn("WARNING: PVID %s found on %s which is excluded: %s",
			 	  dev->pvid, dev_name(dev), dev_filtered_reason(dev));
			dm_list_del(&devl->list);
		}
	}
}

int lvmdevices(struct cmd_context *cmd, int argc, char **argv)
{
	struct dm_list search_pvids;
	struct dm_list found_devs;
	struct device_id_list *dil;
	struct device_list *devl;
	struct device *dev;
	struct dev_use *du, *du2;
	const char *deviceidtype;

	dm_list_init(&search_pvids);
	dm_list_init(&found_devs);

	if (!setup_devices_file(cmd))
		return ECMD_FAILED;

	if (!cmd->enable_devices_file) {
		log_error("Devices file not enabled.");
		return ECMD_FAILED;
	}

	if (arg_is_set(cmd, update_ARG) ||
	    arg_is_set(cmd, adddev_ARG) || arg_is_set(cmd, deldev_ARG) ||
	    arg_is_set(cmd, addpvid_ARG) || arg_is_set(cmd, delpvid_ARG)) {
		if (!lock_devices_file(cmd, LOCK_EX)) {
			log_error("Failed to lock the devices file to create.");
			return ECMD_FAILED;
		}
		if (!devices_file_exists(cmd)) {
			if (!devices_file_touch(cmd)) {
				log_error("Failed to create the devices file.");
				return ECMD_FAILED;
			}
		}

		/*
		 * The hint file is associated with the default/system devices file,
		 * so don't clear hints when using a different --devicesfile.
		 */
		if (!cmd->devicesfile)
			clear_hint_file(cmd);
	} else {
		if (!lock_devices_file(cmd, LOCK_SH)) {
			log_error("Failed to lock the devices file.");
			return ECMD_FAILED;
		}
		if (!devices_file_exists(cmd)) {
			log_error("Devices file does not exist.");
			return ECMD_FAILED;
		}
	}

	if (!device_ids_read(cmd)) {
		log_error("Failed to read the devices file.");
		return ECMD_FAILED;
	}

	prepare_open_file_limit(cmd, dm_list_size(&cmd->use_devices));

	dev_cache_scan(cmd);
	device_ids_match(cmd);

	if (arg_is_set(cmd, check_ARG) || arg_is_set(cmd, update_ARG)) {
		int update_set = arg_is_set(cmd, update_ARG);
		int search_count = 0;
		int update_needed = 0;
		int invalid = 0;

		unlink_searched_devnames(cmd);

		label_scan_setup_bcache();

		dm_list_iterate_items(du, &cmd->use_devices) {
			if (!du->dev)
				continue;
			dev = du->dev;

			if (!label_read_pvid(dev, NULL))
				continue;

			/*
			 * label_read_pvid has read the first 4K of the device
			 * so these filters should not for the most part need
			 * to do any further reading of the device.
			 *
			 * We run the filters here for the first time in the
			 * check|update command.  device_ids_validate() then
			 * checks the result of this filtering (by checking the
			 * "persistent" filter explicitly), and prints a warning
			 * if a devices file entry does not pass the filters.
			 * The !passes_filter here is log_debug instead of log_warn
			 * to avoid repeating the same message as device_ids_validate.
			 * (We could also print the warning here and then pass a
			 * parameter to suppress the warning in device_ids_validate.)
			 */
			log_debug("Checking filters with data for %s", dev_name(dev));
			if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
				log_debug("filter result: %s in devices file is excluded by filter: %s.",
					  dev_name(dev), dev_filtered_reason(dev));
			}
		}

		/*
		 * Check that the pvid read from the lvm label matches the pvid
		 * for this devices file entry.  Also print a warning if a dev
		 * from use_devices does not pass the filters that have been
		 * run just above.
		 */
		device_ids_validate(cmd, NULL, &invalid, 1);
		if (invalid)
			update_needed = 1;

		/*
		 * Remove multipath components.
		 * Add multipath devs that had components listed.
		 */
		dm_list_iterate_items_safe(du, du2, &cmd->use_devices) {
			dev_t mpath_devno;
			struct device *mpath_dev;

			if (!du->dev)
				continue;
			dev = du->dev;

			if (!(dev->filtered_flags & DEV_FILTERED_MPATH_COMPONENT))
				continue;

			/* redundant given the flag check, but used to get devno */
			if (!dev_is_mpath_component(cmd, dev, &mpath_devno))
				continue;

			update_needed = 1;
			if (update_set) {
				log_print("Removing multipath component %s.", dev_name(du->dev));
				dm_list_del(&du->list);
			}

			if (!(mpath_dev = dev_cache_get_by_devt(cmd, mpath_devno)))
				continue;

			if (!get_du_for_dev(cmd, mpath_dev)) {
				if (update_set) {
					log_print("Adding multipath device %s for multipath component %s.",
						  dev_name(mpath_dev), dev_name(du->dev));
					if (!device_id_add(cmd, mpath_dev, dev->pvid, NULL, NULL))
						stack;
				} else {
					log_print("Missing multipath device %s for multipath component %s.",
						  dev_name(mpath_dev), dev_name(du->dev));
				}
			}
		}

		/*
		 * Find and fix any devname entries that have moved to a
		 * renamed device.
		 */
		device_ids_find_renamed_devs(cmd, &found_devs, &search_count, 1);

		if (search_count && !strcmp(cmd->search_for_devnames, "none"))
			log_print("Not searching for missing devnames, search_for_devnames=\"none\".");

		dm_list_iterate_items(du, &cmd->use_devices) {
			if (du->dev)
				label_scan_invalidate(du->dev);
		}

		if (arg_is_set(cmd, update_ARG)) {
			if (update_needed || !dm_list_empty(&found_devs)) {
				if (!device_ids_write(cmd))
					goto_bad;
				log_print("Updated devices file to version %s", devices_file_version());
			} else {
				log_print("No update for devices file is needed.");
			}
		} else {
			/*
			 * --check exits with an error if the devices file
			 * needs updates, i.e. running --update would make
			 * changes.
			 */
			if (update_needed) {
				log_error("Updates needed for devices file.");
				goto bad;
			}
		}
		goto out;
	}

	if (arg_is_set(cmd, adddev_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, adddev_ARG, NULL)))
			goto_bad;

		/*
		 * addev will add a device to devices_file even if that device
		 * is excluded by filters.
		 */

		/*
		 * No filter applied here (only the non-data filters would
		 * be applied since we haven't read the device yet.
		 */
		if (!(dev = dev_cache_get(cmd, devname, NULL))) {
			log_error("No device found for %s.", devname);
			goto bad;
		}

		/*
		 * reads pvid from dev header, sets dev->pvid.
		 * (it's ok if the device is not a PV and has no PVID)
		 */
		label_scan_setup_bcache();
		if (!label_read_pvid(dev, NULL)) {
			log_error("Failed to read %s.", devname);
			goto bad;
		}

		/*
		 * Allow filtered devices to be added to devices_file, but
		 * check if it's excluded by filters to print a warning.
		 * Since label_read_pvid has read the first 4K of the device,
		 * the filters should not for the most part need to do any further
		 * reading of the device.
		 *
		 * (This is the first time filters are being run, so we do
		 * not need to wipe filters of any previous result that was
		 * based on filter_deviceid_skip=0.)
		 */
		cmd->filter_deviceid_skip = 1;

		if (!cmd->filter->passes_filter(cmd, cmd->filter, dev, NULL)) {
			log_warn("WARNING: adding device %s that is excluded: %s.",
				 dev_name(dev), dev_filtered_reason(dev));
		}

		/* also allow deviceid_ARG ? */
		deviceidtype = arg_str_value(cmd, deviceidtype_ARG, NULL);

		if (!device_id_add(cmd, dev, dev->pvid, deviceidtype, NULL))
			goto_bad;
		if (!device_ids_write(cmd))
			goto_bad;
		goto out;
	}

	if (arg_is_set(cmd, addpvid_ARG)) {
		struct id id;
		char pvid[ID_LEN+1] = { 0 };
		const char *pvid_arg;

		label_scan_setup_bcache();

		/*
		 * Iterate through all devs on the system, reading the
		 * pvid of each to check if it has this pvid.
		 * Devices that are excluded by no-data filters will not
		 * be checked for the PVID.
		 * addpvid will not add a device to devices_file if it's
		 * excluded by filters.
		 */

		pvid_arg = arg_str_value(cmd, addpvid_ARG, NULL);
		if (!id_read_format_try(&id, pvid_arg)) {
			log_error("Invalid PVID.");
			goto bad;
		}
		memcpy(pvid, &id.uuid, ID_LEN);

		if ((du = get_du_for_pvid(cmd, pvid))) {
			log_error("PVID already exists in devices file for %s.", dev_name(du->dev));
			goto bad;
		}

		if (!(dil = dm_pool_zalloc(cmd->mem, sizeof(*dil))))
			goto_bad;
		memcpy(dil->pvid, &pvid, ID_LEN);
		dm_list_add(&search_pvids, &dil->list);

		_search_devs_for_pvids(cmd, &search_pvids, &found_devs);

		if (dm_list_empty(&found_devs)) {
			log_error("PVID %s not found on any devices.", pvid);
			goto bad;
		}
		dm_list_iterate_items(devl, &found_devs) {
			deviceidtype = arg_str_value(cmd, deviceidtype_ARG, NULL);
			if (!device_id_add(cmd, devl->dev, devl->dev->pvid, deviceidtype, NULL))
				goto_bad;
		}
		if (!device_ids_write(cmd))
			goto_bad;
		goto out;
	}

	if (arg_is_set(cmd, deldev_ARG) && !arg_is_set(cmd, deviceidtype_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, deldev_ARG, NULL)))
			goto_bad;

		if (strncmp(devname, "/dev/", 5))
			log_warn("WARNING: to remove a device by device id, include --deviceidtype.");

		/*
		 * No filter because we always want to allow removing a device
		 * by name from the devices file.
		 */
		if ((dev = dev_cache_get(cmd, devname, NULL))) {
			/*
			 * dev_cache_scan uses sysfs to check if an LV is using each dev
			 * and sets this flag is so.
			 */
			if (dev_is_used_by_active_lv(cmd, dev, NULL, NULL, NULL, NULL)) {
				if (!arg_count(cmd, yes_ARG) &&
			    	    yes_no_prompt("Device %s is used by an active LV, continue to remove? ", devname) == 'n') {
					log_error("Device not removed.");
					goto bad;
				}
			}
			if ((du = get_du_for_dev(cmd, dev)))
				goto dev_del;
		}

		if (!(du = get_du_for_devname(cmd, devname))) {
			log_error("No devices file entry for %s.", devname);
			goto bad;
		}
 dev_del:
		dm_list_del(&du->list);
		free_du(du);
		device_ids_write(cmd);
		goto out;
	}

	/*
	 * By itself, --deldev <devname> specifies a device name to remove.
	 * With an id type specified, --deldev specifies a device id to remove:
	 * --deldev <idname> --deviceidtype <idtype>
	 */
	if (arg_is_set(cmd, deldev_ARG) && arg_is_set(cmd, deviceidtype_ARG)) {
		const char *idtype_str = arg_str_value(cmd, deviceidtype_ARG, NULL);
		const char *idname = arg_str_value(cmd, deldev_ARG, NULL);
		int idtype;

		if (!idtype_str || !idname || !strlen(idname) || !strlen(idtype_str))
			goto_bad;

		if (!(idtype = idtype_from_str(idtype_str))) {
			log_error("Unknown device_id type.");
			goto_bad;
		}

		if (!strncmp(idname, "/dev/", 5))
			log_warn("WARNING: to remove a device by name, do not include --deviceidtype.");

		if (!(du = get_du_for_device_id(cmd, idtype, idname))) {
			log_error("No devices file entry with device id %s %s.", idtype_str, idname);
			goto_bad;
		}

		dev = du->dev;

		if (dev && dev_is_used_by_active_lv(cmd, dev, NULL, NULL, NULL, NULL)) {
			if (!arg_count(cmd, yes_ARG) &&
			    yes_no_prompt("Device %s is used by an active LV, continue to remove? ", dev_name(dev)) == 'n') {
				log_error("Device not removed.");
				goto bad;
			}
		}

		dm_list_del(&du->list);
		free_du(du);
		device_ids_write(cmd);
		goto out;
	}

	if (arg_is_set(cmd, delpvid_ARG)) {
		struct id id;
		char pvid[ID_LEN+1] = { 0 };
		const char *pvid_arg;

		pvid_arg = arg_str_value(cmd, delpvid_ARG, NULL);
		if (!id_read_format_try(&id, pvid_arg)) {
			log_error("Invalid PVID.");
			goto bad;
		}
		memcpy(pvid, &id.uuid, ID_LEN);

		if (!(du = get_du_for_pvid(cmd, pvid))) {
			log_error("PVID not found in devices file.");
			goto bad;
		}

		dm_list_del(&du->list);

		if ((du2 = get_du_for_pvid(cmd, pvid))) {
			log_error("Multiple devices file entries for PVID %s (%s %s), remove by device name.",
				  pvid, du->devname, du2->devname);
			goto bad;
		}

		if (du->devname && (du->devname[0] != '.')) {
			if ((dev = dev_cache_get(cmd, du->devname, NULL)) &&
			    dev_is_used_by_active_lv(cmd, dev, NULL, NULL, NULL, NULL)) {
				if (!arg_count(cmd, yes_ARG) &&
			    	    yes_no_prompt("Device %s is used by an active LV, continue to remove? ", du->devname) == 'n') {
					log_error("Device not removed.");
					goto bad;
				}
			}
		}

		free_du(du);
		device_ids_write(cmd);
		goto out;
	}

	/* If no options, print use_devices list */

	dm_list_iterate_items(du, &cmd->use_devices) {
		char part_buf[64] = { 0 };

		if (du->part)
			snprintf(part_buf, 63, " PART=%d", du->part);

		log_print("Device %s IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s",
			  du->dev ? dev_name(du->dev) : "none",
			  du->idtype ? idtype_to_str(du->idtype) : "none",
			  du->idname ? du->idname : "none",
			  du->devname ? du->devname : "none",
			  du->pvid ? (char *)du->pvid : "none",
			  part_buf);
	}

out:
	return ECMD_PROCESSED;

bad:
	return ECMD_FAILED;
}

