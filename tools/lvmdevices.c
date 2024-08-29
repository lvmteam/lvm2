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

extern char devices_file_hostname_orig[PATH_MAX];
extern char devices_file_product_uuid_orig[PATH_MAX];

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

static char *_part_str(struct dev_use *du)
{
	static char _part_str_buf[64];

	if (du->part)
		snprintf(_part_str_buf, 63, " PART=%d", du->part);
	else
		_part_str_buf[0] = '\0';

	return _part_str_buf;
}

static void _print_check(struct cmd_context *cmd)
{
	struct dm_list use_old;
	struct dm_list use_new;
	struct dm_list done_old;
	struct dm_list done_new;
	struct dev_use *du_old, *du_new;
	int pvid_same;
	int idname_same;
	int idtype_same;
	int devname_same;
	int part_same;
	char *part;

	dm_list_init(&use_old);
	dm_list_init(&use_new);
	dm_list_init(&done_old);
	dm_list_init(&done_new);

	/*
	 * Move the entries that have been processed out of the way so
	 * original entries can be added to use_devices by device_ids_read().
	 * The processed entries are moved back to cmd->use_devices at the
	 * end of this function.
	 */
	dm_list_splice(&use_new, &cmd->use_devices);
	if (!device_ids_read(cmd))
		log_debug("Failed to read the devices file.");
	dm_list_splice(&use_old, &cmd->use_devices);
	dm_list_init(&cmd->use_devices);

	/*
	 * Check if system identifier is changed.
	 */

	if (cmd->device_ids_refresh_trigger) {
		int include_product_uuid = 0;
		int include_hostname = 0;

		if (cmd->product_uuid && cmd->device_ids_check_product_uuid) {
			include_product_uuid = 1;
			if (devices_file_product_uuid_orig[0] &&
			    strcmp(cmd->product_uuid, devices_file_product_uuid_orig))
				log_print_unless_silent("PRODUCT_UUID=%s (old %s): update",
					cmd->product_uuid, devices_file_product_uuid_orig);
			else if (!devices_file_product_uuid_orig[0])
				log_print_unless_silent("PRODUCT_UUID=%s: add", cmd->product_uuid);
		}
		if (!include_product_uuid && devices_file_product_uuid_orig[0])
			log_print_unless_silent("PRODUCT_UUID=%s: remove", devices_file_product_uuid_orig);

		/* hostname is only updated or added if product_uuid is not included */
		if (cmd->hostname && cmd->device_ids_check_hostname && !include_product_uuid) {
			include_hostname = 1;
			if (devices_file_hostname_orig[0] &&
			    strcmp(cmd->hostname, devices_file_hostname_orig))
				log_print_unless_silent("HOSTNAME=%s (old %s): update",
					cmd->hostname, devices_file_hostname_orig);
			else if (!devices_file_hostname_orig[0])
				log_print_unless_silent("HOSTNAME=%s: add", cmd->hostname);
		}
		if (!include_hostname && devices_file_hostname_orig[0])
			log_print_unless_silent("HOSTNAME=%s: remove", devices_file_hostname_orig);
	}

	/*
	 * Check entries with proper id types.
	 */
restart1:
	dm_list_iterate_items(du_old, &use_old) {
		if (du_old->idtype == DEV_ID_TYPE_DEVNAME)
			continue;
		dm_list_iterate_items(du_new, &use_new) {
			if (du_new->idtype == DEV_ID_TYPE_DEVNAME)
				continue;

			if (du_old->idtype != du_new->idtype)
				continue;

			if (!du_old->idname || !du_new->idname)
				continue;

			if (du_old->part != du_new->part)
				continue;

			if (!strcmp(du_old->idname, du_new->idname)) {
				part = _part_str(du_old);

				/*
				 * Old and new entries match based on device id.
				 * Possible differences between old and new:
				 * DEVNAME mismatch can be common.
				 * PVID mismatch is not common, but can
				 * happen from something like dd of one
				 * PV to another.
				 */

				if (!du_new->dev) {
					/* We can't know the new pvid and devname without a device. */
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: device not found",
						idtype_to_str(du_old->idtype),
						du_old->idname,
						du_old->devname ?: "none",
						du_old->pvid ?: "none",
						part);
					goto next1;
				}

				pvid_same = (du_old->pvid && du_new->pvid && !strcmp(du_old->pvid, du_new->pvid)) || (!du_old->pvid && !du_new->pvid);
				devname_same = (du_old->devname && du_new->devname && !strcmp(du_old->devname, du_new->devname)) || (!du_old->devname && !du_new->devname);

				if (pvid_same && devname_same) {
					log_verbose("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: no change",
						    idtype_to_str(du_new->idtype),
						    du_new->idname,
						    du_new->devname ?: "none",
						    du_new->pvid ?: "none",
						    part);

				} else if (!pvid_same && !devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s (old %s) PVID=%s (old %s)%s: update",
						idtype_to_str(du_new->idtype),
						du_new->idname,
						du_new->devname ?: "none",
						du_old->devname ?: "none",
						du_new->pvid ?: "none",
						du_old->pvid ?: "none",
						part);

				} else if (pvid_same && !devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s (old %s) PVID=%s%s: update",
						idtype_to_str(du_new->idtype),
						du_new->idname,
						du_new->devname ?: "none",
						du_old->devname ?: "none",
						du_new->pvid ?: "none",
						part);

				} else if (!pvid_same && devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s (old %s)%s: update",
						idtype_to_str(du_new->idtype),
						du_new->idname,
						du_new->devname ?: "none",
						du_new->pvid ?: "none",
						du_old->pvid ?: "none",
						part);
				}
			next1:
				dm_list_del(&du_old->list);
				dm_list_del(&du_new->list);
				dm_list_add(&done_old, &du_old->list);
				dm_list_add(&done_new, &du_new->list);
				goto restart1;
			}
		}
	}

	/*
	 * Check entries with devname id type.
	 */
