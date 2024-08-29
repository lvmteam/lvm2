/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
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
#include "lib/device/device_id.h"
#include "lib/label/hints.h"

struct vgchange_params {
	int lock_start_count;
	unsigned int lock_start_sanlock : 1;
	unsigned int vg_complete_to_activate : 1;
	char *root_dm_uuid; /* dm uuid of LV under root fs */
};

/*
 * Increments *count by the number of _new_ monitored devices.
 */
static int _monitor_lvs_in_vg(struct cmd_context *cmd,
			      struct volume_group *vg, int reg, int *count)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	int r = 1;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_info(cmd, lv, lv_is_thin_pool(lv) ? 1 : 0,
			     NULL, 0, 0))
			continue;
		/*
		 * FIXME: Need to consider all cases... PVMOVE, etc
		 */
		if (lv_is_pvmove(lv))
			continue;

		if (!monitor_dev_for_events(cmd, lv, 0, reg)) {
			r = 0;
			continue;
		}

		(*count)++;
	}

	return r;
}

static int _poll_lvs_in_vg(struct cmd_context *cmd,
			   struct volume_group *vg)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	int count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (lv_is_active(lv) &&
		    (lv_is_pvmove(lv) || lv_is_converting(lv) || lv_is_merging(lv))) {
			lv_spawn_background_polling(cmd, lv);
			count++;
		}
	}

	/*
	 * returns the number of polled devices
	 * - there is no way to know if lv is already being polled
	 */

	return count;
}

static int _activate_lvs_in_vg(struct cmd_context *cmd, struct volume_group *vg,
			       activation_change_t activate)
{
	struct lv_list *lvl;
	struct logical_volume *lv;
	int count = 0, expected_count = 0, r = 1;

	sigint_allow();
	dm_list_iterate_items(lvl, &vg->lvs) {
		if (sigint_caught())
			return_0;

		lv = lvl->lv;

		if (!lv_is_visible(lv) && (!cmd->process_component_lvs || !lv_is_component(lv)))
			continue;

		/* If LV is sparse, activate origin instead */
		if (lv_is_cow(lv) && lv_is_virtual_origin(origin_from_cow(lv)))
			lv = origin_from_cow(lv);

		/* Only request activation of snapshot origin devices */
		if (lv_is_snapshot(lv) || lv_is_cow(lv))
			continue;

		/* Only request activation of mirror LV */
		if (lv_is_mirror_image(lv) || lv_is_mirror_log(lv))
			continue;

		if (lv_is_vdo_pool(lv))
			continue;

		if (lv_activation_skip(lv, activate, arg_is_set(cmd, ignoreactivationskip_ARG)))
			continue;

		if ((activate == CHANGE_AAY) &&
		    !lv_passes_auto_activation_filter(cmd, lv))
			continue;

		/* vg NOAUTOACTIVATE flag was already checked */
		if ((activate == CHANGE_AAY) && (lv->status & LV_NOAUTOACTIVATE))
			continue;

		expected_count++;

		if (!lv_change_activate(cmd, lv, activate)) {
			stack;
			r = 0;
			continue;
		}

		count++;
	}

	sigint_restore();

	if (expected_count)
		log_verbose("%sctivated %d logical volumes in volume group %s.",
			    is_change_activating(activate) ? "A" : "Dea",
			    count, vg->name);

	/*
	 * After successful activation we need to initialise polling
	 * for all activated LVs in a VG. Possible enhancement would
	 * be adding --poll y|n cmdline option for pvscan and call
	 * init_background_polling routine in autoactivation handler.
	 */
	if (count && is_change_activating(activate) &&
	    !vgchange_background_polling(cmd, vg)) {
		stack;
		r = 0;
	}

	/* Wait until devices are available */
	if (!sync_local_dev_names(vg->cmd)) {
		log_error("Failed to sync local devices for VG %s.", vg->name);
		r = 0;
	}

	return r;
}

static int _vgchange_monitoring(struct cmd_context *cmd, struct volume_group *vg)
{
	int r = 1;
	int monitored = 0;

	if (lvs_in_vg_activated(vg) &&
	    dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) {
		if (!_monitor_lvs_in_vg(cmd, vg, dmeventd_monitor_mode(), &monitored))
			r = 0;
		log_print_unless_silent("%d logical volume(s) in volume group "
					"\"%s\" %smonitored",
					monitored, vg->name, (dmeventd_monitor_mode()) ? "" : "un");
	}

	return r;
}

int vgchange_background_polling(struct cmd_context *cmd, struct volume_group *vg)
{
	int polled;

	if (background_polling()) {
		log_debug_activation("Starting background polling for volume group \"%s\".", vg->name);
		polled = _poll_lvs_in_vg(cmd, vg);
		if (polled)
			log_print_unless_silent("Background polling started for %d logical volume(s) "
						"in volume group \"%s\"",
						polled, vg->name);
	}

	return 1;
}

