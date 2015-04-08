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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "tools.h"

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
	struct lvinfo info;
	int lv_active;
	int count = 0;

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;

		if (!lv_info(cmd, lv, 0, &info, 0, 0))
			lv_active = 0;
		else
			lv_active = info.exists;

		if (lv_active &&
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
	int count = 0, expected_count = 0;

	sigint_allow();
	dm_list_iterate_items(lvl, &vg->lvs) {
		if (sigint_caught())
			return_0;

		lv = lvl->lv;

		if (!lv_is_visible(lv))
			continue;

		/* If LV is sparse, activate origin instead */
		if (lv_is_cow(lv) && lv_is_virtual_origin(origin_from_cow(lv)))
			lv = origin_from_cow(lv);

		/* Only request activation of snapshot origin devices */
		if ((lv->status & SNAPSHOT) || lv_is_cow(lv))
			continue;

		/* Only request activation of mirror LV */
		if ((lv->status & MIRROR_IMAGE) || (lv->status & MIRROR_LOG))
			continue;

		/* Only request activation of the first replicator-dev LV */
		/* Avoids retry with all heads in case of failure */
		if (lv_is_replicator_dev(lv) && (lv != first_replicator_dev(lv)))
			continue;

		if (lv_activation_skip(lv, activate, arg_count(cmd, ignoreactivationskip_ARG)))
			continue;

		if ((activate == CHANGE_AAY) &&
		    !lv_passes_auto_activation_filter(cmd, lv))
			continue;

		expected_count++;

		if (!lv_change_activate(cmd, lv, activate)) {
			if (!lv_is_active_exclusive_remotely(lv))
				stack;
			else {
				/*
				 * If the LV is active exclusive remotely,
				 * then ignore it here
				 */
				log_verbose("%s/%s is exclusively active on"
					    " a remote node", vg->name, lv->name);
				expected_count--; /* not accounted */
			}
			continue;
		}

		count++;
	}

	sigint_restore();
	sync_local_dev_names(vg->cmd);  /* Wait until devices are available */

	if (expected_count)
		log_verbose("%s %d logical volumes in volume group %s",
			    is_change_activating(activate) ?
			    "Activated" : "Deactivated", count, vg->name);

	return (expected_count != count) ? 0 : 1;
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

static int _vgchange_background_polling(struct cmd_context *cmd, struct volume_group *vg)
{
	int polled;

	if (lvs_in_vg_activated(vg) && background_polling()) {
	        polled = _poll_lvs_in_vg(cmd, vg);
		if (polled)
			log_print_unless_silent("Background polling started for %d logical volume(s) "
						"in volume group \"%s\"",
						polled, vg->name);
	}

	return 1;
}

int vgchange_activate(struct cmd_context *cmd, struct volume_group *vg,
		      activation_change_t activate)
{
	int lv_open, active, monitored = 0, r = 1;
	const struct lv_list *lvl;
	int do_activate = is_change_activating(activate);

	/*
	 * We can get here in the odd case where an LV is already active in
	 * a foreign VG, which allows the VG to be accessed by vgchange -a
	 * so the LV can be deactivated.
	 */
	if (vg->system_id && vg->system_id[0] &&
	    cmd->system_id && cmd->system_id[0] &&
	    strcmp(vg->system_id, cmd->system_id) &&
	    is_change_activating(activate)) {
		log_error("Cannot activate LVs in a foreign VG.");
		return ECMD_FAILED;
	}

	/*
	 * Safe, since we never write out new metadata here. Required for
	 * partial activation to work.
	 */
        cmd->handles_missing_pvs = 1;

	/* FIXME: Force argument to deactivate them? */
	if (!do_activate && (lv_open = lvs_in_vg_opened(vg))) {
		dm_list_iterate_items(lvl, &vg->lvs)
			if (lv_is_visible(lvl->lv) &&
			    !lv_check_not_in_use(lvl->lv)) {
				log_error("Can't deactivate volume group \"%s\" with %d open "
					  "logical volume(s)", vg->name, lv_open);
				return 0;
			}
	}

	/* FIXME Move into library where clvmd can use it */
	if (do_activate)
		check_current_backup(vg);

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

	/* Print message only if there was not found a missing VG */
	if (!vg->cmd_missing_vgs)
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

static int _vgchange_clustered(struct cmd_context *cmd,
			       struct volume_group *vg)
{
	int clustered = arg_int_value(cmd, clustered_ARG, 0);
	struct lv_list *lvl;
	struct lv_segment *mirror_seg;

	if (clustered && vg_is_clustered(vg)) {
		if (vg->system_id && *vg->system_id)
			log_warn("WARNING: Clearing invalid system ID %s from volume group %s.",
				 vg->system_id, vg->name);
		else {
			log_error("Volume group \"%s\" is already clustered", vg->name);
			return 0;
		}
	}

	if (!clustered && !vg_is_clustered(vg)) {
		if ((!vg->system_id || !*vg->system_id) && cmd->system_id && *cmd->system_id)
			log_warn("Setting missing system ID on Volume Group %s to %s.",
				 vg->name, cmd->system_id);
		else {
			log_error("Volume group \"%s\" is already not clustered",
				  vg->name);
			return 0;
		}
	}

	if (clustered && !arg_count(cmd, yes_ARG)) {
		if (!clvmd_is_running()) {
			if (yes_no_prompt("LVM cluster daemon (clvmd) is not running. "
					  "Make volume group \"%s\" clustered "
					  "anyway? [y/n]: ", vg->name) == 'n') {
				log_error("No volume groups changed.");
				return 0;
			}

		} else if (!locking_is_clustered() &&
			   (yes_no_prompt("LVM locking type is not clustered. "
					  "Make volume group \"%s\" clustered "
					  "anyway? [y/n]: ", vg->name) == 'n')) {
			log_error("No volume groups changed.");
			return 0;
		}
#ifdef CMIRROR_REGION_COUNT_LIMIT
		dm_list_iterate_items(lvl, &vg->lvs) {
			if (!lv_is_mirror(lvl->lv))
				continue;
			mirror_seg = first_seg(lvl->lv);
			if ((lvl->lv->size / mirror_seg->region_size) >
			    CMIRROR_REGION_COUNT_LIMIT) {
				log_error("Unable to convert %s to clustered mode:"
					  " Mirror region size of %s is too small.",
					  vg->name, lvl->lv->name);
				return 0;
			}
		}
#endif
	}

	if (!vg_set_system_id(vg, clustered ? NULL : cmd->system_id))
		return_0;

	if (!vg_set_clustered(vg, clustered))
		return_0;

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
		log_warn("Physical extent size cannot be larger than %s.",
			 display_size(cmd, (uint64_t) MAX_EXTENT_SIZE));
		return 1;
	}

	extent_size = arg_uint_value(cmd, physicalextentsize_ARG, 0);
	/* FIXME: remove check - redundant with vg_change_pesize */
	if (extent_size == vg->extent_size) {
		log_warn("Physical extent size of VG %s is already %s.",
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

	if (lvs_in_vg_activated(vg)) {
		log_error("Volume group has active logical volumes");
		return 0;
	}

	if (!id_create(&vg->id)) {
		log_error("Failed to generate new random UUID for VG %s.",
			  vg->name);
		return 0;
	}

	dm_list_iterate_items(lvl, &vg->lvs) {
		memcpy(&lvl->lv->lvid, &vg->id, sizeof(vg->id));
	}

	return 1;
}

static int _vgchange_metadata_copies(struct cmd_context *cmd,
				     struct volume_group *vg)
{
	uint32_t mda_copies = arg_uint_value(cmd, vgmetadatacopies_ARG, DEFAULT_VGMETADATACOPIES);

	if (mda_copies == vg_mda_copies(vg)) {
		if (vg_mda_copies(vg) == VGMETADATACOPIES_UNMANAGED)
			log_warn("Number of metadata copies for VG %s is already unmanaged.",
				 vg->name);
		else
			log_warn("Number of metadata copies for VG %s is already %u.",
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

	if (arg_count(cmd, detachprofile_ARG)) {
		new_profile_name = "(no profile)";
		vg->profile = NULL;
	} else {
		if (arg_count(cmd, metadataprofile_ARG))
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

	/* FIXME Merge with vg_set_system_id() */
	if (systemid_on_pvs(vg)) {
		log_error("Metadata format %s does not support this type of system ID.",
			  vg->fid->fmt->name);
		return 0;
	}

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

		if (!arg_count(cmd, yes_ARG) &&
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

		if (!arg_count(cmd, yes_ARG) &&
		    yes_no_prompt("Set foreign system ID %s on volume group %s? [y/n]: ",
				  system_id, vg->name) == 'n') {
			log_error("Volume group %s system ID not changed.", vg->name);
			return 0;
		}
	}

	log_verbose("Changing system ID for VG %s from \"%s\" to \"%s\".",
		    vg->name, vg->system_id, system_id);

	vg->system_id = system_id;
	
	if (vg->lvm1_system_id)
		*vg->lvm1_system_id = '\0';

	return 1;
}

static int vgchange_single(struct cmd_context *cmd, const char *vg_name,
			   struct volume_group *vg,
			   struct processing_handle *handle __attribute__((unused)))
{
	int ret = ECMD_PROCESSED;
	unsigned i;
	struct lv_list *lvl;

	static const struct {
		int arg;
		int (*fn)(struct cmd_context *cmd, struct volume_group *vg);
	} _vgchange_args[] = {
		{ logicalvolume_ARG, &_vgchange_logicalvolume },
		{ maxphysicalvolumes_ARG, &_vgchange_physicalvolumes },
		{ resizeable_ARG, &_vgchange_resizeable },
		{ deltag_ARG, &_vgchange_deltag },
		{ addtag_ARG, &_vgchange_addtag },
		{ physicalextentsize_ARG, &_vgchange_pesize },
		{ uuid_ARG, &_vgchange_uuid },
		{ alloc_ARG, &_vgchange_alloc },
		{ clustered_ARG, &_vgchange_clustered },
		{ vgmetadatacopies_ARG, &_vgchange_metadata_copies },
		{ metadataprofile_ARG, &_vgchange_profile },
		{ profile_ARG, &_vgchange_profile },
		{ detachprofile_ARG, &_vgchange_profile },
		{ systemid_ARG, &_vgchange_system_id },
	};

	if (vg_is_exported(vg)) {
		log_error("Volume group \"%s\" is exported", vg_name);
		return ECMD_FAILED;
	}

	/*
	 * FIXME: DEFAULT_BACKGROUND_POLLING should be "unspecified".
	 * If --poll is explicitly provided use it; otherwise polling
	 * should only be started if the LV is not already active. So:
	 * 1) change the activation code to say if the LV was actually activated
	 * 2) make polling of an LV tightly coupled with LV activation
	 *
	 * Do not initiate any polling if --sysinit option is used.
	 */
	init_background_polling(arg_count(cmd, sysinit_ARG) ? 0 :
						arg_int_value(cmd, poll_ARG,
						DEFAULT_BACKGROUND_POLLING));

	for (i = 0; i < DM_ARRAY_SIZE(_vgchange_args); ++i) {
		if (arg_count(cmd, _vgchange_args[i].arg)) {
			if (!archive(vg))
				return_ECMD_FAILED;
			if (!_vgchange_args[i].fn(cmd, vg))
				return_ECMD_FAILED;
		}
	}

	if (vg_is_archived(vg)) {
		if (!vg_write(vg) || !vg_commit(vg))
			return_ECMD_FAILED;

		backup(vg);

		log_print_unless_silent("Volume group \"%s\" successfully changed", vg->name);

		/* FIXME: fix clvmd bug and take DLM lock for non clustered VGs. */
		if (arg_is_set(cmd, clustered_ARG) &&
		    vg_is_clustered(vg) && /* just switched to clustered */
		    locking_is_clustered() &&
		    locking_supports_remote_queries())
			dm_list_iterate_items(lvl, &vg->lvs) {
				if ((lv_lock_holder(lvl->lv) != lvl->lv) ||
				    !lv_is_active(lvl->lv))
					continue;

				if (!activate_lv_excl_local(cmd, lvl->lv) ||
				    !lv_is_active_exclusive_locally(lvl->lv)) {
					log_error("Can't reactive logical volume %s, "
						  "please fix manually.",
						  display_lvname(lvl->lv));
					ret = ECMD_FAILED;
				}

				if (lv_is_mirror(lvl->lv))
					/* Give hint for clustered mirroring */
					log_print_unless_silent("For clustered mirroring of %s "
								"deactivation and activation is needed.",
								display_lvname(lvl->lv));
			}
	}

	if (arg_count(cmd, activate_ARG)) {
		if (!vgchange_activate(cmd, vg, (activation_change_t)
				       arg_uint_value(cmd, activate_ARG, CHANGE_AY)))
			return_ECMD_FAILED;
	}

	if (arg_count(cmd, refresh_ARG)) {
		/* refreshes the visible LVs (which starts polling) */
		if (!_vgchange_refresh(cmd, vg))
			return_ECMD_FAILED;
	}

	if (!arg_count(cmd, activate_ARG) &&
	    !arg_count(cmd, refresh_ARG) &&
	    arg_count(cmd, monitor_ARG)) {
		/* -ay* will have already done monitoring changes */
		if (!_vgchange_monitoring(cmd, vg))
			return_ECMD_FAILED;
	}

	if (!arg_count(cmd, refresh_ARG) &&
	    background_polling())
		if (!_vgchange_background_polling(cmd, vg))
			return_ECMD_FAILED;

        return ret;
}

int vgchange(struct cmd_context *cmd, int argc, char **argv)
{
	int noupdate =
		arg_count(cmd, activate_ARG) ||
		arg_count(cmd, monitor_ARG) ||
		arg_count(cmd, poll_ARG) ||
		arg_count(cmd, refresh_ARG);

	int update_partial_safe =
		arg_count(cmd, deltag_ARG) ||
		arg_count(cmd, addtag_ARG) ||
		arg_count(cmd, metadataprofile_ARG) ||
		arg_count(cmd, profile_ARG) ||
		arg_count(cmd, detachprofile_ARG);

	int update_partial_unsafe =
		arg_count(cmd, logicalvolume_ARG) ||
		arg_count(cmd, maxphysicalvolumes_ARG) ||
		arg_count(cmd, resizeable_ARG) ||
		arg_count(cmd, uuid_ARG) ||
		arg_count(cmd, physicalextentsize_ARG) ||
		arg_count(cmd, clustered_ARG) ||
		arg_count(cmd, alloc_ARG) ||
		arg_count(cmd, vgmetadatacopies_ARG) ||
		arg_count(cmd, systemid_ARG);

	int update = update_partial_safe || update_partial_unsafe;

	if (!update && !noupdate) {
		log_error("Need one or more command options.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_count(cmd, profile_ARG) || arg_count(cmd, metadataprofile_ARG)) &&
	     arg_count(cmd, detachprofile_ARG)) {
		log_error("Only one of --metadataprofile and --detachprofile permitted.");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, activate_ARG) && arg_count(cmd, refresh_ARG)) {
		log_error("Only one of -a and --refresh permitted.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_count(cmd, ignorelockingfailure_ARG) ||
	     arg_count(cmd, sysinit_ARG)) && update) {
		log_error("Only -a permitted with --ignorelockingfailure and --sysinit");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, activate_ARG) &&
	    (arg_count(cmd, monitor_ARG) || arg_count(cmd, poll_ARG))) {
		if (!is_change_activating((activation_change_t) arg_uint_value(cmd, activate_ARG, 0))) {
			log_error("Only -ay* allowed with --monitor or --poll.");
			return EINVALID_CMD_LINE;
		}
	}

	if (arg_count(cmd, poll_ARG) && arg_count(cmd, sysinit_ARG)) {
		log_error("Only one of --poll and --sysinit permitted.");
		return EINVALID_CMD_LINE;
	}

	if ((arg_count(cmd, activate_ARG) == 1) &&
	    arg_count(cmd, autobackup_ARG)) {
		log_error("-A option not necessary with -a option");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, maxphysicalvolumes_ARG) &&
	    arg_sign_value(cmd, maxphysicalvolumes_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("MaxPhysicalVolumes may not be negative");
		return EINVALID_CMD_LINE;
	}

	if (arg_count(cmd, physicalextentsize_ARG) &&
	    arg_sign_value(cmd, physicalextentsize_ARG, SIGN_NONE) == SIGN_MINUS) {
		log_error("Physical extent size may not be negative");
		return EINVALID_CMD_LINE;
	}

	/*
	 * If --sysinit -aay is used and at the same time lvmetad is used,
	 * we want to rely on autoactivation to take place. Also, we
	 * need to take special care here as lvmetad service does
	 * not neet to be running at this moment yet - it could be
	 * just too early during system initialization time.
	 */
	if (arg_count(cmd, sysinit_ARG) && lvmetad_used() &&
	    arg_uint_value(cmd, activate_ARG, 0) == CHANGE_AAY) {
		if (!lvmetad_socket_present()) {
			/*
			 * If lvmetad socket is not present yet,
			 * the service is just not started. It'll
			 * be started a bit later so we need to do
			 * the activation without lvmetad which means
			 * direct activation instead of autoactivation.
			 */
			log_warn("lvmetad is not active yet, using direct activation during sysinit");
			lvmetad_set_active(cmd, 0);
		} else if (lvmetad_active()) {
			/*
			 * If lvmetad is active already, we want
			 * to make use of the autoactivation.
			 */
			log_warn("lvmetad is active, skipping direct activation during sysinit");
			return ECMD_PROCESSED;
		}
	}

	if (arg_count(cmd, clustered_ARG) && !argc && !arg_count(cmd, yes_ARG) &&
	    (yes_no_prompt("Change clustered property of all volumes groups? [y/n]: ") == 'n')) {
		log_error("No volume groups changed.");
		return ECMD_FAILED;
	}

	if (!update || !update_partial_unsafe)
		cmd->handles_missing_pvs = 1;

	if (arg_is_set(cmd, activate_ARG))
		cmd->include_active_foreign_vgs = 1;

	return process_each_vg(cmd, argc, argv, update ? READ_FOR_UPDATE : 0,
			       NULL, &vgchange_single);
}