restart2:
	dm_list_iterate_items(du_old, &use_old) {
		if (du_old->idtype != DEV_ID_TYPE_DEVNAME)
			continue;
		dm_list_iterate_items(du_new, &use_new) {
			if (du_new->idtype != DEV_ID_TYPE_DEVNAME)
				continue;

			if (!du_old->pvid || !du_new->pvid)
				continue;

			if (du_old->part != du_new->part)
				continue;

			if (!memcmp(du_old->pvid, du_new->pvid, strlen(du_old->pvid))) {
				part = _part_str(du_old);

				/*
				 * Old and new entries match based on PVID.
				 * IDNAME and DEVNAME might not match.
				 */

				if (!du_new->dev) {
					/* We can't know the new idname and devname without a device. */
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: device not found",
						idtype_to_str(du_old->idtype),
						du_old->idname ?: "none",
						du_old->devname ?: "none",
						du_old->pvid,
						part);
					goto next2;
				}

				idname_same = (du_old->idname && du_new->idname && !strcmp(du_old->idname, du_new->idname)) || (!du_old->idname && !du_new->idname);
				devname_same = (du_old->devname && du_new->devname && !strcmp(du_old->devname, du_new->devname)) || (!du_old->devname && !du_new->devname);

				if (idname_same && devname_same) {
					log_verbose("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: no change",
						    idtype_to_str(du_new->idtype),
						    du_new->idname ?: "none",
						    du_new->devname ?: "none",
						    du_new->pvid,
						    part);

				} else if (!idname_same && !devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s (old %s) DEVNAME=%s (old %s) PVID=%s%s: update",
						    idtype_to_str(du_new->idtype),
						    du_new->idname ?: "none",
						    du_old->idname ?: "none",
						    du_new->devname ?: "none",
						    du_old->devname ?: "none",
						    du_new->pvid,
						    part);

				} else if (idname_same && !devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s (old %s) PVID=%s%s: update",
						    idtype_to_str(du_new->idtype),
						    du_new->idname ?: "none",
						    du_new->devname ?: "none",
						    du_old->devname ?: "none",
						    du_new->pvid,
						    part);

				} else if (!idname_same && devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s (old %s) DEVNAME=%s PVID=%s%s: update",
						    idtype_to_str(du_new->idtype),
						    du_new->idname ?: "none",
						    du_old->idname ?: "none",
						    du_new->devname ?: "none",
						    du_new->pvid,
						    part);
				}
			next2:
				dm_list_del(&du_old->list);
				dm_list_del(&du_new->list);
				dm_list_add(&done_old, &du_old->list);
				dm_list_add(&done_new, &du_new->list);
				goto restart2;
			}
		}
	}

	/*
	 * Check entries with new IDTYPE (refresh can do this)
	 * Compare PVIDs.
	 */
