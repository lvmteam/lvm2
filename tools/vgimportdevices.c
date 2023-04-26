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
/* coverity[unnecessary_header] needed for MuslC */
#include <sys/file.h>

struct vgimportdevices_params {
	uint32_t added_devices;
};

static int _vgimportdevices_single(struct cmd_context *cmd,
				   const char *vg_name,
				   struct volume_group *vg,
				   struct processing_handle *handle)
{
	struct vgimportdevices_params *vp = (struct vgimportdevices_params *) handle->custom_handle;
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct pv_list *pvl;
	struct physical_volume *pv;
	int update_vg = 1;
	int updated_pvs = 0;
	const char *idtypestr = NULL; /* deviceidtype_ARG ? */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (is_missing_pv(pvl->pv) || !pvl->pv->dev) {
			memcpy(pvid, &pvl->pv->id.uuid, ID_LEN);
			log_print("Not importing devices for VG %s with missing PV %s.",
				  vg->name, pvid);
			return ECMD_PROCESSED;
		}
	}

	/*
	 * We want to allow importing devices of foreign and shared 
	 * VGs, but we do not want to update device_ids in those VGs.
	 *
	 * If --foreign is set, then foreign VGs will be passed
	 * to this function; add devices but don't update vg.
	 * shared VGs are passed to this function; add devices
	 * and do not update.
	 */
	if (vg_is_foreign(vg) || vg_is_shared(vg))
		update_vg = 0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		pv = pvl->pv;

		idtypestr = pv->device_id_type;

		memcpy(pvid, &pvl->pv->id.uuid, ID_LEN);
		device_id_add(cmd, pv->dev, pvid, idtypestr, NULL, 0);
		vp->added_devices++;

		/* We could skip update if the device_id has not changed. */

		if (!update_vg)
			continue;

		updated_pvs++;
	}

	/*
	 * Writes the device_id of each PV into the vg metadata.
	 * This is not a critial step and should not influence
	 * the result of the command.
	 */
	if (updated_pvs) {
		if (!vg_write(vg) || !vg_commit(vg))
			log_print("Failed to write device ids in VG metadata.");
	}

	return ECMD_PROCESSED;
}

/*
 * This command always scans all devices on the system,
 * any pre-existing devices_file does not limit the scope.
 *
 * This command adds the VG's devices to whichever
 * devices_file is set in config or command line.
 * If devices_file doesn't exist, it's created.
 *
 * If devices_file is "" then this file will scan all devices
 * and show the devices that it would otherwise have added to
 * the devices_file.  The VG is not updated with device_ids.
 *
 * This command updates the VG metadata to add device_ids
 * (if the metadata is missing them), unless an option is
 * set to skip that, e.g. --nodeviceidupdate?
 *
 * If the VG found has a foreign system ID then an error
 * will be printed.  To import devices from a foreign VG:
 * vgimportdevices --foreign -a
 * vgimportdevices --foreign VG
 *
 * If there are duplicate VG names it will do nothing.
 *
 * If there are duplicate PVIDs related to VG it will do nothing,
 * the user would need to add the PVs they want with lvmdevices --add.
 *
 * vgimportdevices -a (no vg arg) will import all accesible VGs.
 */

int vgimportdevices(struct cmd_context *cmd, int argc, char **argv)
{
	struct vgimportdevices_params vp = { 0 };
	struct processing_handle *handle;
	int created_file = 0;
	int ret = ECMD_FAILED;

	if (arg_is_set(cmd, foreign_ARG))
		cmd->include_foreign_vgs = 1;

	cmd->include_shared_vgs = 1;

	/* So that we can warn about this. */
	cmd->handles_missing_pvs = 1;

	if (!lock_global(cmd, "ex"))
		return ECMD_FAILED;

	/*
	 * Prepare/create devices file preemptively because the error path for
	 * this case from process_each/setup_devices is not as clean.
	 * This means that when setup_devices is called, it the devices
	 * file steps will be redundant, and need to handle being repeated.
	 */
	if (!setup_devices_file(cmd)) {
		log_error("Failed to set up devices file.");
		return ECMD_FAILED;
	}
	if (!cmd->enable_devices_file) {
		log_error("Devices file not enabled.");
		return ECMD_FAILED;
	}
	if (!lock_devices_file(cmd, LOCK_EX)) {
		log_error("Failed to lock the devices file.");
		return ECMD_FAILED;
	}
	if (!devices_file_exists(cmd)) {
	       	if (!devices_file_touch(cmd)) {
			log_error("Failed to create devices file.");
			return ECMD_FAILED;
		}
		created_file = 1;
	}

	/*
	 * The hint file is associated with the default/system devices file,
	 * so don't clear hints when using a different --devicesfile.
	 */
	if (!cmd->devicesfile)
		clear_hint_file(cmd);

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		goto out;
	}
	handle->custom_handle = &vp;

	/*
	 * import is a case where we do not want to be limited by an existing
	 * devices file because we want to search outside the devices file for
	 * new devs to add to it, but we do want devices file entries on
	 * use_devices so we can update and write out that list.
	 *
	 * Ususally when devices file is enabled, we use filter-deviceid and
	 * skip filter-regex.  In this import case it's reversed, and we skip
	 * filter-deviceid and use filter-regex.
	 */
	cmd->filter_deviceid_skip = 1;
	cmd->filter_regex_with_devices_file = 1;
	cmd->create_edit_devices_file = 1;

	/*
	 * This helps a user bootstrap existing shared VGs into the devices
	 * file. Reading the vg to import devices requires locking, but
	 * lockstart won't find the vg before it's in the devices file.
	 * So, allow importing devices without an lvmlockd lock (in a
	 * a shared vg the vg metadata won't be updated with device ids,
	 * so the lvmlockd lock isn't protecting vg modification.)
	 */
	cmd->lockd_gl_disable = 1;
	cmd->lockd_vg_disable = 1;

	/*
	 * For each VG:
	 * device_id_add() each PV in the VG
	 * update device_ids in the VG (potentially)
	 *
	 * process_each_vg->label_scan->setup_devices
	 * setup_devices sees create_edit_devices_file is 1,
	 * so it does lock_devices_file(EX), then it creates/reads
	 * the devices file, then each device_id_add happens
	 * above, and then device_ids_write happens below.
	 */
	ret = process_each_vg(cmd, argc, argv, NULL, NULL, READ_FOR_UPDATE,
			      0, handle, _vgimportdevices_single);
	if (ret == ECMD_FAILED) {
		/*
		 * Error from setting up devices file or label_scan,
		 * _vgimportdevices_single does not return an error.
		 */
		goto_out;
	}

	if (!vp.added_devices) {
		log_error("No devices to add.");
		ret = ECMD_FAILED;
		goto out;
	}

	if (!device_ids_write(cmd)) {
		log_error("Failed to write the devices file.");
		ret = ECMD_FAILED;
		goto out;
	}

	log_print("Added %u devices to devices file.", vp.added_devices);
out:
	if ((ret == ECMD_FAILED) && created_file)
		if (unlink(cmd->devices_file_path) < 0)
			log_sys_debug("unlink", cmd->devices_file_path);
	destroy_processing_handle(cmd, handle);
	return ret;
}

