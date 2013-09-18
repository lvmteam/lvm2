/*
 * Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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

/*
 * MACROS:
 * cfg_section(id, name, parent, flags, since_version, comment)
 * cfg(id, name, parent, flags, type, default_value, since_version, comment)
 * cfg_array(id, name, parent, flags, types, default_value, since_version, comment)
 *
 * VARIABLES:
 * cfg_section:		define a new configuration section
 * cfg:			define a new configuration setting of a simple type
 * cfg_array:		define a new configuration setting of array type
 *
 * id:			unique identifier
 * name:		configuration node name
 * parent:		id of parent configuration node
 * flags:		configuration item flags:
 * 				CFG_NAME_VARIABLE - configuration node name is variable
 * 				CFG_ALLOW_EMPTY - node value can be emtpy
 * 				CFG_ADVANCED - this node belongs to advanced config set
 * 				CFG_UNSUPPORTED - this node belongs to unsupported config set
 * 				CFG_PROFILABLE - this node is customizable by a profile
 * type:		allowed type for the value of simple configuation setting, one of:
 * 				CFG_TYPE_BOOL
 * 				CFG_TYPE_INT
 * 				CFG_TYPE_FLOAT
 * 				CFG_TYPE_STRING
 * types:		allowed types for the values of array configuration setting
 * 			(use logical "OR" to define more than one allowed type,
 * 			 e.g. CFG_TYPE_STRING | CFG_TYPE_INT)
 * default_value:	default value of type 'type' for the configuration node,
 * 			if this is an array with several 'types' defined then
 * 			default value is a string where each string representation
 * 			of each value is prefixed by '#X' where X is one of:
 *				'B' for boolean value
 * 				'I' for integer value
 * 				'F' for float value
 * 				'S' for string value
 * 				'#' for the '#' character itself
 * 			For example, "#Sfd#I16" means default value [ "fd", 16 ].
 * comment:		brief comment used in configuration dumps
 * since_version:	the version this configuration node first appeared in (be sure
 *			that parent nodes are consistent with versioning, no check done
 *			if parent node is older or the same age as any child node!)
 */

cfg_section(root_CFG_SECTION, "(root)", root_CFG_SECTION, 0, vsn(0, 0, 0), NULL)

cfg_section(config_CFG_SECTION, "config", root_CFG_SECTION, 0, vsn(2, 2, 99), "Configuration handling.")
cfg_section(devices_CFG_SECTION, "devices", root_CFG_SECTION, 0, vsn(1, 0, 0), NULL)
cfg_section(allocation_CFG_SECTION, "allocation", root_CFG_SECTION, CFG_PROFILABLE, vsn(2, 2, 77), NULL)
cfg_section(log_CFG_SECTION, "log", root_CFG_SECTION, 0, vsn(1, 0, 0), NULL)
cfg_section(backup_CFG_SECTION, "backup", root_CFG_SECTION, 0, vsn(1, 0, 0), NULL)
cfg_section(shell_CFG_SECTION, "shell", root_CFG_SECTION, 0, vsn(1, 0, 0), NULL)
cfg_section(global_CFG_SECTION, "global", root_CFG_SECTION, 0, vsn(1, 0, 0), NULL)
cfg_section(activation_CFG_SECTION, "activation", root_CFG_SECTION, CFG_PROFILABLE, vsn(1, 0, 0), NULL)
cfg_section(metadata_CFG_SECTION, "metadata", root_CFG_SECTION, CFG_ADVANCED, vsn(1, 0, 0), NULL)
cfg_section(report_CFG_SECTION, "report", root_CFG_SECTION, CFG_ADVANCED, vsn(1, 0, 0), NULL)
cfg_section(dmeventd_CFG_SECTION, "dmeventd", root_CFG_SECTION, 0, vsn(1, 2, 3), NULL)
cfg_section(tags_CFG_SECTION, "tags", root_CFG_SECTION, 0, vsn(1, 0, 18), NULL)

cfg(config_checks_CFG, "checks", config_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(2, 2, 99), "Configuration tree check on each LVM command execution.")
cfg(config_abort_on_errors_CFG, "abort_on_errors", config_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(2,2,99), "Abort LVM command execution if configuration is invalid.")
cfg(config_profile_dir_CFG, "profile_dir", config_CFG_SECTION, 0, CFG_TYPE_STRING, 0, vsn(2, 2, 99), "Directory with configuration profiles.")