restart3:
	dm_list_iterate_items(du_old, &use_old) {
		dm_list_iterate_items(du_new, &use_new) {

			if (!du_old->pvid || !du_new->pvid)
				continue;

			if (du_old->part != du_new->part)
				continue;

			if (!memcmp(du_old->pvid, du_new->pvid, strlen(du_old->pvid))) {
				part = _part_str(du_old);

				/*
				 * Old and new entries match based on PVID.
				 * IDTYPE, IDNAME, DEVNAME might not match.
				 */

				if (!du_new->dev) {
					/* could this happen? */
					log_print_unless_silent("IDTYPE=%s (%s) IDNAME=%s (%s) DEVNAME=%s (%s) PVID=%s%s: device not found",
						idtype_to_str(du_new->idtype),
						idtype_to_str(du_old->idtype),
						du_new->idname ?: "none",
						du_old->idname ?: "none",
						du_new->devname ?: "none",
						du_old->devname ?: "none",
						du_new->pvid,
						part);
					goto next3;
				}

				idtype_same = (du_old->idtype == du_new->idtype);
				idname_same = (du_old->idname && du_new->idname && !strcmp(du_old->idname, du_new->idname)) || (!du_old->idname && !du_new->idname);
				devname_same = (du_old->devname && du_new->devname && !strcmp(du_old->devname, du_new->devname)) || (!du_old->devname && !du_new->devname);

				if (idtype_same && idname_same && devname_same) {
					/* this case will probably be caught earlier */
					log_verbose("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: no change",
						    idtype_to_str(du_new->idtype),
						    du_new->idname ?: "none",
						    du_new->devname ?: "none",
						    du_new->pvid,
						    part);

				} else if (!idtype_same && !idname_same && !devname_same) {
					log_print_unless_silent("IDTYPE=%s (old %s) IDNAME=%s (old %s) DEVNAME=%s (old %s) PVID=%s%s: update",
						idtype_to_str(du_new->idtype),
						idtype_to_str(du_old->idtype),
						du_new->idname ?: "none",
						du_old->idname ?: "none",
						du_new->devname ?: "none",
						du_old->devname ?: "none",
						du_new->pvid,
						part);

				} else if (!idtype_same && !idname_same && devname_same) {
					log_print_unless_silent("IDTYPE=%s (old %s) IDNAME=%s (old %s) DEVNAME=%s PVID=%s%s: update",
						idtype_to_str(du_new->idtype),
						idtype_to_str(du_old->idtype),
						du_new->idname ?: "none",
						du_old->idname ?: "none",
						du_new->devname ?: "none",
						du_new->pvid,
						part);

				} else if (idtype_same && !idname_same && !devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s (old %s) DEVNAME=%s (old %s) PVID=%s%s: update",
						idtype_to_str(du_new->idtype),
						du_new->idname ?: "none",
						du_old->idname ?: "none",
						du_new->devname ?: "none",
						du_old->devname ?: "none",
						du_new->pvid,
						part);

				} else if (idtype_same && !idname_same && devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s (old %s) DEVNAME=%s PVID=%s%s: update",
						idtype_to_str(du_new->idtype),
						du_new->idname ?: "none",
						du_old->idname ?: "none",
						du_new->devname ?: "none",
						du_new->pvid,
						part);

				} else if (idtype_same && idname_same && !devname_same) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s (old %s) PVID=%s%s: update",
						idtype_to_str(du_new->idtype),
						du_new->idname ?: "none",
						du_new->devname ?: "none",
						du_old->devname ?: "none",
						du_new->pvid,
						part);
				}
			next3:
				dm_list_del(&du_old->list);
				dm_list_del(&du_new->list);
				dm_list_add(&done_old, &du_old->list);
				dm_list_add(&done_new, &du_new->list);
				goto restart3;
			}
		}
	}

	/*
	 * Handle old and new entries that remain and are identical.
	 * This covers entries that do not have enough valid fields
	 * set to definitively identify a PV.
	 */