int vgchange_activate(struct cmd_context *cmd, struct volume_group *vg,
		      activation_change_t activate, int vg_complete_to_activate, char *root_dm_uuid)
{
	int lv_open, active, monitored = 0, r = 1;
	const struct lv_list *lvl;
	struct pv_list *pvl;
	int do_activate = is_change_activating(activate);

	/*
	 * We can get here in the odd case where an LV is already active in
	 * a foreign VG, which allows the VG to be accessed by vgchange -a
	 * so the LV can be deactivated.
	 */
	if (vg->system_id && vg->system_id[0] &&
	    cmd->system_id && cmd->system_id[0] &&
	    strcmp(vg->system_id, cmd->system_id) &&
	    do_activate) {
		log_error("Cannot activate LVs in a foreign VG.");
		return 0;
	}

	if ((activate == CHANGE_AAY) && (vg->status & NOAUTOACTIVATE)) {
		log_debug("Autoactivation is disabled for VG %s.", vg->name);
		return 1;
	}

	if (do_activate && vg_complete_to_activate) {
		dm_list_iterate_items(pvl, &vg->pvs) {
			if (!pvl->pv->dev) {
				log_print("VG %s is incomplete.", vg->name);
				return 1;
			}
		}
	}

	/*
	 * Safe, since we never write out new metadata here. Required for
	 * partial activation to work.
	 */
        cmd->handles_missing_pvs = 1;

	/* FIXME: Force argument to deactivate them? */
	if (!do_activate) {
		dm_list_iterate_items(lvl, &vg->lvs)
			label_scan_invalidate_lv(cmd, lvl->lv);

	       	if ((lv_open = lvs_in_vg_opened(vg))) {
			dm_list_iterate_items(lvl, &vg->lvs) {
				if (lv_is_visible(lvl->lv) &&
				    !lv_is_vdo_pool(lvl->lv) && // FIXME: API skip flag missing
				    !lv_check_not_in_use(lvl->lv, 1)) {
					log_error("Can't deactivate volume group \"%s\" with %d open logical volume(s)",
						  vg->name, lv_open);
					return 0;
				}
			}
		}
	}

	/* FIXME Move into library where clvmd can use it */
	if (do_activate)
		check_current_backup(vg);
	else /* Component LVs might be active, support easy deactivation */
		cmd->process_component_lvs = 1;

	if (do_activate && (active = lvs_in_vg_activated(vg))) {
		log_verbose("%d logical volume(s) in volume group \"%s\" "
			    "already active", active, vg->name);
		if (dmeventd_monitor_mode() != DMEVENTD_MONITOR_IGNORE) {
			if (!_monitor_lvs_in_vg(cmd, vg, dmeventd_monitor_mode(), &monitored))
				r = 0;
			log_verbose("%d existing logical volume(s) in volume "
				    "group \"%s\" %smonitored",
				    monitored, vg->name,
				    dmeventd_monitor_mode() ? "" : "un");
		}
	}

	if (!_activate_lvs_in_vg(cmd, vg, activate)) {
		stack;
		r = 0;
	}

	/*
	 * Possibly trigger auto-generation of system.devices:
	 * - if root_dm_uuid contains vg->id, and
	 * - /etc/lvm/devices/auto-import-rootvg exists, and
	 * - /etc/lvm/devices/system.devices does not exist, then
	 * - create /run/lvm/lvm-devices-import to
	 *   trigger lvm-devices-import.path and .service
	 * - lvm-devices-import will run vgimportdevices --rootvg
	 *   to create system.devices
	 */
	if (root_dm_uuid) {
		char path[PATH_MAX];
		struct stat info;
		FILE *fp;

		if (memcmp(root_dm_uuid + 4, &vg->id, ID_LEN))
			goto out;

		if (cmd->enable_devices_file || devices_file_exists(cmd))
			goto out;

		if (dm_snprintf(path, sizeof(path), "%s/devices/auto-import-rootvg", cmd->system_dir) < 0)
			goto out;

		if (stat(path, &info) < 0)
			goto out;

		log_debug("Found %s creating %s", path, DEVICES_IMPORT_PATH);

		if (!(fp = fopen(DEVICES_IMPORT_PATH, "w"))) {
			log_debug("failed to create %s", DEVICES_IMPORT_PATH);
			goto out;
		}
		if (fclose(fp))
			stack;
	}
out:
	/* Print message only if there was not found a missing VG */
	log_print_unless_silent("%d logical volume(s) in volume group \"%s\" now active",
				lvs_in_vg_activated(vg), vg->name);
	return r;
}

static int _vgchange_refresh(struct cmd_context *cmd, struct volume_group *vg)
{
	log_verbose("Refreshing volume group \"%s\"", vg->name);

	if (!vg_refresh_visible(cmd, vg))
		return_0;

	return 1;
}

static int _vgchange_alloc(struct cmd_context *cmd, struct volume_group *vg)
{
	alloc_policy_t alloc;

	alloc = (alloc_policy_t) arg_uint_value(cmd, alloc_ARG, ALLOC_NORMAL);

	/* FIXME: make consistent with vg_set_alloc_policy() */
	if (alloc == vg->alloc) {
		log_error("Volume group allocation policy is already %s",
			  get_alloc_string(vg->alloc));
		return 0;
	}

	if (!vg_set_alloc_policy(vg, alloc))
		return_0;

	return 1;
}

static int _vgchange_resizeable(struct cmd_context *cmd,
				struct volume_group *vg)
{
	int resizeable = arg_int_value(cmd, resizeable_ARG, 0);

	if (resizeable && vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" is already resizeable",
			  vg->name);
		return 0;
	}

	if (!resizeable && !vg_is_resizeable(vg)) {
		log_error("Volume group \"%s\" is already not resizeable",
			  vg->name);
		return 0;
	}

	if (resizeable)
		vg->status |= RESIZEABLE_VG;
	else
		vg->status &= ~RESIZEABLE_VG;

	return 1;
}

static int _vgchange_autoactivation(struct cmd_context *cmd,
				    struct volume_group *vg)
{
	int aa_no_arg = !arg_int_value(cmd, setautoactivation_ARG, 0);
	int aa_no_meta = (vg->status & NOAUTOACTIVATE) ? 1 : 0;

	if ((aa_no_arg && aa_no_meta) || (!aa_no_arg && !aa_no_meta)) {
		log_error("Volume group autoactivation is already %s.",
			  aa_no_arg ? "no" : "yes");
		return 0;
	}

	if (aa_no_arg)
		vg->status |= NOAUTOACTIVATE;
	else
		vg->status &= ~NOAUTOACTIVATE;

	return 1;
}

static int _vgchange_logicalvolume(struct cmd_context *cmd,
				   struct volume_group *vg)
{
	uint32_t max_lv = arg_uint_value(cmd, logicalvolume_ARG, 0);

	if (!vg_set_max_lv(vg, max_lv))
		return_0;

	return 1;
}

static int _vgchange_physicalvolumes(struct cmd_context *cmd,
				     struct volume_group *vg)
{
	uint32_t max_pv = arg_uint_value(cmd, maxphysicalvolumes_ARG, 0);

	if (!vg_set_max_pv(vg, max_pv))
		return_0;

	return 1;
}

static int _vgchange_pesize(struct cmd_context *cmd, struct volume_group *vg)
{
	uint32_t extent_size;

	if (arg_uint64_value(cmd, physicalextentsize_ARG, 0) > MAX_EXTENT_SIZE) {
		log_warn("WARNING: Physical extent size cannot be larger than %s.",
			 display_size(cmd, (uint64_t) MAX_EXTENT_SIZE));
		return 1;
	}

	extent_size = arg_uint_value(cmd, physicalextentsize_ARG, 0);
	/* FIXME: remove check - redundant with vg_change_pesize */
	if (extent_size == vg->extent_size) {
		log_warn("WARNING: Physical extent size of VG %s is already %s.",
			 vg->name, display_size(cmd, (uint64_t) extent_size));
		return 1;
	}

	if (!vg_set_extent_size(vg, extent_size))
		return_0;

	if (!vg_check_pv_dev_block_sizes(vg)) {
		log_error("Failed to change physical extent size for VG %s.",
			   vg->name);
		return 0;
	}

	return 1;
}