cfg(devices_dir_CFG, "dir", devices_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DEV_DIR, vsn(1, 0, 0), NULL)
cfg_array(devices_scan_CFG, "scan", devices_CFG_SECTION, 0, CFG_TYPE_STRING, "#S/dev", vsn(1, 0, 0), NULL)
cfg_array(devices_loopfiles_CFG, "loopfiles", devices_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 2, 0), NULL)
cfg(devices_obtain_device_list_from_udev_CFG, "obtain_device_list_from_udev", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_OBTAIN_DEVICE_LIST_FROM_UDEV, vsn(2, 2, 85), NULL)
cfg_array(devices_preferred_names_CFG, "preferred_names", devices_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, NULL, vsn(1, 2, 19), NULL)
cfg_array(devices_filter_CFG, "filter", devices_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg_array(devices_global_filter_CFG, "global_filter", devices_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(2, 2, 98), NULL)
cfg(devices_cache_CFG, "cache", devices_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(devices_cache_dir_CFG, "cache_dir", devices_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 2, 19), NULL)
cfg(devices_cache_file_prefix_CFG, "cache_file_prefix", devices_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 2, 19), NULL)
cfg(devices_write_cache_state_CFG, "write_cache_state", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(1, 0, 0), NULL)
cfg_array(devices_types_CFG, "types", devices_CFG_SECTION, 0, CFG_TYPE_INT | CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(devices_sysfs_scan_CFG, "sysfs_scan", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SYSFS_SCAN, vsn(1, 0, 8), NULL)
cfg(devices_multipath_component_detection_CFG, "multipath_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MULTIPATH_COMPONENT_DETECTION, vsn(2, 2, 89), NULL)
cfg(devices_md_component_detection_CFG, "md_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MD_COMPONENT_DETECTION, vsn(1, 0, 18), NULL)
cfg(devices_md_chunk_alignment_CFG, "md_chunk_alignment", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MD_CHUNK_ALIGNMENT, vsn(2, 2, 48), NULL)
cfg(devices_default_data_alignment_CFG, "default_data_alignment", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_DATA_ALIGNMENT, vsn(2, 2, 75), NULL)
cfg(devices_data_alignment_detection_CFG, "data_alignment_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DATA_ALIGNMENT_DETECTION, vsn(2, 2, 51), NULL)
cfg(devices_data_alignment_CFG, "data_alignment", devices_CFG_SECTION, 0, CFG_TYPE_INT, 0, vsn(2, 2, 45), NULL)
cfg(devices_data_alignment_offset_detection_CFG, "data_alignment_offset_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DATA_ALIGNMENT_OFFSET_DETECTION, vsn(2, 2, 50), NULL)
cfg(devices_ignore_suspended_devices_CFG, "ignore_suspended_devices", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_IGNORE_SUSPENDED_DEVICES, vsn(1, 2, 19), NULL)
cfg(devices_disable_after_error_count_CFG, "disable_after_error_count", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_DISABLE_AFTER_ERROR_COUNT, vsn(2, 2, 75), NULL)
cfg(devices_require_restorefile_with_uuid_CFG, "require_restorefile_with_uuid", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REQUIRE_RESTOREFILE_WITH_UUID, vsn(2, 2, 73), NULL)
cfg(devices_pv_min_size_CFG, "pv_min_size", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_PV_MIN_SIZE_KB, vsn(2, 2, 85), NULL)
cfg(devices_issue_discards_CFG, "issue_discards", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ISSUE_DISCARDS, vsn(2, 2, 85), NULL)

cfg_array(allocation_cling_tag_list_CFG, "cling_tag_list", allocation_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(2, 2, 77), NULL)
cfg(allocation_maximise_cling_CFG, "maximise_cling", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MAXIMISE_CLING, vsn(2, 2, 85), NULL)
cfg(allocation_mirror_logs_require_separate_pvs_CFG, "mirror_logs_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MIRROR_LOGS_REQUIRE_SEPARATE_PVS, vsn(2, 2, 85), NULL)
cfg(allocation_thin_pool_metadata_require_separate_pvs_CFG, "thin_pool_metadata_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_THIN_POOL_METADATA_REQUIRE_SEPARATE_PVS, vsn(2, 2, 89), NULL)
cfg(allocation_thin_pool_zero_CFG, "thin_pool_zero", allocation_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_THIN_POOL_ZERO, vsn(2, 2, 99), NULL)
cfg(allocation_thin_pool_discards_CFG, "thin_pool_discards", allocation_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_THIN_POOL_DISCARDS, vsn(2, 2, 99), NULL)
cfg(allocation_thin_pool_chunk_size_CFG, "thin_pool_chunk_size", allocation_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_INT, DEFAULT_THIN_POOL_CHUNK_SIZE, vsn(2, 2, 99), NULL)

cfg(log_verbose_CFG, "verbose", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_VERBOSE, vsn(1, 0, 0), NULL)
cfg(log_silent_CFG, "silent", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SILENT, vsn(2, 2, 98), NULL)
cfg(log_syslog_CFG, "syslog", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SYSLOG, vsn(1, 0, 0), NULL)
cfg(log_file_CFG, "file", log_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(log_overwrite_CFG, "overwrite", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_OVERWRITE, vsn(1, 0, 0), NULL)
cfg(log_level_CFG, "level", log_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_LOGLEVEL, vsn(1, 0, 0), NULL)
cfg(log_indent_CFG, "indent", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_INDENT, vsn(1, 0, 0), NULL)
cfg(log_command_names_CFG, "command_names", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_CMD_NAME, vsn(1, 0, 0), NULL)
cfg(log_prefix_CFG, "prefix", log_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, DEFAULT_MSG_PREFIX, vsn(1, 0, 0), NULL)
cfg(log_activation_CFG, "activation", log_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(1, 0, 0), NULL)
cfg(log_activate_file_CFG, "activate_file", log_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg_array(log_debug_classes_CFG, "debug_classes", log_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, "#Smemory#Sdevices#Sactivation#Sallocation#Slvmetad#Smetadata#Scache#Slocking", vsn(2, 2, 99), NULL)

cfg(backup_backup_CFG, "backup", backup_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_BACKUP_ENABLED, vsn(1, 0, 0), NULL)
cfg(backup_backup_dir_CFG, "backup_dir", backup_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(backup_archive_CFG, "archive", backup_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ARCHIVE_ENABLED, vsn(1, 0, 0), NULL)
cfg(backup_archive_dir_CFG, "archive_dir", backup_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(backup_retain_min_CFG, "retain_min", backup_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_ARCHIVE_NUMBER, vsn(1, 0, 0), NULL)
cfg(backup_retain_days_CFG, "retain_days", backup_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_ARCHIVE_DAYS, vsn(1, 0, 0), NULL)

cfg(shell_history_size_CFG, "history_size", shell_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_MAX_HISTORY, vsn(1, 0, 0), NULL)

cfg(global_umask_CFG, "umask", global_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_UMASK, vsn(1, 0, 0), NULL)
cfg(global_test_CFG, "test", global_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(1, 0, 0), NULL)
cfg(global_units_CFG, "units", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_UNITS, vsn(1, 0, 0), NULL)
cfg(global_si_unit_consistency_CFG, "si_unit_consistency", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SI_UNIT_CONSISTENCY,  vsn(2, 2, 54), NULL)
cfg(global_activation_CFG, "activation", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ACTIVATION, vsn(1, 0, 0), NULL)
cfg(global_suffix_CFG, "suffix", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SUFFIX, vsn(1, 0, 0), NULL)
cfg(global_fallback_to_lvm1_CFG, "fallback_to_lvm1", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_LVM1, vsn(1, 0, 18), NULL)
cfg(global_format_CFG, "format", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_FORMAT, vsn(1, 0, 0), NULL)
cfg_array(global_format_libraries_CFG, "format_libraries", global_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg_array(global_segment_libraries_CFG, "segment_libraries", global_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL)
cfg(global_proc_CFG, "proc", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PROC_DIR, vsn(1, 0, 0), NULL)
cfg(global_locking_type_CFG, "locking_type", global_CFG_SECTION, 0, CFG_TYPE_INT, 1, vsn(1, 0, 0), NULL)
cfg(global_wait_for_locks_CFG, "wait_for_locks", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_WAIT_FOR_LOCKS, vsn(2, 2, 50), NULL)
cfg(global_fallback_to_clustered_locking_CFG, "fallback_to_clustered_locking", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_CLUSTERED_LOCKING, vsn(2, 2, 42), NULL)
cfg(global_fallback_to_local_locking_CFG, "fallback_to_local_locking", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_LOCAL_LOCKING, vsn(2, 2, 42), NULL)
cfg(global_locking_dir_CFG, "locking_dir", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_LOCK_DIR, vsn(1, 0, 0), NULL)
cfg(global_prioritise_write_locks_CFG, "prioritise_write_locks", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_PRIORITISE_WRITE_LOCKS, vsn(2, 2, 52), NULL)
cfg(global_library_dir_CFG, "library_dir", global_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(global_locking_library_CFG, "locking_library", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(global_abort_on_internal_errors_CFG, "abort_on_internal_errors", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ABORT_ON_INTERNAL_ERRORS, vsn(2, 2, 57), NULL)
cfg(global_detect_internal_vg_cache_corruption_CFG, "detect_internal_vg_cache_corruption", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DETECT_INTERNAL_VG_CACHE_CORRUPTION, vsn(2, 2, 96), NULL)
cfg(global_metadata_read_only_CFG, "metadata_read_only", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_METADATA_READ_ONLY, vsn(2, 2, 75), NULL)
cfg(global_mirror_segtype_default_CFG, "mirror_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_SEGTYPE, vsn(2, 2, 87), NULL)
cfg(global_raid10_segtype_default_CFG, "raid10_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_RAID10_SEGTYPE, vsn(2, 2, 99), NULL)
cfg(global_lvdisplay_shows_full_device_path_CFG, "lvdisplay_shows_full_device_path", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_LVDISPLAY_SHOWS_FULL_DEVICE_PATH, vsn(2, 2, 89), NULL)
cfg(global_use_lvmetad_CFG, "use_lvmetad", global_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(2, 2, 93), NULL)
cfg(global_thin_check_executable_CFG, "thin_check_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, THIN_CHECK_CMD, vsn(2, 2, 94), NULL)
cfg_array(global_thin_check_options_CFG, "thin_check_options", global_CFG_SECTION, 0, CFG_TYPE_STRING, "#S" DEFAULT_THIN_CHECK_OPTIONS, vsn(2, 2, 96), NULL)
cfg_array(global_thin_disabled_features_CFG, "thin_disabled_features", global_CFG_SECTION, 0, CFG_TYPE_STRING, "#S", vsn(2, 2, 99), NULL)
cfg(global_thin_dump_executable_CFG, "thin_dump_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, THIN_DUMP_CMD, vsn(2, 2, 100), NULL)
cfg(global_thin_repair_executable_CFG, "thin_repair_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, THIN_REPAIR_CMD, vsn(2, 2, 100), NULL)
cfg_array(global_thin_repair_options_CFG, "thin_repair_options", global_CFG_SECTION, 0, CFG_TYPE_STRING, "#S" DEFAULT_THIN_REPAIR_OPTIONS, vsn(2, 2, 100), NULL)

cfg(activation_checks_CFG, "checks", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ACTIVATION_CHECKS, vsn(2, 2, 86), NULL)
cfg(activation_udev_sync_CFG, "udev_sync", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_UDEV_SYNC, vsn(2, 2, 51), NULL)
cfg(activation_udev_rules_CFG, "udev_rules", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_UDEV_RULES, vsn(2, 2, 57), NULL)
cfg(activation_verify_udev_operations_CFG, "verify_udev_operations", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_VERIFY_UDEV_OPERATIONS, vsn(2, 2, 86), NULL)
cfg(activation_retry_deactivation_CFG, "retry_deactivation", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_RETRY_DEACTIVATION, vsn(2, 2, 89), NULL)
cfg(activation_missing_stripe_filler_CFG, "missing_stripe_filler", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_STRIPE_FILLER, vsn(1, 0, 0), NULL)
cfg(activation_use_linear_target_CFG, "use_linear_target", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_LINEAR_TARGET, vsn(2, 2, 89), NULL)
cfg(activation_reserved_stack_CFG, "reserved_stack", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RESERVED_STACK, vsn(1, 0, 0), NULL)
cfg(activation_reserved_memory_CFG, "reserved_memory", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RESERVED_MEMORY, vsn(1, 0, 0), NULL)
cfg(activation_process_priority_CFG, "process_priority", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_PROCESS_PRIORITY, vsn(1, 0, 0), NULL)
cfg_array(activation_volume_list_CFG, "volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL)
cfg_array(activation_auto_activation_volume_list_CFG, "auto_activation_volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, NULL, vsn(2, 2, 97), NULL)
cfg_array(activation_read_only_volume_list_CFG, "read_only_volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, NULL, vsn(2, 2, 89), NULL)
cfg(activation_mirror_region_size_CFG, "mirror_region_size", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RAID_REGION_SIZE, vsn(1, 0, 0), NULL)
cfg(activation_raid_region_size_CFG, "raid_region_size", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RAID_REGION_SIZE, vsn(2, 2, 99), NULL)
cfg(activation_readahead_CFG, "readahead", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_READ_AHEAD, vsn(1, 0, 23), NULL)
cfg(activation_raid_fault_policy_CFG, "raid_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_RAID_FAULT_POLICY, vsn(2, 2, 89), NULL)
cfg(activation_mirror_device_fault_policy_CFG, "mirror_device_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_DEVICE_FAULT_POLICY, vsn(1, 2, 10), NULL)
cfg(activation_mirror_log_fault_policy_CFG, "mirror_log_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_LOG_FAULT_POLICY, vsn(1, 2, 18), NULL)
cfg(activation_mirror_image_fault_policy_CFG, "mirror_image_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(2, 2, 57), NULL)
cfg(activation_snapshot_autoextend_threshold_CFG, "snapshot_autoextend_threshold", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_SNAPSHOT_AUTOEXTEND_THRESHOLD, vsn(2, 2, 75), NULL)
cfg(activation_snapshot_autoextend_percent_CFG, "snapshot_autoextend_percent", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_SNAPSHOT_AUTOEXTEND_PERCENT, vsn(2, 2, 75), NULL)
cfg(activation_thin_pool_autoextend_threshold_CFG, "thin_pool_autoextend_threshold", activation_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_INT, DEFAULT_THIN_POOL_AUTOEXTEND_THRESHOLD, vsn(2, 2, 89), NULL)
cfg(activation_thin_pool_autoextend_percent_CFG, "thin_pool_autoextend_percent", activation_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_INT, DEFAULT_THIN_POOL_AUTOEXTEND_PERCENT, vsn(2, 2, 89), NULL)
cfg_array(activation_mlock_filter_CFG, "mlock_filter", activation_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(2, 2, 62), NULL)
cfg(activation_use_mlockall_CFG, "use_mlockall", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_MLOCKALL, vsn(2, 2, 62), NULL)
cfg(activation_monitoring_CFG, "monitoring", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DMEVENTD_MONITOR, vsn(2, 2, 63), NULL)
cfg(activation_polling_interval_CFG, "polling_interval", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_INTERVAL, vsn(2, 2, 63), NULL)
cfg(activation_auto_set_activation_skip_CFG, "auto_set_activation_skip", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_AUTO_SET_ACTIVATION_SKIP, vsn(2,2,99), NULL)

cfg(metadata_pvmetadatacopies_CFG, "pvmetadatacopies", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_PVMETADATACOPIES, vsn(1, 0, 0), NULL)
cfg(metadata_vgmetadatacopies_CFG, "vgmetadatacopies", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_VGMETADATACOPIES, vsn(2, 2, 69), NULL)
cfg(metadata_pvmetadatasize_CFG, "pvmetadatasize", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_PVMETADATASIZE, vsn(1, 0, 0), NULL)
cfg(metadata_pvmetadataignore_CFG, "pvmetadataignore", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_BOOL, DEFAULT_PVMETADATAIGNORE, vsn(2, 2, 69), NULL)
cfg(metadata_stripesize_CFG, "stripesize", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_STRIPESIZE, vsn(1, 0, 0), NULL)
cfg_array(metadata_dirs_CFG, "dirs", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)
cfg(metadata_disk_areas_CFG, "disk_areas", metadata_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_ADVANCED | CFG_UNSUPPORTED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)

cfg(report_aligned_CFG, "aligned", report_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REP_ALIGNED, vsn(1, 0, 0), NULL)
cfg(report_buffered_CFG, "buffered", report_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REP_BUFFERED, vsn(1, 0, 0), NULL)
cfg(report_headings_CFG, "headings", report_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REP_HEADINGS, vsn(1, 0, 0), NULL)
cfg(report_separator_CFG, "separator", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_REP_SEPARATOR, vsn(1, 0, 0), NULL)
cfg(report_prefixes_CFG, "prefixes", report_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REP_PREFIXES, vsn(2, 2, 36), NULL)
cfg(report_quoted_CFG, "quoted", report_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REP_QUOTED, vsn(2, 2, 39), NULL)
cfg(report_colums_as_rows_CFG, "colums_as_rows", report_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REP_COLUMNS_AS_ROWS, vsn(1, 0, 0), NULL)
cfg(report_devtypes_sort_CFG, "devtypes_sort", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DEVTYPES_SORT, vsn(2, 2, 101), NULL)
cfg(report_devtypes_cols_CFG, "devtypes_cols", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DEVTYPES_COLS, vsn(2, 2, 101), NULL)
cfg(report_devtypes_cols_verbose_CFG, "devtypes_cols_verbose", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DEVTYPES_COLS_VERB, vsn(2, 2, 101), NULL)
cfg(report_lvs_sort_CFG, "lvs_sort", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_LVS_SORT, vsn(1, 0, 0), NULL)
cfg(report_lvs_cols_CFG, "lvs_cols", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_LVS_COLS, vsn(1, 0, 0), NULL)
cfg(report_lvs_cols_verbose_CFG, "lvs_cols_verbose", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_LVS_COLS_VERB, vsn(1, 0, 0), NULL)
cfg(report_vgs_sort_CFG, "vgs_sort", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_VGS_SORT, vsn(1, 0, 0), NULL)
cfg(report_vgs_cols_CFG, "vgs_cols", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_VGS_COLS, vsn(1, 0, 0), NULL)
cfg(report_vgs_cols_verbose_CFG, "vgs_cols_verbose", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_VGS_COLS_VERB, vsn(1, 0, 0), NULL)
cfg(report_pvs_sort_CFG, "pvs_sort", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PVS_SORT, vsn(1, 0, 0), NULL)
cfg(report_pvs_cols_CFG, "pvs_cols", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PVS_COLS, vsn(1, 0, 0), NULL)
cfg(report_pvs_cols_verbose_CFG, "pvs_cols_verbose", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PVS_COLS_VERB, vsn(1, 0, 0), NULL)
cfg(report_segs_sort_CFG, "segs_sort", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_SEGS_SORT, vsn(1, 0, 0), NULL)
cfg(report_segs_cols_CFG, "segs_cols", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_SEGS_COLS, vsn(1, 0, 0), NULL)
cfg(report_segs_cols_verbose_CFG, "segs_cols_verbose", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_SEGS_COLS_VERB, vsn(1, 0, 0), NULL)
cfg(report_pvsegs_sort_CFG, "pvsegs_sort", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PVSEGS_SORT, vsn(1, 1, 3), NULL)
cfg(report_pvsegs_cols_CFG, "pvsegs_cols", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PVSEGS_COLS, vsn(1, 1, 3), NULL)
cfg(report_pvsegs_cols_verbose_CFG, "pvsegs_cols_verbose", report_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PVSEGS_COLS_VERB, vsn(1, 1, 3), NULL)

cfg(dmeventd_mirror_library_CFG, "mirror_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_MIRROR_LIB, vsn(1, 2, 3), NULL)
cfg(dmeventd_raid_library_CFG, "raid_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_RAID_LIB, vsn(2, 2, 87), NULL)
cfg(dmeventd_snapshot_library_CFG, "snapshot_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_SNAPSHOT_LIB, vsn(1, 2, 26), NULL)
cfg(dmeventd_thin_library_CFG, "thin_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_THIN_LIB, vsn(2, 2, 89), NULL)
cfg(dmeventd_executable_CFG, "executable", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, NULL, vsn(2, 2, 73), NULL)

cfg(tags_hosttags_CFG, "hosttags", tags_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_HOSTTAGS, vsn(1, 0, 18), NULL)

cfg_section(tag_CFG_SUBSECTION, "tag", tags_CFG_SECTION, CFG_NAME_VARIABLE, vsn(1, 0, 18), NULL)
cfg(tag_host_list_CFG, "host_list", tag_CFG_SUBSECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL)

cfg(CFG_COUNT, NULL, root_CFG_SECTION, 0, CFG_TYPE_INT, 0, vsn(0, 0, 0), NULL)