restart4:
	dm_list_iterate_items(du_old, &use_old) {
		dm_list_iterate_items(du_new, &use_new) {
			idtype_same = (du_old->idtype == du_new->idtype);
			idname_same = (du_old->idname && du_new->idname && !strcmp(du_old->idname, du_new->idname)) || (!du_old->idname && !du_new->idname);
			devname_same = (du_old->devname && du_new->devname && !strcmp(du_old->devname, du_new->devname)) || (!du_old->devname && !du_new->devname);
			pvid_same = (du_old->pvid && du_new->pvid && !strcmp(du_old->pvid, du_new->pvid)) || (!du_old->pvid && !du_new->pvid);
			part_same = (du_old->part == du_new->part);

			if (idtype_same && idname_same && devname_same && pvid_same && part_same) {
				part = _part_str(du_old);

				log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: indeterminate",
					idtype_to_str(du_new->idtype),
					du_new->idname ?: "none",
					du_new->devname ?: "none",
					du_new->pvid ?: "none",
					part);

				dm_list_del(&du_old->list);
				dm_list_del(&du_new->list);
				dm_list_add(&done_old, &du_old->list);
				dm_list_add(&done_new, &du_new->list);
				goto restart4;
			}
		}
	}

	/*
	 * Entries remaining on old/new lists can't be directly
	 * correlated by loops above.
	 * Just print remaining old entries as being removed and
	 * remaining new entries as being added.
	 * If we find specific cases that reach here, we may
	 * want to add loops above to detect and print them
	 * more specifically.
	 */

	dm_list_iterate_items(du_old, &use_old) {
		part = _part_str(du_old);

		log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: old entry",
			idtype_to_str(du_old->idtype),
			du_old->idname ?: "none",
			du_old->devname ?: "none",
			du_old->pvid ?: "none",
			part);
	}

	dm_list_iterate_items(du_new, &use_new) {
		part = _part_str(du_new);

		log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: new entry",
			idtype_to_str(du_new->idtype),
			du_new->idname ?: "none",
			du_new->devname ?: "none",
			du_new->pvid ?: "none",
			part);
	}

	/* Restore cmd->use_devices list */

	dm_list_splice(&cmd->use_devices, &use_new);
	dm_list_splice(&cmd->use_devices, &done_new);
	free_dus(&use_old);
	free_dus(&done_old);
}