static int _vgchange_addtag(struct cmd_context *cmd, struct volume_group *vg)
{
	return change_tag(cmd, vg, NULL, NULL, addtag_ARG);
}

static int _vgchange_deltag(struct cmd_context *cmd, struct volume_group *vg)
{
	return change_tag(cmd, vg, NULL, NULL, deltag_ARG);
}

static int _vgchange_uuid(struct cmd_context *cmd __attribute__((unused)),
			  struct volume_group *vg)
{
	struct lv_list *lvl;
	struct id old_vg_id;

	if (lvs_in_vg_activated(vg)) {
		log_error("Volume group has active logical volumes.");
		return 0;
	}

	memcpy(&old_vg_id, &vg->id, ID_LEN);

	if (!id_create(&vg->id)) {
		log_error("Failed to generate new random UUID for VG %s.",
			  vg->name);
		return 0;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		memcpy(&lvl->lv->lvid, &vg->id, sizeof(vg->id));
	}

	/*
	 * If any LVs in this VG have PVs stacked on them, then
	 * update the device_id of the stacked PV.
	 */
	device_id_update_vg_uuid(cmd, vg, &old_vg_id);

	return 1;
}

static int _vgchange_metadata_copies(struct cmd_context *cmd,
				     struct volume_group *vg)
{
	uint32_t mda_copies = arg_uint_value(cmd, vgmetadatacopies_ARG, DEFAULT_VGMETADATACOPIES);

	log_debug("vgchange_metadata_copies new %u vg_mda_copies %u D %u",
		  mda_copies, vg_mda_copies(vg), DEFAULT_VGMETADATACOPIES);

	if (mda_copies == vg_mda_copies(vg)) {
		if (vg_mda_copies(vg) == VGMETADATACOPIES_UNMANAGED)
			log_warn("WARNING: Number of metadata copies for VG %s is already unmanaged.",
				 vg->name);
		else
			log_warn("WARNING: Number of metadata copies for VG %s is already %u.",
				 vg->name, mda_copies);
		return 1;
	}

	if (!vg_set_mda_copies(vg, mda_copies))
		return_0;

	return 1;
}

static int _vgchange_profile(struct cmd_context *cmd,
			     struct volume_group *vg)
{
	const char *old_profile_name, *new_profile_name;
	struct profile *new_profile;

	old_profile_name = vg->profile ? vg->profile->name : "(no profile)";

	if (arg_is_set(cmd, detachprofile_ARG)) {
		new_profile_name = "(no profile)";
		vg->profile = NULL;
	} else {
		if (arg_is_set(cmd, metadataprofile_ARG))
			new_profile_name = arg_str_value(cmd, metadataprofile_ARG, NULL);
		else
			new_profile_name = arg_str_value(cmd, profile_ARG, NULL);
		if (!(new_profile = add_profile(cmd, new_profile_name, CONFIG_PROFILE_METADATA)))
			return_0;
		vg->profile = new_profile;
	}

	log_verbose("Changing configuration profile for VG %s: %s -> %s.",
		    vg->name, old_profile_name, new_profile_name);

	return 1;
}

/*
 * This function will not be called unless the local host is allowed to use the
 * VG.  Either the VG has no system_id, or the VG and host have matching
 * system_ids, or the host has the VG's current system_id in its
 * extra_system_ids list.  This function is not allowed to change the system_id
 * of a foreign VG (VG owned by another host).
 */
static int _vgchange_system_id(struct cmd_context *cmd, struct volume_group *vg)
{
	const char *system_id;
	const char *system_id_arg_str = arg_str_value(cmd, systemid_ARG, NULL);

	if (!(system_id = system_id_from_string(cmd, system_id_arg_str))) {
		log_error("Unable to set system ID.");
		return 0;
	}

	if (!strcmp(vg->system_id, system_id)) {
		log_error("Volume Group system ID is already \"%s\".", vg->system_id);
		return 0;
	}

	if (!*system_id && cmd->system_id && strcmp(system_id, cmd->system_id)) {
		log_warn("WARNING: Removing the system ID allows unsafe access from other hosts.");

		if (!arg_is_set(cmd, yes_ARG) &&
		    yes_no_prompt("Remove system ID %s from volume group %s? [y/n]: ",
				  vg->system_id, vg->name) == 'n') {
			log_error("System ID of volume group %s not changed.", vg->name);
			return 0;
		}
	}

	if (*system_id && (!cmd->system_id || strcmp(system_id, cmd->system_id))) {
		if (lvs_in_vg_activated(vg)) {
			log_error("Logical Volumes in VG %s must be deactivated before system ID can be changed.",
				  vg->name);
			return 0;
		}

		if (cmd->system_id)
			log_warn("WARNING: Requested system ID %s does not match local system ID %s.",
				 system_id, cmd->system_id ? : "");
		else
			log_warn("WARNING: No local system ID is set.");
		log_warn("WARNING: Volume group %s might become inaccessible from this machine.",
			 vg->name);

		if (!arg_is_set(cmd, yes_ARG) &&
		    yes_no_prompt("Set foreign system ID %s on volume group %s? [y/n]: ",
				  system_id, vg->name) == 'n') {
			log_error("Volume group %s system ID not changed.", vg->name);
			return 0;
		}
	}

	log_verbose("Changing system ID for VG %s from \"%s\" to \"%s\".",
		    vg->name, vg->system_id, system_id);

	vg->system_id = system_id;
	
	return 1;
}

static int _passes_lock_start_filter(struct cmd_context *cmd,
				     struct volume_group *vg,
				     const int cfg_id)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	const char *str;

	/* undefined list means no restrictions, all vg names pass */

	cn = find_config_tree_array(cmd, cfg_id, NULL);
	if (!cn)
		return 1;

	/* with a defined list, the vg name must be included to pass */

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type == DM_CFG_EMPTY_ARRAY)
			break;
		if (cv->type != DM_CFG_STRING) {
			log_error("Ignoring invalid string in lock_start list.");
			continue;
		}
		str = cv->v.str;
		if (!*str) {
			log_error("Ignoring empty string in config file.");
			continue;
		}

		/* ignoring tags for now */

		if (!strcmp(str, vg->name))
			return 1;
	}

	return 0;
}

static int _vgchange_lock_start(struct cmd_context *cmd, struct volume_group *vg,
				struct vgchange_params *vp)
{
	int auto_opt = 0;
	int exists = 0;
	int r;

	if (!vg_is_shared(vg))
		return 1;

	if (arg_is_set(cmd, force_ARG))
		goto do_start;

	/*
	 * Any waiting is done at the end of vgchange.
	 */
	if ((cmd->lockopt & LOCKOPT_AUTO) || (cmd->lockopt & LOCKOPT_AUTONOWAIT))
		auto_opt = 1;

	if (!_passes_lock_start_filter(cmd, vg, activation_lock_start_list_CFG)) {
		log_verbose("Not starting %s since it does not pass lock_start_list", vg->name);
		return 1;
	}

	if (auto_opt && !_passes_lock_start_filter(cmd, vg, activation_auto_lock_start_list_CFG)) {
		log_verbose("Not starting %s since it does not pass auto_lock_start_list", vg->name);
		return 1;
	}

do_start:
	r = lockd_start_vg(cmd, vg, &exists);

	if (r)
		vp->lock_start_count++;
	else if (exists)
		vp->lock_start_count++;
	if (!strcmp(vg->lock_type, "sanlock"))
		vp->lock_start_sanlock = 1;

	return r;
}

static int _vgchange_lock_stop(struct cmd_context *cmd, struct volume_group *vg)
{
	return lockd_stop_vg(cmd, vg);
}

static int _vgchange_single(struct cmd_context *cmd, const char *vg_name,
			    struct volume_group *vg,
			    struct processing_handle *handle)
{
	struct vgchange_params *vp = (struct vgchange_params *)handle->custom_handle;
	int ret = ECMD_PROCESSED;
	unsigned i;
	activation_change_t activate;
	int changed = 0;

	static const struct {
		int arg;
		int (*fn)(struct cmd_context *cmd, struct volume_group *vg);
	} _vgchange_args[] = {
		{ logicalvolume_ARG, &_vgchange_logicalvolume },
		{ maxphysicalvolumes_ARG, &_vgchange_physicalvolumes },
		{ resizeable_ARG, &_vgchange_resizeable },
		{ setautoactivation_ARG, &_vgchange_autoactivation },
		{ deltag_ARG, &_vgchange_deltag },
		{ addtag_ARG, &_vgchange_addtag },
		{ physicalextentsize_ARG, &_vgchange_pesize },
		{ uuid_ARG, &_vgchange_uuid },
		{ alloc_ARG, &_vgchange_alloc },
		{ vgmetadatacopies_ARG, &_vgchange_metadata_copies },
		{ metadataprofile_ARG, &_vgchange_profile },
		{ profile_ARG, &_vgchange_profile },
		{ detachprofile_ARG, &_vgchange_profile },
	};

	/*
	 * FIXME: DEFAULT_BACKGROUND_POLLING should be "unspecified".
	 * If --poll is explicitly provided use it; otherwise polling
	 * should only be started if the LV is not already active. So:
	 * 1) change the activation code to say if the LV was actually activated
	 * 2) make polling of an LV tightly coupled with LV activation
	 *
	 * Do not initiate any polling if --sysinit option is used.
	 */
	init_background_polling(arg_is_set(cmd, sysinit_ARG) ? 0 :
						arg_int_value(cmd, poll_ARG,
						DEFAULT_BACKGROUND_POLLING));

	for (i = 0; i < DM_ARRAY_SIZE(_vgchange_args); ++i) {
		if (arg_is_set(cmd, _vgchange_args[i].arg)) {
			if (!_vgchange_args[i].fn(cmd, vg))
				return_ECMD_FAILED;
			changed = 1;
		}
	}

	if (changed) {
		if (!vg_write(vg) || !vg_commit(vg))
			return_ECMD_FAILED;

		log_print_unless_silent("Volume group \"%s\" successfully changed.", vg->name);
	}

	if (arg_is_set(cmd, activate_ARG)) {
		activate = (activation_change_t) arg_uint_value(cmd, activate_ARG, 0);
		if (!vgchange_activate(cmd, vg, activate, vp->vg_complete_to_activate, vp->root_dm_uuid))
			return_ECMD_FAILED;
	} else if (arg_is_set(cmd, refresh_ARG)) {
		/* refreshes the visible LVs (which starts polling) */
		if (!_vgchange_refresh(cmd, vg))
			return_ECMD_FAILED;
	} else {
		/* -ay* will have already done monitoring changes */
		if (arg_is_set(cmd, monitor_ARG) &&
		    !_vgchange_monitoring(cmd, vg))
			return_ECMD_FAILED;

		/* When explicitly specified --poll */
		if (arg_is_set(cmd, poll_ARG) &&
		    !vgchange_background_polling(cmd, vg))
			return_ECMD_FAILED;
	}

	return ret;
}