int lvmdevices(struct cmd_context *cmd, int argc, char **argv)
{
	struct dm_list search_pvids;
	struct dm_list found_devs;
	struct dm_list scan_devs;
	struct device_id_list *dil;
	struct device_list *devl;
	struct device *dev;
	struct dev_use *du, *du2;
	const char *deviceidtype;

	dm_list_init(&search_pvids);
	dm_list_init(&found_devs);
	dm_list_init(&scan_devs);

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
		int update_needed = 0;

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
		 * for this devices file entry.
		 */
		device_ids_validate(cmd, NULL, 0, 1, &update_needed);

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

			log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: remove multipath component",
					idtype_to_str(du->idtype),
					du->idname ?: "none",
					du->devname ?: "none",
					du->pvid ?: "none",
					_part_str(du));

			update_needed = 1;

			if (update_set)
				dm_list_del(&du->list);

			if (!(mpath_dev = dev_cache_get_by_devt(cmd, mpath_devno)))
				continue;

			if (!get_du_for_dev(cmd, mpath_dev)) {
				if (update_set) {
					log_print("Adding multipath device %s for multipath component %s.",
						  dev_name(mpath_dev), dev_name(du->dev));
					if (!device_id_add(cmd, mpath_dev, dev->pvid, NULL, NULL, 0))
						stack;
				} else {
					log_print_unless_silent("Missing multipath device %s for multipath component %s.",
						dev_name(mpath_dev), dev_name(du->dev));
				}
			}
		}

		if (!dm_list_empty(&cmd->device_ids_check_serial)) {
			device_ids_check_serial(cmd, &scan_devs, 1, &update_needed);
			/* device_ids_check_serial has done label_read_pvid on the scan_devs. */
		}

		/*
		 * Find devname entries that have moved to a renamed device.
		 * If --refresh is set, then also look for missing PVIDs on
		 * devices with new device ids of any type, e.g. a PVID that's
		 * moved to a new WWID.
		 */
		cmd->search_for_devnames = "all";
		device_ids_search(cmd, &found_devs, arg_is_set(cmd, refresh_ARG), 1, &update_needed);

		_print_check(cmd);

		dm_list_iterate_items(du, &cmd->use_devices) {
			if (du->dev)
				label_scan_invalidate(du->dev);
		}

		if (arg_is_set(cmd, delnotfound_ARG)) {
			dm_list_iterate_items_safe(du, du2, &cmd->use_devices) {
				if (!du->dev) {
					log_print_unless_silent("IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s: delete",
						idtype_to_str(du->idtype),
						du->idname ?: "none",
						du->devname ?: "none",
						du->pvid ?: "none",
						_part_str(du));
					dm_list_del(&du->list);
					free_du(du);
					update_needed = 1;
				}
			}
		}

		if (arg_is_set(cmd, update_ARG)) {
			if (update_needed || !dm_list_empty(&found_devs) || cmd->devices_file_hash_mismatch) {
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
			 * changes to the devices entries.
			 */
			if (update_needed) {
				log_error("Updates needed for devices file.");
				goto bad;
			}

			/*
			 * If only the hash comment would be updated, it isn't
			 * considered a "real" update for purposes of the
			 * --check exit code, since no device entries would be
			 * changed (although --update would lead to a new
			 * file version with the updated hash comment.)
			 */
			if (cmd->devices_file_hash_mismatch)
				log_print("Hash update needed for devices file.");
		}
		goto out;
	}

	if (arg_is_set(cmd, adddev_ARG)) {
		const char *devname;

		if (!(devname = arg_str_value(cmd, adddev_ARG, NULL)))
			goto_bad;

		/*
		 * adddev will add a device to devices_file even if that device
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

		if (!device_id_add(cmd, dev, dev->pvid, deviceidtype, NULL, 1))
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
			if (!device_id_add(cmd, devl->dev, devl->dev->pvid, deviceidtype, NULL, 1))
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
		log_print("Device %s IDTYPE=%s IDNAME=%s DEVNAME=%s PVID=%s%s",
			  du->dev ? dev_name(du->dev) : "none",
			  du->idtype ? idtype_to_str(du->idtype) : "none",
			  du->idname ? du->idname : "none",
			  du->devname ? du->devname : "none",
			  du->pvid ? (char *)du->pvid : "none",
			  _part_str(du));
	}

out:
	return ECMD_PROCESSED;

bad:
	return ECMD_FAILED;
}