/*
 * Automatic creation of system.devices for root VG on first boot
 * is useful for OS images where the OS installer is not used to
 * customize the OS for system.
 *
 * - OS image prep:
 *   . rm /etc/lvm/devices/system.devices (if it exists)
 *   . touch /etc/lvm/devices/auto-import-rootvg
 *   . enable lvm-devices-import.path
 *   . enable lvm-devices-import.service
 *
 * - lvchange -ay <rootvg>/<rootlv>
 *   . run by initrd so root fs can be mounted
 *   . does not use system.devices
 *   . named <rootvg>/<rootlv> comes from kernel command line rd.lvm
 *   . uses first device that appears containing the named root LV
 *
 * - vgchange -aay <rootvg>
 *   . triggered by udev when all PVs from root VG are online
 *   . activate LVs in root VG (in addition to the already active root LV)
 *   . check for /etc/lvm/devices/auto-import-rootvg (found)
 *   . check for /etc/lvm/devices/system.devices (not found)
 *   . create /run/lvm/lvm-devices-import because
 *     auto-import-rootvg was found and system.devices was not found
 *
 * - lvm-devices-import.path
 *   . triggered by /run/lvm/lvm-devices-import
 *   . start lvm-devices-import.service
 *
 * - lvm-devices-import.service
 *   . check for /etc/lvm/devices/system.devices, do nothing if found
 *   . run vgimportdevices --rootvg --auto
 *
 * - vgimportdevices --rootvg --auto
 *   . check for /etc/lvm/devices/auto-import-rootvg (found)
 *   . check for /etc/lvm/devices/system.devices (not found)
 *   . creates /etc/lvm/devices/system.devices for PVs in root VG
 *   . removes /etc/lvm/devices/auto-import-rootvg
 *   . removes /run/lvm/lvm-devices-import
 *
 * On future startup, /etc/lvm/devices/system.devices will exist,
 * and /etc/lvm/devices/auto-import-rootvg will not exist, so
 * vgchange -aay <rootvg> will not create /run/lvm/lvm-devices-import,
 * and lvm-devices-import.path and lvm-device-import.service will not run.
 *
 * lvm-devices-import.path:
 * [Path]
 * PathExists=/run/lvm/lvm-devices-import
 * Unit=lvm-devices-import.service
 * ConditionPathExists=!/etc/lvm/devices/system.devices
 *
 * lvm-devices-import.service:
 * [Service]
 * Type=oneshot
 * RemainAfterExit=no
 * ExecStart=/usr/sbin/vgimportdevices --rootvg --auto
 * ConditionPathExists=!/etc/lvm/devices/system.devices
 */
static void _get_rootvg_dev(struct cmd_context *cmd, char **dm_uuid_out)
{
	char path[PATH_MAX];
	struct stat info;

	if (cmd->enable_devices_file || devices_file_exists(cmd))
		return;

	if (dm_snprintf(path, sizeof(path), "%s/devices/auto-import-rootvg", cmd->system_dir) < 0)
		return;

	if (stat(path, &info) < 0)
		return;

	if (!get_rootvg_dev_uuid(cmd, dm_uuid_out))
		stack;
}

static int _vgchange_autoactivation_setup(struct cmd_context *cmd,
					  struct vgchange_params *vp,
					  int *skip_command,
					  const char **vgname_ret,
					  uint32_t *flags)
{
	const char *aa;
	char *vgname = NULL;
	int vg_locked = 0;
	int found_none = 0, found_all = 0, found_incomplete = 0;

	if (!(aa = arg_str_value(cmd, autoactivation_ARG, NULL)))
		return_0;

	if (strcmp(aa, "event")) {
		log_print("Skip vgchange for unknown autoactivation value.");
		*skip_command = 1;
		return 1;
	}

	if (!find_config_tree_bool(cmd, global_event_activation_CFG, NULL)) {
		log_print("Skip vgchange for event and event_activation=0.");
		*skip_command = 1;
		return 1;
	}

	vp->vg_complete_to_activate = 1;
	cmd->use_hints = 0;

	/*
	 * Add an option to skip the pvs_online optimization? e.g.
	 * "online_skip" in --autoactivation / auto_activation_settings
	 *
	 * if (online_skip)
	 *	return 1;
	 */

	/* reads devices file, does not populate dev-cache */
	if (!setup_devices_for_online_autoactivation(cmd))
		return_0;

	get_single_vgname_cmd_arg(cmd, NULL, &vgname);

	_get_rootvg_dev(cmd, &vp->root_dm_uuid);

	/*
	 * Lock the VG before scanning the PVs so _vg_read can avoid the normal
	 * lock_vol+rescan (READ_WITHOUT_LOCK avoids the normal lock_vol and
	 * can_use_one_scan avoids the normal rescan.)  If this early lock_vol
	 * fails, continue and use the normal lock_vol in _vg_read.
	 */
	if (vgname) {
		if (!lock_vol(cmd, vgname, LCK_VG_WRITE, NULL)) {
			log_debug("Failed early VG locking for autoactivation.");
		} else {
			*flags |= READ_WITHOUT_LOCK;
			cmd->can_use_one_scan = 1;
			vg_locked = 1;
		}
	}

	/*
	 * Perform label_scan on PVs that are online (per /run/lvm files)
	 * for the given VG (or when no VG name is given, all online PVs.)
	 * If this fails, the caller will do a normal process_each_vg without
	 * optimizations (which will do a full label_scan.)
	 */
	if (!label_scan_vg_online(cmd, vgname, &found_none, &found_all, &found_incomplete)) {
		log_print("PVs online error%s%s, using all devices.", vgname ? " for VG " : "", vgname ?: "");
		goto bad;
	}

	/*
	 * Not the expected usage, activate any VGs that are complete based on
	 * pvs_online.  Only online pvs are used.
	 */
	if (!vgname) {
		*flags |= PROCESS_SKIP_SCAN;
		return 1;
	}

	/*
	 * The expected and optimal usage, which is the purpose of
	 * this function.  We expect online files to be found for
	 * all PVs because the udev rule calls
	 * vgchange -aay --autoactivation event <vgname>
	 * only after all PVs for vgname are found online.
	 */
	if (found_all) {
		*flags |= PROCESS_SKIP_SCAN;
		*vgname_ret = vgname;
		return 1;
	}

	/*
	 * Not expected usage, no online pvs for the vgname were found.  The
	 * caller will fall back to process_each doing a full label_scan to
	 * look for the VG.  (No optimization used.)
	 */
	if (found_none) {
		log_print("PVs online not found for VG %s, using all devices.", vgname);
		goto bad;
	}

	/*
	 * Not expected usage, only some online pvs for the vgname were found.
	 * The caller will fall back to process_each doing a full label_scan to
	 * look for all PVs in the VG.  (No optimization used.)
	 */
	if (found_incomplete) {
		log_print("PVs online incomplete for VG %s, using all devices.", vgname);
		goto bad;
	}

	/*
	 * Shouldn't happen, the caller will fall back to standard
	 * process_each.  (No optimization used.)
	 */
	log_print("PVs online unknown for VG %s, using all devices.", vgname);

 bad:
	/*
	 * The online scanning optimization didn't work, so undo the vg
	 * locking optimization before falling back to normal processing.
	 */
	if (vg_locked) {
		unlock_vg(cmd, NULL, vgname);
		*flags &= ~READ_WITHOUT_LOCK;
		cmd->can_use_one_scan = 0;
	}

	free(vgname);

	return 1;

}

int vgchange(struct cmd_context *cmd, int argc, char **argv)
{
	struct vgchange_params vp = { 0 };
	struct processing_handle *handle;
	const char *vgname = NULL;
	uint32_t flags = 0;
	int ret;

	int noupdate =
		arg_is_set(cmd, activate_ARG) ||
		arg_is_set(cmd, monitor_ARG) ||
		arg_is_set(cmd, poll_ARG) ||
		arg_is_set(cmd, refresh_ARG);

	int update_partial_safe =
		arg_is_set(cmd, deltag_ARG) ||
		arg_is_set(cmd, addtag_ARG) ||
		arg_is_set(cmd, metadataprofile_ARG) ||
		arg_is_set(cmd, profile_ARG) ||
		arg_is_set(cmd, detachprofile_ARG);

	int update_partial_unsafe =
		arg_is_set(cmd, logicalvolume_ARG) ||
		arg_is_set(cmd, maxphysicalvolumes_ARG) ||
		arg_is_set(cmd, resizeable_ARG) ||
		arg_is_set(cmd, setautoactivation_ARG) ||
		arg_is_set(cmd, uuid_ARG) ||
		arg_is_set(cmd, physicalextentsize_ARG) ||
		arg_is_set(cmd, alloc_ARG) ||
		arg_is_set(cmd, vgmetadatacopies_ARG);

	int update = update_partial_safe || update_partial_unsafe;

	if (!update && !noupdate) {
		log_error("Need one or more command options.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_is_set(cmd, profile_ARG) || arg_is_set(cmd, metadataprofile_ARG)) &&
	     arg_is_set(cmd, detachprofile_ARG)) {
		log_error("Only one of --metadataprofile and --detachprofile permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, activate_ARG) && arg_is_set(cmd, refresh_ARG)) {
		log_error("Only one of -a and --refresh permitted.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_is_set(cmd, ignorelockingfailure_ARG) ||
	     arg_is_set(cmd, sysinit_ARG)) && update) {
		log_error("Only -a permitted with --ignorelockingfailure and --sysinit.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, activate_ARG) &&
	    (arg_is_set(cmd, monitor_ARG) || arg_is_set(cmd, poll_ARG))) {
		if (!is_change_activating((activation_change_t) arg_uint_value(cmd, activate_ARG, 0))) {
			log_error("Only -ay* allowed with --monitor or --poll.");
			return EINVALID_CMD_LINE;
		}
	}

	if (arg_is_set(cmd, poll_ARG) && arg_is_set(cmd, sysinit_ARG)) {
		log_error("Only one of --poll and --sysinit permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, maxphysicalvolumes_ARG) &&
	    arg_sign_value(cmd, maxphysicalvolumes_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("MaxPhysicalVolumes may not be negative.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, physicalextentsize_ARG) &&
	    arg_sign_value(cmd, physicalextentsize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical extent size may not be negative.");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, clustered_ARG) && !argc && !arg_is_set(cmd, yes_ARG) &&
	    (yes_no_prompt("Change clustered property of all volumes groups? [y/n]: ") == 'n')) {
		log_error("No volume groups changed.");
		return ECMD_FAILED;
	}

	if (!update || !update_partial_unsafe)
		cmd->handles_missing_pvs = 1;

	if (noupdate)
		cmd->ignore_device_name_mismatch = 1;

	/*
	 * If the devices file includes PVs stacked on LVs, then
	 * vgchange --uuid may need to update the devices file.
	 * No PV-on-LV stacked is done without scan_lvs set.
	 */
	if (arg_is_set(cmd, uuid_ARG) && cmd->scan_lvs)
		cmd->edit_devices_file = 1;

	/*
	 * Include foreign VGs that contain active LVs.
	 * That shouldn't happen in general, but if it does by some
	 * mistake, then we want to allow those LVs to be deactivated.
	 */
	if (arg_is_set(cmd, activate_ARG))
		cmd->include_active_foreign_vgs = 1;

	/* The default vg lock mode is ex, but these options only need sh. */
	if ((cmd->command->command_enum == vgchange_activate_CMD) ||
	    (cmd->command->command_enum == vgchange_refresh_CMD)) {
		cmd->lockd_vg_default_sh = 1;
		/* Allow deactivating if locks fail. */
		if (is_change_activating((activation_change_t)arg_uint_value(cmd, activate_ARG, CHANGE_AY)))
			cmd->lockd_vg_enforce_sh = 1;
	}

	if (arg_is_set(cmd, autoactivation_ARG)) {
		int skip_command = 0;
		if (!_vgchange_autoactivation_setup(cmd, &vp, &skip_command, &vgname, &flags))
			return ECMD_FAILED;
		if (skip_command)
			return ECMD_PROCESSED;
	}

	/*
	 * Do not use udev for device listing or device info because
	 * vgchange --monitor y is called during boot when udev is being
	 * initialized and is not yet ready to be used.
	 */
	if (arg_is_set(cmd, monitor_ARG) &&
	    arg_int_value(cmd, monitor_ARG, DEFAULT_DMEVENTD_MONITOR)) {
		init_obtain_device_list_from_udev(0);
		init_external_device_info_source(DEV_EXT_NONE);
	}

	if (update)
		flags |= READ_FOR_UPDATE;
	else if (arg_is_set(cmd, activate_ARG) ||
		 arg_is_set(cmd, refresh_ARG))
		flags |= READ_FOR_ACTIVATE;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = &vp;

	ret = process_each_vg(cmd, argc, argv, vgname, NULL, flags, 0, handle, &_vgchange_single);

	destroy_processing_handle(cmd, handle);
	return ret;
}

static int _vgchange_locktype(struct cmd_context *cmd, struct volume_group *vg, int *no_change)
{
	const char *lock_type = arg_str_value(cmd, locktype_ARG, NULL);
	struct lv_list *lvl;
	struct logical_volume *lv;
	int lv_lock_count = 0;

	/* Special recovery case. */
	if (lock_type && !strcmp(lock_type, "none") && (cmd->lockopt & LOCKOPT_FORCE)) {
		vg->status &= ~CLUSTERED;
		vg->lock_type = "none";
		vg->lock_args = NULL;

		dm_list_iterate_items(lvl, &vg->lvs)
			lvl->lv->lock_args = NULL;

		return 1;
	}

	if (!vg->lock_type) {
		if (vg_is_clustered(vg))
			vg->lock_type = "clvm";
		else
			vg->lock_type = "none";
	}

	if (lock_type && !strcmp(vg->lock_type, lock_type)) {
		log_warn("WARNING: New lock type %s matches the current lock type %s.",
			 lock_type, vg->lock_type);
		*no_change = 1;
		return 1;
	}

	if (is_lockd_type(vg->lock_type) && is_lockd_type(lock_type)) {
		log_error("Cannot change lock type directly from \"%s\" to \"%s\".",
			  vg->lock_type, lock_type);
		log_error("First change lock type to \"none\", then to \"%s\".",
			  lock_type);
		return 0;
	}

	/*
	 * When lvm is currently using lvmlockd, this function can:
	 * - change none to lockd type
	 * - change none to clvm (with warning about not being able to use it)
	 * - change lockd type to none
	 * - change lockd type to clvm (with warning about not being able to use it)
	 * - change clvm to none
	 * - change clvm to lockd type
	 */

	if (lvs_in_vg_activated(vg)) {
		log_error("Changing VG %s lock type not allowed with active LVs",
			  vg->name);
		return 0;
	}

	/* clvm to none */
	if (lock_type && !strcmp(vg->lock_type, "clvm") && !strcmp(lock_type, "none")) {
		vg->status &= ~CLUSTERED;
		vg->lock_type = "none";
		return 1;
	}

	/* clvm to ..., first undo clvm */
	if (!strcmp(vg->lock_type, "clvm")) {
		vg->status &= ~CLUSTERED;
	}

	/*
	 * lockd type to ..., first undo lockd type
	 */
	if (is_lockd_type(vg->lock_type)) {
		if (!lockd_free_vg_before(cmd, vg, 1, 0))
			return 0;

		lockd_free_vg_final(cmd, vg);

		vg->status &= ~CLUSTERED;
		vg->lock_type = "none";
		vg->lock_args = NULL;

		dm_list_iterate_items(lvl, &vg->lvs)
			lvl->lv->lock_args = NULL;
	}

	/* ... to lockd type */
	if (is_lockd_type(lock_type)) {
		/*
		 * For lock_type dlm, lockd_init_vg() will do a single
		 * vg_write() that sets lock_type, sets lock_args, clears
		 * system_id, and sets all LV lock_args to dlm.
		 * For lock_type sanlock, lockd_init_vg() needs to know
		 * how many LV locks are needed so that it can make the
		 * sanlock lv large enough.
		 */
		dm_list_iterate_items(lvl, &vg->lvs) {
			lv = lvl->lv;

			if (lockd_lv_uses_lock(lv)) {
				lv_lock_count++;

				if (!strcmp(lock_type, "dlm"))
					lv->lock_args = "dlm";
			}
		}

		/*
		 * See below.  We cannot set valid LV lock_args until stage 1
		 * of the change is done, so we need to skip the validation of
		 * the lock_args during stage 1.
		 */
		if (!strcmp(lock_type, "sanlock"))
			vg->skip_validate_lock_args = 1;

		vg->system_id = NULL;

		if (!lockd_init_vg(cmd, vg, lock_type, lv_lock_count)) {
			log_error("Failed to initialize lock args for lock type %s", lock_type);
			return 0;
		}

		/*
		 * For lock_type sanlock, there must be multiple steps
		 * because the VG needs an active lvmlock LV before
		 * LV lock areas can be allocated, which must be done
		 * before LV lock_args are written.  So, the LV lock_args
		 * remain unset during the first stage of the conversion.
		 *
		 * Stage 1:
		 * lockd_init_vg() creates and activates the lvmlock LV,
		 * then sets lock_type, sets lock_args, and clears system_id.
		 *
		 * Stage 2:
		 * We get here, and can now set LV lock_args.  This uses
		 * the standard code path for allocating LV locks in
		 * vg_write() by setting LV lock_args to "pending",
		 * which tells vg_write() to call lockd_init_lv()
		 * and sets the lv->lock_args value before writing the VG.
		 */
		if (!strcmp(lock_type, "sanlock")) {
			dm_list_iterate_items(lvl, &vg->lvs) {
				lv = lvl->lv;
				if (lockd_lv_uses_lock(lv))
					lv->lock_args = "pending";
			}

			vg->skip_validate_lock_args = 0;
		}

		return 1;
	}

	/* ... to none */
	if (lock_type && !strcmp(lock_type, "none")) {
		vg->lock_type = NULL;
		vg->system_id = cmd->system_id ? dm_pool_strdup(vg->vgmem, cmd->system_id) : NULL;
		return 1;
	}

	log_error("Cannot change to unknown lock type %s", lock_type);
	return 0;
}

static int _vgchange_locktype_single(struct cmd_context *cmd, const char *vg_name,
			             struct volume_group *vg,
			             struct processing_handle *handle)
{
	int no_change = 0;

	if (!_vgchange_locktype(cmd, vg, &no_change))
		return_ECMD_FAILED;

	if (no_change)
		return ECMD_PROCESSED;

	if (!vg_write(vg) || !vg_commit(vg))
		return_ECMD_FAILED;

	/*
	 * When init_vg_sanlock is called for vgcreate, the lockspace remains
	 * started and lvmlock remains active, but when called for
	 * vgchange --locktype sanlock, the lockspace is not started so the
	 * lvmlock LV should be deactivated at the end.  vg_write writes the
	 * new leases to lvmlock, so we need to wait until after vg_write to
	 * deactivate it.
	 */
	if (vg->lock_type && !strcmp(vg->lock_type, "sanlock") &&
	    (cmd->command->command_enum == vgchange_locktype_CMD)) {
		if (!deactivate_lv(cmd, vg->sanlock_lv)) {
			log_error("Failed to deactivate %s.",
				  display_lvname(vg->sanlock_lv));
			return ECMD_FAILED;
		}
	}

	log_print_unless_silent("Volume group \"%s\" successfully changed.", vg->name);

	return ECMD_PROCESSED;
}

int vgchange_locktype_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	const char *lock_type = arg_str_value(cmd, locktype_ARG, NULL);
	int ret;


	/*
	 * vgchange --locktype none --lockopt force VG
	 *
	 * This is a special/forced exception to change the lock type to none.
	 * It's needed for recovery cases and skips the normal steps of undoing
	 * the current lock type.  It's a way to forcibly get access to a VG
	 * when the normal locking mechanisms are not working.
	 *
	 * It ignores: the current lvm locking config, lvmlockd, the state of
	 * the vg on other hosts, etc.  It is meant to just remove any locking
	 * related metadata from the VG (cluster/lock_type flags, lock_type,
	 * lock_args).
	 *
	 * This can be necessary when manually recovering from certain failures.
	 * e.g. when a pv is lost containing the lvmlock lv (holding sanlock
	 * leases), the vg lock_type needs to be changed to none, and then
	 * back to sanlock, which recreates the lvmlock lv and leases.
	 *
	 * Set lockd_gl_disable, lockd_vg_disable, lockd_lv_disable to
	 * disable locking.  lockd_gl(), lockd_vg() and lockd_lv() will
	 * just return success when they see the disable flag set.
	 */
	if (cmd->lockopt & LOCKOPT_FORCE) {
		if (!arg_is_set(cmd, yes_ARG) &&
		     yes_no_prompt("Forcibly change VG lock type to %s? [y/n]: ", lock_type) == 'n') {
			log_error("VG lock type not changed.");
			return 0;
		}

		cmd->lockd_gl_disable = 1;
		cmd->lockd_vg_disable = 1;
		cmd->lockd_lv_disable = 1;
		cmd->handles_missing_pvs = 1;
		cmd->force_access_clustered = 1;
		goto process;
	}

	if (!lvmlockd_use()) {
		log_error("Using lock type requires lvmlockd.");
		return 0;
	}

	/*
	 * This is a special case where taking the global lock is
	 * not needed to protect global state, because the change is
	 * only to an existing VG.  But, taking the global lock ex is
	 * helpful in this case to trigger a global cache validation
	 * on other hosts, to cause them to see the new system_id or
	 * lock_type.
	 */
	if (!lockd_global(cmd, "ex"))
		return 0;

process:
	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	ret = process_each_vg(cmd, argc, argv, NULL, NULL, READ_FOR_UPDATE, 0, handle, &_vgchange_locktype_single);

	destroy_processing_handle(cmd, handle);
	return ret;
}

static int _vgchange_lock_start_stop_single(struct cmd_context *cmd, const char *vg_name,
					    struct volume_group *vg,
					    struct processing_handle *handle)
{
	struct vgchange_params *vp = (struct vgchange_params *)handle->custom_handle;

	if (arg_is_set(cmd, lockstart_ARG)) {
		if (!_vgchange_lock_start(cmd, vg, vp))
			return_ECMD_FAILED;
	} else if (arg_is_set(cmd, lockstop_ARG)) {
		if (!_vgchange_lock_stop(cmd, vg))
			return_ECMD_FAILED;
	}

	return ECMD_PROCESSED;
}

int vgchange_lock_start_stop_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	struct vgchange_params vp = { 0 };
	int ret;

	if (!lvmlockd_use()) {
		log_error("Using lock start and lock stop requires lvmlockd.");
		return 0;
	}

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	if (arg_is_set(cmd, lockstop_ARG))
		cmd->lockd_vg_default_sh = 1;

	/*
	 * Starting lockspaces.  For VGs not yet started, locks are not
	 * available to acquire, and for VGs already started, there's nothing
	 * to do, so disable VG locks.  Try to acquire the global lock sh to
	 * validate the cache (if no gl is available, lockd_gl will force a
	 * cache validation).  If the global lock is available, it can be
	 * beneficial to hold sh to serialize lock-start with vgremove of the
	 * same VG from another host.
	 */
	if (arg_is_set(cmd, lockstart_ARG)) {
		cmd->lockd_vg_disable = 1;

		if (!lockd_global(cmd, "sh"))
			log_debug("No global lock for lock start.");

		/* Disable the lockd_gl in process_each_vg. */
		cmd->lockd_gl_disable = 1;
	} else {
		/* If the VG was started when it was exported, allow it to be stopped. */
		cmd->include_exported_vgs = 1;
	}

	handle->custom_handle = &vp;

	ret = process_each_vg(cmd, argc, argv, NULL, NULL, 0, 0, handle, &_vgchange_lock_start_stop_single);

	/* Wait for lock-start ops that were initiated in vgchange_lockstart. */

	if (arg_is_set(cmd, lockstart_ARG) && vp.lock_start_count) {
		if (!lockd_global(cmd, "un"))
			stack;

		if ((cmd->lockopt & LOCKOPT_NOWAIT) || (cmd->lockopt & LOCKOPT_AUTONOWAIT)) {
			log_print_unless_silent("Starting locking.  VG can only be read until locks are ready.");
		} else {
			if (vp.lock_start_sanlock)
				log_print_unless_silent("Starting locking.  Waiting for sanlock may take a few seconds to 3 min...");
			else
				log_print_unless_silent("Starting locking.  Waiting until locks are ready...");
			lockd_start_wait(cmd);
		}
	}

	destroy_processing_handle(cmd, handle);
	return ret;
}

static int _vgchange_systemid_single(struct cmd_context *cmd, const char *vg_name,
			             struct volume_group *vg,
			             struct processing_handle *handle)
{
	if (arg_is_set(cmd, majoritypvs_ARG)) {
		struct pv_list *pvl;
		int missing_pvs = 0;
		int found_pvs = 0;

		dm_list_iterate_items(pvl, &vg->pvs) {
			if (!pvl->pv->dev)
				missing_pvs++;
			else
				found_pvs++;
		}
		if (found_pvs <= missing_pvs) {
			log_error("Cannot change system ID without the majority of PVs (found %d of %d).",
				  found_pvs, found_pvs+missing_pvs);
			return ECMD_FAILED;
		}
	}

	if (!_vgchange_system_id(cmd, vg))
		return_ECMD_FAILED;

	if (!vg_write(vg) || !vg_commit(vg))
		return_ECMD_FAILED;

	log_print_unless_silent("Volume group \"%s\" successfully changed.", vg->name);

	return ECMD_PROCESSED;
}

int vgchange_systemid_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct processing_handle *handle;
	int ret;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	if (arg_is_set(cmd, majoritypvs_ARG))
		cmd->handles_missing_pvs = 1;

	ret = process_each_vg(cmd, argc, argv, NULL, NULL, READ_FOR_UPDATE, 0, handle, &_vgchange_systemid_single);

	destroy_processing_handle(cmd, handle);
	return ret;
}

