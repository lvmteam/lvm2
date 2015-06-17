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
 * - define a configuration section:
 *   cfg_section(id, name, parent, flags, since_version, deprecated_since_version, deprecation_comment, comment)
 *
 * - define a configuration setting of simple type:
 *   cfg(id, name, parent, flags, type, default_value, since_version, unconfigured_default_value, deprecated_since_version, deprecation_comment, comment)
 *
 * - define a configuration array of one or more types:
 *   cfg_array(id, name, parent, flags, types, default_value, since_version, unconfigured_default_value, deprecated_since_version, deprecation_comment, comment)
 *
 *
 * If default value can't be assigned statically because it depends on some
 * run-time checks or if it depends on other settings already defined,
 * the configuration setting  or array can be defined with the
 * "{cfg|cfg_array}_runtime" macro. In this case the  default value
 * is evaluated by automatically calling "get_default_<id>" function.
 * See config.h and "function types to evaluate default value at runtime".
 *
 *
 * VARIABLES:
 * 
 * id:                         Unique identifier.
 *
 * name:                       Configuration node name.
 *
 * parent:                     Id of parent configuration node.
 *
 * flags:                      Configuration item flags:
 *                                 CFG_NAME_VARIABLE - configuration node name is variable
 *                                 CFG_ALLOW_EMPTY - node value can be emtpy
 *                                 CFG_ADVANCED - this node belongs to advanced config set
 *                                 CFG_UNSUPPORTED - this node is not officially supported and it's used primarily by developers
 *                                 CFG_PROFILABLE - this node is customizable by a profile
 *                                 CFG_PROFILABLE_METADATA - profilable and attachable to VG/LV metadata
 *                                 CFG_DEFAULT_UNDEFINED - node's default value is undefined (depends on other system/kernel values outside of lvm)
 *                                 CFG_DEFAULT_COMMENTED - node's default value is commented out on output
 *                                 CFG_DISABLED - configuration is disabled (defaults always used)
 *
 * type:		       Allowed type for the value of simple configuation setting, one of:
 *                                 CFG_TYPE_BOOL
 *                                 CFG_TYPE_INT
 *                                 CFG_TYPE_FLOAT
 *                                 CFG_TYPE_STRING
 *
 * types:                      Allowed types for the values of array configuration setting
 *                             (use logical "OR" to define more than one allowed type,
 *                             e.g. CFG_TYPE_STRING | CFG_TYPE_INT).
 *
 * default_value:              Default value of type 'type' for the configuration node,
 *                             if this is an array with several 'types' defined then
 *                             default value is a string where each string representation
 *                             of each value is prefixed by '#X' where X is one of:
 *                                 'B' for boolean value
 *                                 'I' for integer value
 *                                 'F' for float value
 *                                 'S' for string value
 *                                 '#' for the '#' character itself
 *                             For example, "#Sfd#I16" means default value [ "fd", 16 ].
 *
 * since_version:              The version this configuration node first appeared in (be sure
 *                             that parent nodes are consistent with versioning, no check done
 *                             if parent node is older or the same age as any child node!)
 *                             Use "vsn" macro to translate the "major.minor.release" version
 *                             into a single number that is being stored internally in memory.
 *                             (see also lvmconfig ... --withversions)
 *
 * unconfigured_default_value: Unconfigured default value used as a default value which is
 *                             in "@...@" form and which is then substitued with concrete value
 *                             while running configure.
 *                             (see also 'lvmconfig --type default --unconfigured')
 *
 * deprecated_since_version:   The version since this configuration node is deprecated.
 *
 * deprecation_comment:        Comment about deprecation reason and related info (e.g. which
 *                             configuration is used now instead).
 *
 * comment:                    Comment used in configuration dumps. The very first line is the
 *                             summarizing comment.
 *                             (see also lvmconfig ... --withcomments and --withsummary)
 *
 *
 * Difference between CFG_DEFAULT_COMMENTED and CFG_DEFAULT_UNDEFINED:
 *
 * UNDEFINED is used if default value is NULL or the value
 * depends on other system/kernel values outside of lvm.
 * The most common case is when dm-thin or dm-cache have
 * built-in default settings in the kernel, and lvm will use
 * those built-in default values unless the corresponding lvm
 * config setting is set.
 *
 * COMMENTED is used to comment out the default setting in
 * lvm.conf.  The effect is that if the LVM version is
 * upgraded, and the new version of LVM has new built-in
 * default values, the new defaults are used by LVM unless
 * the previous default value was set (uncommented) in lvm.conf.
 */
#include "defaults.h"

cfg_section(root_CFG_SECTION, "(root)", root_CFG_SECTION, 0, vsn(0, 0, 0), 0, NULL, NULL)

cfg_section(config_CFG_SECTION, "config", root_CFG_SECTION, 0, vsn(2, 2, 99), 0, NULL,
	"How LVM configuration settings are handled.\n")

cfg_section(devices_CFG_SECTION, "devices", root_CFG_SECTION, 0, vsn(1, 0, 0), 0, NULL,
	"How LVM uses block devices.\n")

cfg_section(allocation_CFG_SECTION, "allocation", root_CFG_SECTION, CFG_PROFILABLE, vsn(2, 2, 77), 0, NULL,
	"How LVM selects free space for Logical Volumes.\n")

cfg_section(log_CFG_SECTION, "log", root_CFG_SECTION, 0, vsn(1, 0, 0), 0, NULL,
	"How LVM log information is reported.\n")

cfg_section(backup_CFG_SECTION, "backup", root_CFG_SECTION, 0, vsn(1, 0, 0), 0, NULL,
	"How LVM metadata is backed up and archived.\n"
	"In LVM, a 'backup' is a copy of the metadata for the\n"
	"current system, and an 'archive' contains old metadata\n"
	"configurations. They are stored in a human readable\n"
	"text format.\n")

cfg_section(shell_CFG_SECTION, "shell", root_CFG_SECTION, 0, vsn(1, 0, 0), 0, NULL,
	"Settings for running LVM in shell (readline) mode.\n")

cfg_section(global_CFG_SECTION, "global", root_CFG_SECTION, CFG_PROFILABLE, vsn(1, 0, 0), 0, NULL,
	"Miscellaneous global LVM settings.\n")

cfg_section(activation_CFG_SECTION, "activation", root_CFG_SECTION, CFG_PROFILABLE, vsn(1, 0, 0), 0, NULL, NULL)

cfg_section(metadata_CFG_SECTION, "metadata", root_CFG_SECTION, CFG_DEFAULT_COMMENTED, vsn(1, 0, 0), 0, NULL, NULL)

cfg_section(report_CFG_SECTION, "report", root_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, vsn(1, 0, 0), 0, NULL,
	"LVM report command output formatting.\n")

cfg_section(dmeventd_CFG_SECTION, "dmeventd", root_CFG_SECTION, 0, vsn(1, 2, 3), 0, NULL,
	"Settings for the LVM event daemon.\n")

cfg_section(tags_CFG_SECTION, "tags", root_CFG_SECTION, CFG_DEFAULT_COMMENTED, vsn(1, 0, 18), 0, NULL,
	"Host tag settings.\n")

cfg_section(local_CFG_SECTION, "local", root_CFG_SECTION, 0, vsn(2, 2, 117), 0, NULL,
	"LVM settings that are specific to the local host.\n")

cfg(config_checks_CFG, "checks", config_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(2, 2, 99), NULL, 0, NULL,
	"If enabled, any LVM configuration mismatch is reported.\n"
	"This implies checking that the configuration key is understood\n"
	"by LVM and that the value of the key is the proper type.\n"
	"If disabled, any configuration mismatch is ignored and the default\n"
	"value is used without any warning (a message about the\n"
	"configuration key not being found is issued in verbose mode only).\n")

cfg(config_abort_on_errors_CFG, "abort_on_errors", config_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(2,2,99), NULL, 0, NULL,
	"Abort the LVM process if a configuration mismatch is found.\n")

cfg_runtime(config_profile_dir_CFG, "profile_dir", config_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(2, 2, 99), 0, NULL,
	"Directory where LVM looks for configuration profiles.\n")

cfg(devices_dir_CFG, "dir", devices_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_STRING, DEFAULT_DEV_DIR, vsn(1, 0, 0), NULL, 0, NULL,
	"Directory in which to create volume group device nodes.\n"
	"Commands also accept this as a prefix on volume group names.\n")

cfg_array(devices_scan_CFG, "scan", devices_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_STRING, "#S/dev", vsn(1, 0, 0), NULL, 0, NULL,
	"Directories containing device nodes to use with LVM.\n")

cfg_array(devices_loopfiles_CFG, "loopfiles", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED | CFG_UNSUPPORTED, CFG_TYPE_STRING, NULL, vsn(1, 2, 0), NULL, 0, NULL, NULL)

cfg(devices_obtain_device_list_from_udev_CFG, "obtain_device_list_from_udev", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_OBTAIN_DEVICE_LIST_FROM_UDEV, vsn(2, 2, 85), NULL, 0, NULL,
	"Obtain the list of available devices from udev.\n"
	"This avoids opening or using any inapplicable non-block\n"
	"devices or subdirectories found in the udev directory.\n"
	"Any device node or symlink not managed by udev in the udev\n"
	"directory is ignored. This setting applies only to the\n"
	"udev-managed device directory; other directories will be\n"
	"scanned fully. LVM needs to be compiled with udev support\n"
	"for this setting to apply.\n")

cfg(devices_external_device_info_source_CFG, "external_device_info_source", devices_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_EXTERNAL_DEVICE_INFO_SOURCE, vsn(2, 2, 116), NULL, 0, NULL,
	"Select an external device information source.\n"
	"Some information may already be available in the system and\n"
	"LVM can use this information to determine the exact type\n"
	"or use of devices it processes. Using an existing external\n"
	"device information source can speed up device processing\n"
	"as LVM does not need to run its own native routines to acquire\n"
	"this information. For example, this information is used to\n"
	"drive LVM filtering like MD component detection, multipath\n"
	"component detection, partition detection and others.\n"
	"Possible options are: none, udev.\n"
	"none - No external device information source is used.\n"
	"udev - Reuse existing udev database records. Applicable\n"
	"only if LVM is compiled with udev support.\n")

cfg_array(devices_preferred_names_CFG, "preferred_names", devices_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED , CFG_TYPE_STRING, "#S", vsn(1, 2, 19), NULL, 0, NULL,
	"Select which path name to display for a block device.\n"
	"If multiple path names exist for a block device,\n"
	"and LVM needs to display a name for the device,\n"
	"the path names are matched against each item in\n"
	"this list of regular expressions. The first match is used.\n"
	"Try to avoid using undescriptive /dev/dm-N names, if present.\n"
	"If no preferred name matches, or if preferred_names are not\n"
	"defined, built-in rules are used until one produces a preference.\n"
	"Rule 1 checks path prefixes and gives preference in this order:\n"
	"/dev/mapper, /dev/disk, /dev/dm-*, /dev/block (/dev from devices/dev)\n"
	"Rule 2 prefers the path with the least slashes.\n"
	"Rule 3 prefers a symlink.\n"
	"Rule 4 prefers the path with least value in lexicographical order.\n"
	"Example:\n"
	"preferred_names = [ \"^/dev/mpath/\", \"^/dev/mapper/mpath\", \"^/dev/[hs]d\" ]\n")

cfg_array(devices_filter_CFG, "filter", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL,
	"Limit the block devices that are used by LVM commands.\n"
	"This is a list of regular expressions used to accept or\n"
	"reject block device path names.  Each regex is delimited\n"
	"by a vertical bar '|' (or any character) and is preceded\n"
	"by 'a' to accept the path, or by 'r' to reject the path.\n"
	"The first regex in the list to match the path is used,\n"
	"producing the 'a' or 'r' result for the device.\n"
	"When multiple path names exist for a block device, if any\n"
	"path name matches an 'a' pattern before an 'r' pattern,\n"
	"then the device is accepted. If all the path names match\n"
	"an 'r' pattern first, then the device is rejected.\n"
	"Unmatching path names do not affect the accept or reject\n"
	"decision. If no path names for a device match a pattern,\n"
	"then the device is accepted.\n"
	"Be careful mixing 'a' and 'r' patterns, as the combination\n"
	"might produce unexpected results (test any changes.)\n"
	"Run vgscan after changing the filter to regenerate the cache.\n"
	"See the use_lvmetad comment for a special case regarding filters.\n"
	"Example:\n"
	"Accept every block device.\n"
	"filter = [ \"a|.*/|\" ]\n"
	"Example:\n"
	"Reject the cdrom drive.\n"
	"filter = [ \"r|/dev/cdrom|\" ]\n"
	"Example:\n"
	"Work with just loopback devices, e.g. for testing.\n"
	"filter = [ \"a|loop|\", \"r|.*|\" ]\n"
	"Example:\n"
	"Accept all loop devices and ide drives except hdc.\n"
	"filter =[ \"a|loop|\", \"r|/dev/hdc|\", \"a|/dev/ide|\", \"r|.*|\" ]\n"
	"Example:\n"
	"Use anchors to be very specific.\n"
	"filter = [ \"a|^/dev/hda8$|\", \"r|.*/|\" ]\n")

cfg_array(devices_global_filter_CFG, "global_filter", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 98), NULL, 0, NULL,
	"Limit the block devices that are used by LVM system components.\n"
	"Because devices/filter may be overridden from the command line,\n"
	"it is not suitable for system-wide device filtering, e.g. udev\n"
	"and lvmetad. Use global_filter to hide devices from these LVM\n"
	"system components. The syntax is the same as devices/filter.\n"
	"Devices rejected by global_filter are not opened by LVM.\n")

cfg_runtime(devices_cache_CFG, "cache", devices_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 0, 0), vsn(1, 2, 19),
	"This has been replaced by the devices/cache_dir setting.\n",
	"Cache file path.\n")

cfg_runtime(devices_cache_dir_CFG, "cache_dir", devices_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 2, 19), 0, NULL,
	"Directory in which to store the device cache file.\n"
	"The results of filtering are cached on disk to avoid\n"
	"rescanning dud devices (which can take a very long time).\n"
	"By default this cache is stored in a file named .cache.\n"
	"It is safe to delete this file; the tools regenerate it.\n"
	"If obtain_device_list_from_udev is enabled, the list of devices\n"
	"is obtained from udev and any existing .cache file is removed.\n")

cfg(devices_cache_file_prefix_CFG, "cache_file_prefix", devices_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, DEFAULT_CACHE_FILE_PREFIX, vsn(1, 2, 19), NULL, 0, NULL,
	"A prefix used before the .cache file name. See devices/cache_dir.\n")

cfg(devices_write_cache_state_CFG, "write_cache_state", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(1, 0, 0), NULL, 0, NULL,
	"Enable/disable writing the cache file. See devices/cache_dir.\n")

cfg_array(devices_types_CFG, "types", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED | CFG_ADVANCED, CFG_TYPE_INT | CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL,
	"List of additional acceptable block device types.\n"
	"These are of device type names from /proc/devices,\n"
	"followed by the maximum number of partitions.\n"
	"Example:\n"
	"types = [ \"fd\", 16 ]\n")

cfg(devices_sysfs_scan_CFG, "sysfs_scan", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SYSFS_SCAN, vsn(1, 0, 8), NULL, 0, NULL,
	"Restrict device scanning to block devices appearing in sysfs.\n"
	"This is a quick way of filtering out block devices that are\n"
	"not present on the system. sysfs must be part of the kernel\n"
	"and mounted.)\n")

cfg(devices_multipath_component_detection_CFG, "multipath_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MULTIPATH_COMPONENT_DETECTION, vsn(2, 2, 89), NULL, 0, NULL,
	"Ignore devices that are components of DM multipath devices.\n")

cfg(devices_md_component_detection_CFG, "md_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MD_COMPONENT_DETECTION, vsn(1, 0, 18), NULL, 0, NULL,
	"Ignore devices that are components of software RAID (md) devices.\n")

cfg(devices_fw_raid_component_detection_CFG, "fw_raid_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FW_RAID_COMPONENT_DETECTION, vsn(2, 2, 112), NULL, 0, NULL,
	"Ignore devices that are components of firmware RAID devices.\n"
	"LVM must use an external_device_info_source other than none\n"
	"for this detection to execute.\n")

cfg(devices_md_chunk_alignment_CFG, "md_chunk_alignment", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MD_CHUNK_ALIGNMENT, vsn(2, 2, 48), NULL, 0, NULL,
	"Align PV data blocks with md device's stripe-width.\n"
	"This applies if a PV is placed directly on an md device.\n")

cfg(devices_default_data_alignment_CFG, "default_data_alignment", devices_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, DEFAULT_DATA_ALIGNMENT, vsn(2, 2, 75), NULL, 0, NULL,
	"Default alignment of the start of a PV data area in MB.\n"
	"If set to 0, a value of 64KB will be used.\n"
	"Set to 1 for 1MiB, 2 for 2MiB, etc.\n")

cfg(devices_data_alignment_detection_CFG, "data_alignment_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DATA_ALIGNMENT_DETECTION, vsn(2, 2, 51), NULL, 0, NULL,
	"Detect PV data alignment based on sysfs device information.\n"
	"The start of a PV data area will be a multiple of\n"
	"minimum_io_size or optimal_io_size exposed in sysfs.\n"
	"minimum_io_size is the smallest request the device can perform\n"
	"without incurring a read-modify-write penalty, e.g. MD chunk size.\n"
	"optimal_io_size is the device's preferred unit of receiving I/O,\n"
	"e.g. MD stripe width.\n"
	"minimum_io_size is used if optimal_io_size is undefined (0).\n"
	"If md_chunk_alignment is enabled, that detects the optimal_io_size.\n"
	"This setting takes precedence over md_chunk_alignment.\n")

cfg(devices_data_alignment_CFG, "data_alignment", devices_CFG_SECTION, 0, CFG_TYPE_INT, 0, vsn(2, 2, 45), NULL, 0, NULL,
	"Alignment of the start of a PV data area in KB.\n"
	"If a PV is placed directly on an md device and\n"
	"md_chunk_alignment or data_alignment_detection are enabled,\n"
	"then this setting is ignored.  Otherwise, md_chunk_alignment\n"
	"and data_alignment_detection are disabled if this is set.\n"
	"Set to 0 to use the default alignment or the page size, if larger.\n")

cfg(devices_data_alignment_offset_detection_CFG, "data_alignment_offset_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DATA_ALIGNMENT_OFFSET_DETECTION, vsn(2, 2, 50), NULL, 0, NULL,
	"Detect PV data alignment offset based on sysfs device information.\n"
	"The start of a PV aligned data area will be shifted by the\n"
	"alignment_offset exposed in sysfs.  This offset is often 0, but\n"
	"may be non-zero.  Certain 4KB sector drives that compensate for\n"
	"windows partitioning will have an alignment_offset of 3584 bytes\n"
	"(sector 7 is the lowest aligned logical block, the 4KB sectors start\n"
	"at LBA -1, and consequently sector 63 is aligned on a 4KB boundary).\n"
	"pvcreate --dataalignmentoffset will skip this detection.\n")

cfg(devices_ignore_suspended_devices_CFG, "ignore_suspended_devices", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_IGNORE_SUSPENDED_DEVICES, vsn(1, 2, 19), NULL, 0, NULL,
	"Ignore DM devices that have I/O suspended while scanning devices.\n"
	"Otherwise, LVM waits for a suspended device to become accessible.\n"
	"This should only be needed in recovery situations.\n")

cfg(devices_ignore_lvm_mirrors_CFG, "ignore_lvm_mirrors", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_IGNORE_LVM_MIRRORS, vsn(2, 2, 104), NULL, 0, NULL,
	"Do not scan 'mirror' LVs to avoid possible deadlocks.\n"
	"This avoids possible deadlocks when using the 'mirror'\n"
	"segment type.  This setting determines whether logical volumes\n"
	"using the 'mirror' segment type are scanned for LVM labels.\n"
	"This affects the ability of mirrors to be used as physical volumes.\n"
	"If this setting is enabled, it becomes impossible to create VGs\n"
	"on top of mirror LVs, i.e. to stack VGs on mirror LVs.\n"
	"If this setting is disabled, allowing mirror LVs to be scanned,\n"
	"it may cause LVM processes and I/O to the mirror to become blocked.\n"
	"This is due to the way that the mirror segment type handles failures.\n"
	"In order for the hang to occur, an LVM command must be run just after\n"
	"a failure and before the automatic LVM repair process takes place,\n"
	"or there must be failures in multiple mirrors in the same VG at the\n"
	"same time with write failures occurring moments before a scan of the\n"
	"mirror's labels.\n"
	"The 'mirror' scanning problems do not apply to LVM RAID types like\n"
	"'raid1' which handle failures in a different way, making them a\n"
	"better choice for VG stacking.\n")

cfg(devices_disable_after_error_count_CFG, "disable_after_error_count", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_DISABLE_AFTER_ERROR_COUNT, vsn(2, 2, 75), NULL, 0, NULL,
	"Number of I/O errors after which a device is skipped.\n"
	"During each LVM operation, errors received from each device\n"
	"are counted. If the counter of a device exceeds the limit set\n"
	"here, no further I/O is sent to that device for the remainder\n"
	"of the operation.\n"
	"Setting this to 0 disables the counters altogether.\n")

cfg(devices_require_restorefile_with_uuid_CFG, "require_restorefile_with_uuid", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REQUIRE_RESTOREFILE_WITH_UUID, vsn(2, 2, 73), NULL, 0, NULL,
	"Allow use of pvcreate --uuid without requiring --restorefile.\n")

cfg(devices_pv_min_size_CFG, "pv_min_size", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_PV_MIN_SIZE_KB, vsn(2, 2, 85), NULL, 0, NULL,
	"Minimum size (in KB) of block devices which can be used as PVs.\n"
	"In a clustered environment all nodes must use the same value.\n"
	"Any value smaller than 512KB is ignored.  The previous built-in\n"
	"value was 512.\n")

cfg(devices_issue_discards_CFG, "issue_discards", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ISSUE_DISCARDS, vsn(2, 2, 85), NULL, 0, NULL,
	"Issue discards to PVs that are no longer used by an LV.\n"
	"Discards are sent to an LV's underlying physical volumes when\n"
	"the LV is no longer using the physical volumes' space, e.g.\n"
	"lvremove, lvreduce.  Discards inform the storage that a region\n"
	"is no longer used.  Storage that supports discards advertise\n"
	"the protocol-specific way discards should be issued by the\n"
	"kernel (TRIM, UNMAP, or WRITE SAME with UNMAP bit set).\n"
	"Not all storage will support or benefit from discards, but SSDs\n"
	"and thinly provisioned LUNs generally do.  If enabled, discards\n"
	"will only be issued if both the storage and kernel provide support.\n")

cfg_array(allocation_cling_tag_list_CFG, "cling_tag_list", allocation_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 77), NULL, 0, NULL,
	"Advise LVM which PVs to use when searching for new space.\n"
	"When searching for free space to extend an LV, the 'cling'\n"
	"allocation policy will choose space on the same PVs as the last\n"
	"segment of the existing LV.  If there is insufficient space and a\n"
	"list of tags is defined here, it will check whether any of them are\n"
	"attached to the PVs concerned and then seek to match those PV tags\n"
	"between existing extents and new extents.\n"
	"Example:\n"
	"Use the special tag \"@*\" as a wildcard to match any PV tag.\n"
	"cling_tag_list = [ \"@*\" ]\n"
	"Example:\n"
	"LVs are mirrored between two sites within a single VG.\n"
	"PVs are tagged with either @site1 or @site2 to indicate where\n"
	"they are situated.\n"
	"cling_tag_list = [ \"@site1\", \"@site2\" ]\n")

cfg(allocation_maximise_cling_CFG, "maximise_cling", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MAXIMISE_CLING, vsn(2, 2, 85), NULL, 0, NULL,
	"Use a previous allocation algorithm.\n"
	"Changes made in version 2.02.85 extended the reach of the 'cling'\n"
	"policies to detect more situations where data can be grouped onto\n"
	"the same disks.  This setting can be used to disable the changes\n"
	"and revert to the previous algorithm.\n")

cfg(allocation_use_blkid_wiping_CFG, "use_blkid_wiping", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(2, 2, 105), NULL, 0, NULL,
	"Use blkid to detect existing signatures on new PVs and LVs.\n"
	"The blkid library can detect more signatures than the\n"
	"native LVM detection code, but may take longer.\n"
	"LVM needs to be compiled with blkid wiping support for\n"
	"this setting to apply.\n"
	"LVM native detection code is currently able to recognize:\n"
	"MD device signatures, swap signature, and LUKS signatures.\n"
	"To see the list of signatures recognized by blkid, check the\n"
	"output of the 'blkid -k' command.\n")

cfg(allocation_wipe_signatures_when_zeroing_new_lvs_CFG, "wipe_signatures_when_zeroing_new_lvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(2, 2, 105), NULL, 0, NULL,
	"Look for and erase any signatures while zeroing a new LV.\n"
	"Zeroing is controlled by the -Z/--zero option, and if not\n"
	"specified, zeroing is used by default if possible.\n"
	"Zeroing simply overwrites the first 4 KiB of a new LV\n"
	"with zeroes and does no signature detection or wiping.\n"
	"Signature wiping goes beyond zeroing and detects exact\n"
	"types and positions of signatures within the whole LV.\n"
	"It provides a cleaner LV after creation as all known\n"
	"signatures are wiped.  The LV is not claimed incorrectly\n"
	"by other tools because of old signatures from previous use.\n"
	"The number of signatures that LVM can detect depends on the\n"
	"detection code that is selected (see use_blkid_wiping.)\n"
	"Wiping each detected signature must be confirmed.\n"
	"The command line option -W/--wipesignatures takes precedence\n"
	"over this setting.\n"
	"When this setting is disabled, signatures on new LVs are\n"
	"not detected or erased unless the -W/--wipesignatures y\n"
	"option is used directly.\n")

cfg(allocation_mirror_logs_require_separate_pvs_CFG, "mirror_logs_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MIRROR_LOGS_REQUIRE_SEPARATE_PVS, vsn(2, 2, 85), NULL, 0, NULL,
	"Mirror logs and images will always use different PVs.\n"
	"The default setting changed in version 2.02.85.\n")

cfg(allocation_cache_pool_metadata_require_separate_pvs_CFG, "cache_pool_metadata_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_CACHE_POOL_METADATA_REQUIRE_SEPARATE_PVS, vsn(2, 2, 106), NULL, 0, NULL,
	"Cache pool metadata and data will always use different PVs.\n")

cfg(allocation_cache_pool_cachemode_CFG, "cache_pool_cachemode", allocation_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_CACHE_POOL_CACHEMODE, vsn(2, 2, 113), NULL, 0, NULL,
	"The default cache mode used for new cache pools.\n"
	"Possible options are: writethrough, writeback.\n"
	"writethrough - Data blocks are immediately written from\n"
	"the cache to disk.\n"
	"writeback - Data blocks are written from the cache back\n"
	"to disk after some delay to improve performance.\n")

cfg_runtime(allocation_cache_pool_chunk_size_CFG, "cache_pool_chunk_size", allocation_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_INT, vsn(2, 2, 106), 0, NULL,
	"The minimal chunk size (in kiB) for cache pool volumes.\n"
	"Using a chunk_size that is too large can result in wasteful\n"
	"use of the cache, where small reads and writes can cause\n"
	"large sections of an LV to be mapped into the cache.  However,\n"
	"choosing a chunk_size that is too small can result in more\n"
	"overhead trying to manage the numerous chunks that become mapped\n"
	"into the cache.  The former is more of a problem than the latter\n"
	"in most cases, so we default to a value that is on the smaller\n"
	"end of the spectrum.  Supported values range from 32(kiB) to\n"
	"1048576 in multiples of 32.\n")

cfg(allocation_thin_pool_metadata_require_separate_pvs_CFG, "thin_pool_metadata_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_THIN_POOL_METADATA_REQUIRE_SEPARATE_PVS, vsn(2, 2, 89), NULL, 0, NULL,
	"Thin pool metdata and data will always use different PVs.\n")

cfg(allocation_thin_pool_zero_CFG, "thin_pool_zero", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_THIN_POOL_ZERO, vsn(2, 2, 99), NULL, 0, NULL,
	"Thin pool data chunks are zeroed before they are first used.\n"
	"Zeroing with a larger thin pool chunk size reduces performance.\n")

cfg(allocation_thin_pool_discards_CFG, "thin_pool_discards", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_THIN_POOL_DISCARDS, vsn(2, 2, 99), NULL, 0, NULL,
	"The discards behaviour of thin pool volumes.\n"
	"Possible options are: ignore, nopassdown, passdown.\n")

cfg(allocation_thin_pool_chunk_size_policy_CFG, "thin_pool_chunk_size_policy", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_THIN_POOL_CHUNK_SIZE_POLICY, vsn(2, 2, 101), NULL, 0, NULL,
	"The chunk size calculation policy for thin pool volumes.\n"
	"Possible options are: generic, performance.\n"
	"generic - If thin_pool_chunk_size is defined, use it.\n"
	"Otherwise, calculate the chunk size based on estimation and\n"
	"device hints exposed in sysfs - the minimum_io_size.\n"
	"The chunk size is always at least 64KiB.\n"
	"performance - If thin_pool_chunk_size is defined, use it.\n"
	"Otherwise, calculate the chunk size for performance based on\n"
	"device hints exposed in sysfs - the optimal_io_size.\n"
	"The chunk size is always at least 512KiB.\n")

cfg_runtime(allocation_thin_pool_chunk_size_CFG, "thin_pool_chunk_size", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA | CFG_DEFAULT_UNDEFINED, CFG_TYPE_INT, vsn(2, 2, 99), 0, NULL,
	"The minimal chunk size (in KB) for thin pool volumes.\n"
	"Larger chunk sizes may improve performance for plain\n"
	"thin volumes, however using them for snapshot volumes\n"
	"is less efficient, as it consumes more space and takes\n"
	"extra time for copying.  When unset, lvm tries to estimate\n"
	"chunk size starting from 64KB.  Supported values are in\n"
	"the range 64 to 1048576.\n")

cfg(allocation_physical_extent_size_CFG, "physical_extent_size", allocation_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, DEFAULT_EXTENT_SIZE, vsn(2, 2, 112), NULL, 0, NULL,
	"Default physical extent size to use for new VGs (in KB).\n")

cfg(log_verbose_CFG, "verbose", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_VERBOSE, vsn(1, 0, 0), NULL, 0, NULL,
	"Controls the messages sent to stdout or stderr.\n")

cfg(log_silent_CFG, "silent", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SILENT, vsn(2, 2, 98), NULL, 0, NULL,
	"Suppress all non-essential messages from stdout.\n"
	"This has the same effect as -qq.\n"
	"When enabled, the following commands still produce output:\n"
	"dumpconfig, lvdisplay, lvmdiskscan, lvs, pvck, pvdisplay,\n"
	"pvs, version, vgcfgrestore -l, vgdisplay, vgs.\n"
	"Non-essential messages are shifted from log level 4 to log level 5\n"
	"for syslog and lvm2_log_fn purposes.\n"
	"Any 'yes' or 'no' questions not overridden by other arguments\n"
	"are suppressed and default to 'no'.\n")

cfg(log_syslog_CFG, "syslog", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SYSLOG, vsn(1, 0, 0), NULL, 0, NULL,
	"Send log messages through syslog.\n")

cfg(log_file_CFG, "file", log_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL,
	"Write error and debug log messages to a file specified here.\n")

cfg(log_overwrite_CFG, "overwrite", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_OVERWRITE, vsn(1, 0, 0), NULL, 0, NULL,
	"Overwrite the log file each time the program is run.\n")

cfg(log_level_CFG, "level", log_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_LOGLEVEL, vsn(1, 0, 0), NULL, 0, NULL,
	"The level of log messages that are sent to the log file or syslog.\n"
	"There are 6 syslog-like log levels currently in use: 2 to 7 inclusive.\n"
	"7 is the most verbose (LOG_DEBUG).\n")

cfg(log_indent_CFG, "indent", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_INDENT, vsn(1, 0, 0), NULL, 0, NULL,
	"Indent messages according to their severity.\n")

cfg(log_command_names_CFG, "command_names", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_CMD_NAME, vsn(1, 0, 0), NULL, 0, NULL,
	"Display the command name on each line of output.\n")

cfg(log_prefix_CFG, "prefix", log_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, DEFAULT_MSG_PREFIX, vsn(1, 0, 0), NULL, 0, NULL,
	"A prefix to use before the log message text.\n"
	"(After the command name, if selected).\n"
	"Two spaces allows you to see/grep the severity of each message.\n"
	"To make the messages look similar to the original LVM tools use:\n"
	"indent = 0, command_names = 1, prefix = \" -- \"\n")

cfg(log_activation_CFG, "activation", log_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(1, 0, 0), NULL, 0, NULL,
	"Log messages during activation.\n"
	"Don't use this in low memory situations (can deadlock).\n")

cfg(log_activate_file_CFG, "activate_file", log_CFG_SECTION, CFG_DEFAULT_UNDEFINED | CFG_UNSUPPORTED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL, NULL)

cfg_array(log_debug_classes_CFG, "debug_classes", log_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, "#Smemory#Sdevices#Sactivation#Sallocation#Slvmetad#Smetadata#Scache#Slocking#Slvmpolld", vsn(2, 2, 99), NULL, 0, NULL,
	"Select log messages by class.\n"
	"Some debugging messages are assigned to a class\n"
	"and only appear in debug output if the class is\n"
	"listed here.  Classes currently available:\n"
	"memory, devices, activation, allocation,\n"
	"lvmetad, metadata, cache, locking, lvmpolld.\n"
	"Use \"all\" to see everything.\n")

cfg(backup_backup_CFG, "backup", backup_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_BACKUP_ENABLED, vsn(1, 0, 0), NULL, 0, NULL,
	"Maintain a backup of the current metadata configuration.\n"
	"Think very hard before turning this off!\n")

cfg_runtime(backup_backup_dir_CFG, "backup_dir", backup_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 0, 0), 0, NULL,
	"Location of the metadata backup files.\n"
	"Remember to back up this directory regularly!\n")

cfg(backup_archive_CFG, "archive", backup_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ARCHIVE_ENABLED, vsn(1, 0, 0), NULL, 0, NULL,
	"Maintain an archive of old metadata configurations.\n"
	"Think very hard before turning this off.\n")

cfg_runtime(backup_archive_dir_CFG, "archive_dir", backup_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 0, 0), 0, NULL,
	"Location of the metdata archive files.\n"
	"Remember to back up this directory regularly!\n")

cfg(backup_retain_min_CFG, "retain_min", backup_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_ARCHIVE_NUMBER, vsn(1, 0, 0), NULL, 0, NULL,
	"Minimum number of archives to keep.\n")

cfg(backup_retain_days_CFG, "retain_days", backup_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_ARCHIVE_DAYS, vsn(1, 0, 0), NULL, 0, NULL,
	"Minimum number of days to keep archive files.\n")

cfg(shell_history_size_CFG, "history_size", shell_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_MAX_HISTORY, vsn(1, 0, 0), NULL, 0, NULL,
	"Number of lines of history to store in ~/.lvm_history.\n")

cfg(global_umask_CFG, "umask", global_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_UMASK, vsn(1, 0, 0), NULL, 0, NULL,
	"The file creation mask for any files and directories created.\n"
	"Interpreted as octal if the first digit is zero.\n")

cfg(global_test_CFG, "test", global_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(1, 0, 0), NULL, 0, NULL,
	"No on-disk metadata changes will be made in test mode.\n"
	"Equivalent to having the -t option on every command.\n")

cfg(global_units_CFG, "units", global_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_UNITS, vsn(1, 0, 0), NULL, 0, NULL,
	"Default value for --units argument.\n")

cfg(global_si_unit_consistency_CFG, "si_unit_consistency", global_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_SI_UNIT_CONSISTENCY,  vsn(2, 2, 54), NULL, 0, NULL,
	"Distinguish between powers of 1024 and 1000 bytes.\n"
	"The LVM commands distinguish between powers of 1024 bytes,\n"
	"e.g. KiB, MiB, GiB, and powers of 1000 bytes, e.g. KB, MB, GB.\n"
	"If scripts depend on the old behaviour, disable\n"
	"this setting temporarily until they are updated.\n")

cfg(global_suffix_CFG, "suffix", global_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_SUFFIX, vsn(1, 0, 0), NULL, 0, NULL,
	"Display unit suffix for sizes.\n"
	"This setting has no effect if the units are in human-readable\n"
	"form (global/units=\"h\") in which case the suffix is always\n"
	"displayed.\n")

cfg(global_activation_CFG, "activation", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ACTIVATION, vsn(1, 0, 0), NULL, 0, NULL,
	"Enable/disable communication with the kernel device-mapper.\n"
	"Disable to use the tools to manipulate LVM metadata without\n"
	"activating any logical volumes. If the device-mapper driver\n"
	"is not present in the kernel, disabling this should suppress\n"
	"the error messages.\n")

cfg(global_fallback_to_lvm1_CFG, "fallback_to_lvm1", global_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_LVM1, vsn(1, 0, 18), NULL, 0, NULL,
	"Try running LVM1 tools if LVM cannot communicate with DM.\n"
	"This option only applies to 2.4 kernels and is provided to\n"
	"help switch between device-mapper kernels and LVM1 kernels.\n"
	"The LVM1 tools need to be installed with .lvm1 suffices,\n"
	"e.g. vgscan.lvm1. They will stop working once the lvm2\n"
	"on-disk metadata format is used.\n")

cfg(global_format_CFG, "format", global_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_FORMAT, vsn(1, 0, 0), NULL, 0, NULL,
	"The default metadata format that commands should use.\n"
	"\"lvm1\" or \"lvm2\".\n"
	"The command line override is -M1 or -M2.\n")

cfg_array(global_format_libraries_CFG, "format_libraries", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL,
	"Shared libraries that process different metadata formats.\n"
	"If support for LVM1 metadata was compiled as a shared library use\n"
	"format_libraries = \"liblvm2format1.so\"\n")

cfg_array(global_segment_libraries_CFG, "segment_libraries", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL, 0, NULL, NULL)

cfg(global_proc_CFG, "proc", global_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_STRING, DEFAULT_PROC_DIR, vsn(1, 0, 0), NULL, 0, NULL,
	"Location of proc filesystem.\n")

cfg(global_etc_CFG, "etc", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_ETC_DIR, vsn(2, 2, 117), "@CONFDIR@", 0, NULL,
	"Location of /etc system configuration directory.\n")

cfg(global_locking_type_CFG, "locking_type", global_CFG_SECTION, 0, CFG_TYPE_INT, 1, vsn(1, 0, 0), NULL, 0, NULL,
	"Type of locking to use.\n"
	"Type 0: turns off locking. Warning: this risks metadata\n"
	"corruption if commands run concurrently.\n"
	"Type 1: uses local file-based locking, the standard mode.\n"
	"Type 2: uses the external shared library locking_library.\n"
	"Type 3: uses built-in clustered locking with clvmd.\n"
	"This is incompatible with lvmetad. If use_lvmetad is enabled,\n"
	"lvm prints a warning and disables lvmetad use.\n"
	"Type 4: uses read-only locking which forbids any operations\n"
	"that might change metadata.\n"
	"Type 5: offers dummy locking for tools that do not need any locks.\n"
	"You should not need to set this directly; the tools will select\n"
	"when to use it instead of the configured locking_type.\n"
	"Do not use lvmetad or the kernel device-mapper driver with this\n"
	"locking type. It is used by the --readonly option that offers\n"
	"read-only access to Volume Group metadata that cannot be locked\n"
	"safely because it belongs to an inaccessible domain and might be\n"
	"in use, for example a virtual machine image or a disk that is\n"
	"shared by a clustered machine.\n")

cfg(global_wait_for_locks_CFG, "wait_for_locks", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_WAIT_FOR_LOCKS, vsn(2, 2, 50), NULL, 0, NULL,
	"When disabled, fail if a lock request would block.\n")

cfg(global_fallback_to_clustered_locking_CFG, "fallback_to_clustered_locking", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_CLUSTERED_LOCKING, vsn(2, 2, 42), NULL, 0, NULL,
	"Attempt to use built-in cluster locking if locking_type 2 fails.\n"
	"If using external locking (type 2) and initialisation fails,\n"
	"with this enabled, an attempt will be made to use the built-in\n"
	"clustered locking.\n"
	"If you are using a customised locking_library you should disable this.\n")

cfg(global_fallback_to_local_locking_CFG, "fallback_to_local_locking", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_LOCAL_LOCKING, vsn(2, 2, 42), NULL, 0, NULL,
	"Use locking_type 1 (local) if locking_type 2 or 3 fail.\n"
	"If an attempt to initialise type 2 or type 3 locking failed,\n"
	"perhaps because cluster components such as clvmd are not\n"
	"running, with this enabled, an attempt will be made to use\n"
	"local file-based locking (type 1). If this succeeds, only\n"
	"commands against local volume groups will proceed.\n"
	"Volume Groups marked as clustered will be ignored.\n")

cfg(global_locking_dir_CFG, "locking_dir", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_LOCK_DIR, vsn(1, 0, 0), "@DEFAULT_LOCK_DIR@", 0, NULL,
	"Directory to use for LVM command file locks.\n"
	"Local non-LV directory that holds file-based locks\n"
	"while commands are in progress.  A directory like\n"
	"/tmp that may get wiped on reboot is OK.\n")

cfg(global_prioritise_write_locks_CFG, "prioritise_write_locks", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_PRIORITISE_WRITE_LOCKS, vsn(2, 2, 52), NULL, 0, NULL,
	"Allow quicker VG write access during high volume read access.\n"
	"When there are competing read-only and read-write access\n"
	"requests for a volume group's metadata, instead of always\n"
	"granting the read-only requests immediately, delay them to\n"
	"allow the read-write requests to be serviced.  Without this\n"
	"setting, write access may be stalled by a high volume of\n"
	"read-only requests.\n"
	"This option only affects locking_type 1 viz.\n"
	"local file-based locking.\n")

cfg(global_library_dir_CFG, "library_dir", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL,
	"Search this directory first for shared libraries.\n")

cfg(global_locking_library_CFG, "locking_library", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_LOCKING_LIB, vsn(1, 0, 0), NULL, 0, NULL,
	"The external locking library to use for locking_type 2.\n")

cfg(global_abort_on_internal_errors_CFG, "abort_on_internal_errors", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ABORT_ON_INTERNAL_ERRORS, vsn(2, 2, 57), NULL, 0, NULL,
	"Abort a command that encounters an internal error.\n"
	"Treat any internal errors as fatal errors, aborting\n"
	"the process that encountered the internal error.\n"
	"Please only enable for debugging.\n")

cfg(global_detect_internal_vg_cache_corruption_CFG, "detect_internal_vg_cache_corruption", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DETECT_INTERNAL_VG_CACHE_CORRUPTION, vsn(2, 2, 96), NULL, 0, NULL,
	"Internal verification of VG structures.\n"
	"Check if CRC matches when a parsed VG is\n"
	"used multiple times. This is useful to catch\n"
	"unexpected changes to cached VG structures.\n"
	"Please only enable for debugging.\n")

cfg(global_metadata_read_only_CFG, "metadata_read_only", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_METADATA_READ_ONLY, vsn(2, 2, 75), NULL, 0, NULL,
	"No operations that change on-disk metadata are permitted.\n"
	"Additionally, read-only commands that encounter metadata\n"
	"in need of repair will still be allowed to proceed exactly\n"
	"as if the repair had been performed (except for the unchanged\n"
	"vg_seqno). Inappropriate use could mess up your system,\n"
	"so seek advice first!\n")

cfg(global_mirror_segtype_default_CFG, "mirror_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_SEGTYPE, vsn(2, 2, 87), "@DEFAULT_MIRROR_SEGTYPE@", 0, NULL,
	"The segment type used by the short mirroring option -m.\n"
	"Possible options are: mirror, raid1.\n"
	"mirror - the original RAID1 implementation from LVM/DM.\n"
	"It is characterized by a flexible log solution (core,\n"
	"disk, mirrored), and by the necessity to block I/O while\n"
	"handling a failure.\n"
	"There is an inherent race in the dmeventd failure\n"
	"handling logic with snapshots of devices using this\n"
	"type of RAID1 that in the worst case could cause a\n"
	"deadlock. (Also see devices/ignore_lvm_mirrors.)\n"
	"raid1 - a newer RAID1 implementation using the MD RAID1\n"
	"personality through device-mapper.  It is characterized\n"
	"by a lack of log options. (A log is always allocated for\n"
	"every device and they are placed on the same device as the\n"
	"image - no separate devices are required.)  This mirror\n"
	"implementation does not require I/O to be blocked while\n"
	"handling a failure. This mirror implementation is not\n"
	"cluster-aware and cannot be used in a shared (active/active)\n"
	"fashion in a cluster.\n"
	"The '--type mirror|raid1' option overrides this setting.\n")

cfg(global_raid10_segtype_default_CFG, "raid10_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_RAID10_SEGTYPE, vsn(2, 2, 99), "@DEFAULT_RAID10_SEGTYPE@", 0, NULL,
	"The segment type used by the -i -m combination.\n"
	"The --stripes/-i and --mirrors/-m options can both\n"
	"be specified during the creation of a logical volume\n"
	"to use both striping and mirroring for the LV.\n"
	"There are two different implementations.\n"
	"Possible options are: raid10, mirror.\n"
	"raid10 - LVM uses MD's RAID10 personality through DM.\n"
	"mirror - LVM layers the 'mirror' and 'stripe' segment types.\n"
	"The layering is done by creating a mirror LV on top of\n"
	"striped sub-LVs, effectively creating a RAID 0+1 array.\n"
	"The layering is suboptimal in terms of providing redundancy\n"
	"and performance. The 'raid10' option is perferred.\n"
	"The '--type raid10|mirror' option overrides this setting.\n")

cfg(global_sparse_segtype_default_CFG, "sparse_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_SPARSE_SEGTYPE, vsn(2, 2, 112), "@DEFAULT_SPARSE_SEGTYPE@", 0, NULL,
	"The segment type used by the -V -L combination.\n"
	"The combination of -V and -L options creates a\n"
	"sparse LV. There are two different implementations.\n"
	"Possible options are: snapshot, thin.\n"
	"snapshot - The original snapshot implementation from LVM/DM.\n"
	"It uses an old snapshot that mixes data and metadata within\n"
	"a single COW storage volume and performs poorly when the\n"
	"size of stored data passes hundreds of MB.\n"
	"thin - A newer implementation that uses thin provisioning.\n"
	"It has a bigger minimal chunk size (64KiB) and uses a separate\n"
	"volume for metadata. It has better performance, especially\n"
	"when more data is used.  It also supports full snapshots.\n"
	"The '--type snapshot|thin' option overrides this setting.\n")

cfg(global_lvdisplay_shows_full_device_path_CFG, "lvdisplay_shows_full_device_path", global_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_LVDISPLAY_SHOWS_FULL_DEVICE_PATH, vsn(2, 2, 89), NULL, 0, NULL,
	"The default format for displaying LV names in lvdisplay was changed\n"
	"in version 2.02.89 to show the LV name and path separately.\n"
	"Previously this was always shown as /dev/vgname/lvname even when that\n"
	"was never a valid path in the /dev filesystem.\n"
	"Enable this option to reinstate the previous format.\n")

cfg(global_use_lvmetad_CFG, "use_lvmetad", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_LVMETAD, vsn(2, 2, 93), NULL, 0, NULL,
	"Use lvmetad to cache metadata and reduce disk scanning.\n"
	"When enabled (and running), lvmetad provides LVM commands\n"
	"with VG metadata and PV state.  LVM commands then avoid\n"
	"reading this information from disks which can be slow.\n"
	"When disabled (or not running), LVM commands fall back to\n"
	"scanning disks to obtain VG metadata.\n"
	"lvmetad is kept updated via udev rules which must be set\n"
	"up for LVM to work correctly. (The udev rules should be\n"
	"installed by default.) Without a proper udev setup, changes\n"
	"in the system's block device configuration will be unknown\n"
	"to LVM, and ignored until a manual 'pvscan --cache' is run.\n"
	"If lvmetad was running while use_lvmetad was disabled,\n"
	"it must be stopped, use_lvmetad enabled, and then started.\n"
	"When using lvmetad, LV activation is switched to an automatic,\n"
	"event-based mode.  In this mode, LVs are activated based on\n"
	"incoming udev events that inform lvmetad when PVs appear on\n"
	"the system. When a VG is complete (all PVs present), it is\n"
	"auto-activated. The auto_activation_volume_list setting\n"
	"controls which LVs are auto-activated (all by default.)\n"
	"When lvmetad is updated (automatically by udev events, or\n"
	"directly by pvscan --cache), devices/filter is ignored and\n"
	"all devices are scanned by default. lvmetad always keeps\n"
	"unfiltered information which is provided to LVM commands.\n"
	"Each LVM command then filters based on devices/filter.\n"
	"This does not apply to other, non-regexp, filtering settings:\n"
	"component filters such as multipath and MD are checked\n"
	"during pvscan --cache.\n"
	"To filter a device and prevent scanning from the LVM system\n"
	"entirely, including lvmetad, use devices/global_filter.\n"
	"lvmetad is not compatible with locking_type 3 (clustering).\n"
	"LVM prints warnings and ignores lvmetad if this combination\n"
	"is seen.\n")

cfg(global_thin_check_executable_CFG, "thin_check_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, THIN_CHECK_CMD, vsn(2, 2, 94), "@THIN_CHECK_CMD@", 0, NULL,
	"The full path to the thin_check command.\n"
	"LVM uses this command to check that a thin metadata\n"
	"device is in a usable state.\n"
	"When a thin pool is activated and after it is deactivated,\n"
	"this command is run. Activation will only proceed if the\n"
	"command has an exit status of 0.\n"
	"Set to \"\" to skip this check.  (Not recommended.)\n"
	"Also see thin_check_options.\n"
	"The thin tools are available from the package\n"
	"device-mapper-persistent-data.\n")

cfg(global_thin_dump_executable_CFG, "thin_dump_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, THIN_DUMP_CMD, vsn(2, 2, 100), "@THIN_DUMP_CMD@", 0, NULL,
	"The full path to the thin_dump command.\n"
	"LVM uses this command to dump thin pool metadata.\n"
	"(For thin tools, see thin_check_executable.)\n")

cfg(global_thin_repair_executable_CFG, "thin_repair_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, THIN_REPAIR_CMD, vsn(2, 2, 100), "@THIN_REPAIR_CMD@", 0, NULL,
	"The full path to the thin_repair command.\n"
	"LVM uses this command to repair a thin metadata device\n"
	"if it is in an unusable state.\n"
	"Also see thin_repair_options.\n"
	"(For thin tools, see thin_check_executable.)\n")

cfg_array(global_thin_check_options_CFG, "thin_check_options", global_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, "#S" DEFAULT_THIN_CHECK_OPTION1 "#S" DEFAULT_THIN_CHECK_OPTION2, vsn(2, 2, 96), NULL, 0, NULL,
	"List of options passed to the thin_check command.\n"
	"With thin_check version 2.1 or newer you can add\n"
	"--ignore-non-fatal-errors to let it pass through\n"
	"ignorable errors and fix them later.\n"
	"With thin_check version 3.2 or newer you should add\n"
	"--clear-needs-check-flag.\n")

cfg_array(global_thin_repair_options_CFG, "thin_repair_options", global_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, "#S" DEFAULT_THIN_REPAIR_OPTIONS, vsn(2, 2, 100), NULL, 0, NULL,
	"List of options passed to the thin_repair command.\n")

cfg_array(global_thin_disabled_features_CFG, "thin_disabled_features", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, "#S", vsn(2, 2, 99), NULL, 0, NULL,
	"Features to not use in the thin driver.\n"
	"This can be helpful for testing, or to avoid\n"
	"using a feature that is causing problems.\n"
	"Features: block_size, discards, discards_non_power_2,\n"
	"external_origin, metadata_resize, external_origin_extend,\n"
	"error_if_no_space.\n"
	"Example:\n"
	"thin_disabled_features = [ \"discards\", \"block_size\" ]\n")

cfg(global_cache_check_executable_CFG, "cache_check_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, CACHE_CHECK_CMD, vsn(2, 2, 108), "@CACHE_CHECK_CMD@", 0, NULL,
	"The full path to the cache_check command.\n"
	"LVM uses this command to check that a cache metadata\n"
	"device is in a usable state.\n"
	"When a cached LV is activated and after it is deactivated,\n"
	"this command is run. Activation will only proceed if the\n"
	"command has an exit status of 0.\n"
	"Set to \"\" to skip this check.  (Not recommended.)\n"
	"Also see cache_check_options.\n"
	"The cache tools are available from the package\n"
	"device-mapper-persistent-data.\n")

cfg(global_cache_dump_executable_CFG, "cache_dump_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, CACHE_DUMP_CMD, vsn(2, 2, 108), "@CACHE_DUMP_CMD@", 0, NULL,
	"The full path to the cache_dump command.\n"
	"LVM uses this command to dump cache pool metadata.\n"
	"(For cache tools, see cache_check_executable.)\n")

cfg(global_cache_repair_executable_CFG, "cache_repair_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, CACHE_REPAIR_CMD, vsn(2, 2, 108), "@CACHE_REPAIR_CMD@", 0, NULL,
	"The full path to the cache_repair command.\n"
	"LVM uses this command to repair a cache metadata device\n"
	"if it is in an unusable state.\n"
	"Also see cache_repair_options.\n"
	"(For cache tools, see cache_check_executable.)\n")

cfg_array(global_cache_check_options_CFG, "cache_check_options", global_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, "#S" DEFAULT_CACHE_CHECK_OPTION1, vsn(2, 2, 108), NULL, 0, NULL,
	"List of options passed to the cache_check command.\n")

cfg_array(global_cache_repair_options_CFG, "cache_repair_options", global_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, "#S" DEFAULT_CACHE_REPAIR_OPTIONS, vsn(2, 2, 108), NULL, 0, NULL,
	"List of options passed to the cache_repair command.\n")

cfg(global_system_id_source_CFG, "system_id_source", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_SYSTEM_ID_SOURCE, vsn(2, 2, 117), NULL, 0, NULL,
	"The method LVM uses to set the local system ID.\n"
	"Volume Groups can also be given a system ID (by\n"
	"vgcreate, vgchange, or vgimport.)\n"
	"A VG on shared storage devices is accessible only\n"
	"to the host with a matching system ID.\n"
	"See 'man lvmsystemid' for information on limitations\n"
	"and correct usage.\n"
	"Possible options are: none, lvmlocal, uname, machineid, file.\n"
	"none - The host has no system ID.\n"
	"lvmlocal - Obtain the system ID from the system_id setting in the\n"
	"'local' section of an lvm configuration file, e.g. lvmlocal.conf.\n"
	"uname - Set the system ID from the hostname (uname) of the system.\n"
	"System IDs beginning localhost are not permitted.\n"
	"machineid - Use the contents of the machine-id file to set the\n"
	"system ID.  Some systems create this file at installation time.\n"
	"See 'man machine-id' and global/etc.\n"
	"file - Use the contents of another file (system_id_file) to set\n"
	"the system ID.\n")

cfg(global_system_id_file_CFG, "system_id_file", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 117), NULL, 0, NULL,
	"The full path to the file containing a system ID.\n"
	"This is used when system_id_source is set to 'file'.\n"
	"Comments starting with the character # are ignored.\n")

cfg(activation_checks_CFG, "checks", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ACTIVATION_CHECKS, vsn(2, 2, 86), NULL, 0, NULL,
	"Perform internal checks of libdevmapper operations.\n"
	"Useful for debugging problems with activation.\n"
	"Some of the checks may be expensive, so it's best to use\n"
	"this only when there seems to be a problem.\n")

cfg(global_use_lvmpolld_CFG, "use_lvmpolld", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_LVMPOLLD, vsn(2, 2, 120), NULL, 0, NULL,
	"Use lvmpolld to supervise long running LVM commands.\n"
	"When enabled, control of long running LVM commands is transferred\n"
	"from the original LVM command to the lvmpolld daemon.  This allows\n"
	"the operation to continue independent of the original LVM command.\n"
	"After lvmpolld takes over, the LVM command displays the progress\n"
	"of the ongoing operation.  lvmpolld itself runs LVM commands to manage\n"
	"the progress of ongoing operations.  lvmpolld can be used as a native\n"
	"systemd service, which allows it to be started on demand, and to use\n"
	"its own control group.  When this option is disabled, LVM commands will\n"
	"supervise long running operations by forking themselves.\n")

cfg(activation_udev_sync_CFG, "udev_sync", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_UDEV_SYNC, vsn(2, 2, 51), NULL, 0, NULL,
	"Use udev notifications to synchronize udev and LVM.\n"
	"When disabled, LVM commands will not wait for notifications\n"
	"from udev, but continue irrespective of any possible udev\n"
	"processing in the background.  Only use this if udev is not\n"
	"running or has rules that ignore the devices LVM creates.\n"
	"If enabled when udev is not running, and LVM processes\n"
	"are waiting for udev, run 'dmsetup udevcomplete_all' to\n"
	"wake them up.\n"
	"The '--nodevsync' option overrides this setting.\n")

cfg(activation_udev_rules_CFG, "udev_rules", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_UDEV_RULES, vsn(2, 2, 57), NULL, 0, NULL,
	"Use udev rules to manage LV device nodes and symlinks.\n"
	"When disabled, LVM will manage the device nodes and\n"
	"symlinks for active LVs itself.\n"
	"Manual intervention may be required if this setting is\n"
	"changed while LVs are active.\n")

cfg(activation_verify_udev_operations_CFG, "verify_udev_operations", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_VERIFY_UDEV_OPERATIONS, vsn(2, 2, 86), NULL, 0, NULL,
	"Use extra checks in LVM to verify udev operations.\n"
	"This enables additional checks (and if necessary,\n"
	"repairs) on entries in the device directory after\n"
	"udev has completed processing its events.\n"
	"Useful for diagnosing problems with LVM/udev interactions.\n")

cfg(activation_retry_deactivation_CFG, "retry_deactivation", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_RETRY_DEACTIVATION, vsn(2, 2, 89), NULL, 0, NULL,
	"Retry failed LV deactivation.\n"
	"If LV deactivation fails, LVM will retry for a few\n"
	"seconds before failing. This may happen because a\n"
	"process run from a quick udev rule temporarily opened\n"
	"the device.\n")

cfg(activation_missing_stripe_filler_CFG, "missing_stripe_filler", activation_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_STRING, DEFAULT_STRIPE_FILLER, vsn(1, 0, 0), NULL, 0, NULL,
	"Method to fill missing stripes when activating an incomplete LV.\n"
	"Using 'error' will make inaccessible parts of the device return\n"
	"I/O errors on access.  You can instead use a device path, in which\n"
	"case, that device will be used in place of missing stripes.\n"
	"Using anything other than 'error' with mirrored or snapshotted\n"
	"volumes is likely to result in data corruption.\n")

cfg(activation_use_linear_target_CFG, "use_linear_target", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_LINEAR_TARGET, vsn(2, 2, 89), NULL, 0, NULL,
	"Use the linear target to optimize single stripe LVs.\n"
	"When disabled, the striped target is used. The linear\n"
	"target is an optimised version of the striped target\n"
	"that only handles a single stripe.\n")

cfg(activation_reserved_stack_CFG, "reserved_stack", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RESERVED_STACK, vsn(1, 0, 0), NULL, 0, NULL,
	"Stack size in KB to reserve for use while devices are suspended.\n"
	"Insufficent reserve risks I/O deadlock during device suspension.\n")

cfg(activation_reserved_memory_CFG, "reserved_memory", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RESERVED_MEMORY, vsn(1, 0, 0), NULL, 0, NULL,
	"Memory size in KB to reserve for use while devices are suspended.\n"
	"Insufficent reserve risks I/O deadlock during device suspension.\n")

cfg(activation_process_priority_CFG, "process_priority", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_PROCESS_PRIORITY, vsn(1, 0, 0), NULL, 0, NULL,
	"Nice value used while devices are suspended.\n"
	"Use a high priority so that LVs are suspended\n"
	"for the shortest possible time.\n")

cfg_array(activation_volume_list_CFG, "volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL, 0, NULL,
	"Only LVs selected by this list are activated.\n"
	"If this list is defined, an LV is only activated\n"
	"if it matches an entry in this list.\n"
	"If this list is undefined, it imposes no limits\n"
	"on LV activation (all are allowed).\n"
	"Possible options are: vgname, vgname/lvname, @tag, @*\n"
	"vgname is matched exactly and selects all LVs in the VG.\n"
	"vgname/lvname is matched exactly and selects the LV.\n"
	"@tag selects if tag matches a tag set on the LV or VG.\n"
	"@* selects if a tag defined on the host is also set on\n"
	"the LV or VG.  See tags/hosttags.\n"
	"If any host tags exist but volume_list is not defined,\n"
	"a default single-entry list containing '@*' is assumed.\n"
	"Example:\n"
	"volume_list = [ \"vg1\", \"vg2/lvol1\", \"@tag1\", \"@*\" ]\n")

cfg_array(activation_auto_activation_volume_list_CFG, "auto_activation_volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 97), NULL, 0, NULL,
	"Only LVs selected by this list are auto-activated.\n"
	"This list works like volume_list, but it is used\n"
	"only by auto-activation commands. It does not apply\n"
	"to direct activation commands.\n"
	"If this list is defined, an LV is only auto-activated\n"
	"if it matches an entry in this list.\n"
	"If this list is undefined, it imposes no limits\n"
	"on LV auto-activation (all are allowed.)\n"
	"If this list is defined and empty, i.e. \"[]\",\n"
	"then no LVs are selected for auto-activation.\n"
	"An LV that is selected by this list for\n"
	"auto-activation, must also be selected by\n"
	"volume_list (if defined) before it is activated.\n"
	"Auto-activation is an activation command that\n"
	"includes the 'a' argument: --activate ay or -a ay,\n"
	"e.g. vgchange -a ay, or lvchange -a ay vgname/lvname.\n"
	"The 'a' (auto) argument for auto-activation is\n"
	"meant to be used by activation commands that are\n"
	"run automatically by the system, as opposed to\n"
	"LVM commands run directly by a user. A user may\n"
	"also use the 'a' flag directly to perform auto-\n"
	"activation.\n"
	"An example of a system-generated auto-activation\n"
	"command is 'pvscan --cache -aay' which is generated\n"
	"when udev and lvmetad detect a new VG has appeared\n"
	"on the system, and want LVs in it to be auto-activated.\n"
	"Possible options are: vgname, vgname/lvname, @tag, @*\n"
	"See volume_list for how these options are matched to LVs.\n")

cfg_array(activation_read_only_volume_list_CFG, "read_only_volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 89), NULL, 0, NULL,
	"LVs in this list are activated in read-only mode.\n"
	"If this list is defined, each LV that is to be activated\n"
	"is checked against this list, and if it matches, it is\n"
	"activated in read-only mode.\n"
	"This overrides the permission setting stored in the\n"
	"metadata, e.g. from --permission rw.\n"
	"Possible options are: vgname, vgname/lvname, @tag, @*\n"
	"See volume_list for how these options are matched to LVs.\n")

cfg(activation_mirror_region_size_CFG, "mirror_region_size", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RAID_REGION_SIZE, vsn(1, 0, 0), NULL, vsn(2, 2, 99),
	"This has been replaced by the activation/raid_region_size setting.\n",
        "Size (in KB) of each copy operation when mirroring.\n")

cfg(activation_raid_region_size_CFG, "raid_region_size", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RAID_REGION_SIZE, vsn(2, 2, 99), NULL, 0, NULL,
	"Size in KiB of each raid or mirror synchronization region.\n"
	"For raid or mirror segment types, this is the amount of\n"
	"data that is copied at once when initializing, or moved\n"
	"at once by pvmove.\n")

cfg(activation_error_when_full_CFG, "error_when_full", activation_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_ERROR_WHEN_FULL, vsn(2, 2, 115), NULL, 0, NULL,
	"Return errors if a thin pool runs out of space.\n"
	"When enabled, writes to thin LVs immediately return\n"
	"an error if the thin pool is out of data space.\n"
	"When disabled, writes to thin LVs are queued if the\n"
	"thin pool is out of space, and processed when the\n"
	"thin pool data space is extended.\n"
	"New thin pools are assigned the behavior defined here.\n"
	"The '--errorwhenfull y|n' option overrides this setting.\n")

cfg(activation_readahead_CFG, "readahead", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_READ_AHEAD, vsn(1, 0, 23), NULL, 0, NULL,
	"Setting to use when there is no readahead setting in metadata.\n"
	"Possible options are: none, auto.\n"
	"none - Disable readahead.\n"
	"auto - Use default value chosen by kernel.\n")

cfg(activation_raid_fault_policy_CFG, "raid_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_RAID_FAULT_POLICY, vsn(2, 2, 89), NULL, 0, NULL,
	"Defines how a device failure in a RAID LV is handled.\n"
	"This includes LVs that have the following segment types:\n"
	"raid1, raid4, raid5*, and raid6*.\n"
	"If a device in the LV fails, the policy determines the\n"
	"steps perfomed by dmeventd automatically, and the steps\n"
	"perfomed by 'lvconvert --repair --use-policies' run manually.\n"
	"Automatic handling requires dmeventd to be monitoring the LV.\n"
	"Possible options are: warn, allocate.\n"
	"warn - Use the system log to warn the user that a device\n"
	"in the RAID LV has failed.  It is left to the user to run\n"
	"'lvconvert --repair' manually to remove or replace the failed\n"
	"device.  As long as the number of failed devices does not\n"
	"exceed the redundancy of the logical volume (1 device for\n"
	"raid4/5, 2 for raid6, etc) the LV will remain usable.\n"
	"allocate - Attempt to use any extra physical volumes in the\n"
	"volume group as spares and replace faulty devices.\n")

cfg_runtime(activation_mirror_image_fault_policy_CFG, "mirror_image_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(2, 2, 57), 0, NULL,
	"Defines how a device failure in a 'mirror' LV is handled.\n"
	"An LV with the 'mirror' segment type is composed of mirror\n"
	"images (copies) and a mirror log.\n"
	"A disk log ensures that a mirror LV does not need to be\n"
	"re-synced (all copies made the same) every time a machine\n"
	"reboots or crashes.\n"
	"If a device in the LV fails, this policy determines the\n"
	"steps perfomed by dmeventd automatically, and the steps\n"
	"performed by 'lvconvert --repair --use-policies' run manually.\n"
	"Automatic handling requires dmeventd to be monitoring the LV.\n"
	"Possible options are: remove, allocate, allocate_anywhere.\n"
	"remove - Simply remove the faulty device and run without it.\n"
	"If the log device fails, the mirror would convert to using\n"
	"an in-memory log.  This means the mirror will not\n"
	"remember its sync status across crashes/reboots and\n"
	"the entire mirror will be re-synced.\n"
	"If a mirror image fails, the mirror will convert to a\n"
	"non-mirrored device if there is only one remaining good copy.\n"
	"allocate - Remove the faulty device and try to allocate space\n"
	"on a new device to be a replacement for the failed device.\n"
	"Using this policy for the log is fast and maintains the\n"
	"ability to remember sync state through crashes/reboots.\n"
	"Using this policy for a mirror device is slow, as it\n"
	"requires the mirror to resynchronize the devices, but it\n"
	"will preserve the mirror characteristic of the device.\n"
	"This policy acts like 'remove' if no suitable device and\n"
	"space can be allocated for the replacement.\n"
	"allocate_anywhere - Not yet implemented. Useful to place\n"
	"the log device temporarily on the same physical volume as\n"
	"one of the mirror images. This policy is not recommended\n"
	"for mirror devices since it would break the redundant nature\n"
	"of the mirror. This policy acts like 'remove' if no suitable\n"
	"device and space can be allocated for the replacement.\n")

cfg(activation_mirror_log_fault_policy_CFG, "mirror_log_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_LOG_FAULT_POLICY, vsn(1, 2, 18), NULL, 0, NULL,
	"Defines how a device failure in a 'mirror' log LV is handled.\n"
	"The mirror_image_fault_policy description for mirrored LVs\n"
	"also applies to mirrored log LVs.\n")

cfg(activation_mirror_device_fault_policy_CFG, "mirror_device_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_DEVICE_FAULT_POLICY, vsn(1, 2, 10), NULL, vsn(2, 2, 57),
	"This has been replaced by the activation/mirror_image_fault_policy setting.\n",
        "Define how a device failure affecting a mirror is handled.\n")

cfg(activation_snapshot_autoextend_threshold_CFG, "snapshot_autoextend_threshold", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_SNAPSHOT_AUTOEXTEND_THRESHOLD, vsn(2, 2, 75), NULL, 0, NULL,
	"Auto-extend a snapshot when its usage exceeds this percent.\n"
	"Setting this to 100 disables automatic extension.\n"
	"The minimum value is 50 (a smaller value is treated as 50.)\n"
	"Also see snapshot_autoextend_percent.\n"
	"Automatic extension requires dmeventd to be monitoring the LV.\n"
	"Example:\n"
	"With snapshot_autoextend_threshold 70 and\n"
	"snapshot_autoextend_percent 20, whenever a snapshot\n"
	"exceeds 70% usage, it will be extended by another 20%.\n"
	"For a 1G snapshot, using 700M will trigger a resize to 1.2G.\n"
	"When the usage exceeds 840M, the snapshot will be extended\n"
	"to 1.44G, and so on.\n")

cfg(activation_snapshot_autoextend_percent_CFG, "snapshot_autoextend_percent", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_SNAPSHOT_AUTOEXTEND_PERCENT, vsn(2, 2, 75), NULL, 0, NULL,
	"Auto-extending a snapshot adds this percent extra space.\n"
	"The amount of additional space added to a snapshot is this\n"
	"percent of its current size.\n"
	"Also see snapshot_autoextend_threshold.\n")

cfg(activation_thin_pool_autoextend_threshold_CFG, "thin_pool_autoextend_threshold", activation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA, CFG_TYPE_INT, DEFAULT_THIN_POOL_AUTOEXTEND_THRESHOLD, vsn(2, 2, 89), NULL, 0, NULL,
	"Auto-extend a thin pool when its usage exceeds this percent.\n"
	"Setting this to 100 disables automatic extension.\n"
	"The minimum value is 50 (a smaller value is treated as 50.)\n"
	"Also see thin_pool_autoextend_percent.\n"
	"Automatic extension requires dmeventd to be monitoring the LV.\n"
	"Example:\n"
	"With thin_pool_autoextend_threshold 70 and\n"
	"thin_pool_autoextend_percent 20, whenever a thin pool\n"
	"exceeds 70% usage, it will be extended by another 20%.\n"
	"For a 1G thin pool, using up 700M will trigger a resize to 1.2G.\n"
	"When the usage exceeds 840M, the thin pool will be extended\n"
	"to 1.44G, and so on.\n")

cfg(activation_thin_pool_autoextend_percent_CFG, "thin_pool_autoextend_percent", activation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA, CFG_TYPE_INT, DEFAULT_THIN_POOL_AUTOEXTEND_PERCENT, vsn(2, 2, 89), NULL, 0, NULL,
	"Auto-extending a thin pool adds this percent extra space.\n"
	"The amount of additional space added to a thin pool is this\n"
	"percent of its current size.\n")

cfg_array(activation_mlock_filter_CFG, "mlock_filter", activation_CFG_SECTION, CFG_DEFAULT_UNDEFINED | CFG_ADVANCED, CFG_TYPE_STRING, NULL, vsn(2, 2, 62), NULL, 0, NULL,
	"Do not mlock these memory areas.\n"
	"While activating devices, I/O to devices being\n"
	"(re)configured is suspended. As a precaution against\n"
	"deadlocks, LVM pins memory it is using so it is not\n"
	"paged out, and will not require I/O to reread.\n"
	"Groups of pages that are known not to be accessed during\n"
	"activation do not need to be pinned into memory.\n"
	"Each string listed in this setting is compared against\n"
	"each line in /proc/self/maps, and the pages corresponding\n"
	"to lines that match are not pinned.  On some systems,\n"
	"locale-archive was found to make up over 80% of the memory\n"
	"used by the process.\n"
	"Example:\n"
	"mlock_filter=[ \"locale/locale-archive\", \"gconv/gconv-modules.cache\" ]\n")

cfg(activation_use_mlockall_CFG, "use_mlockall", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_MLOCKALL, vsn(2, 2, 62), NULL, 0, NULL,
	"Use the old behavior of mlockall to pin all memory.\n"
	"Prior to version 2.02.62, LVM used mlockall() to pin\n"
	"the whole process's memory while activating devices.\n")

cfg(activation_monitoring_CFG, "monitoring", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DMEVENTD_MONITOR, vsn(2, 2, 63), NULL, 0, NULL,
	"Monitor LVs that are activated.\n"
	"When enabled, LVM will ask dmeventd to monitor LVs\n"
	"that are activated.\n"
	"The '--ignoremonitoring' option overrides this setting.\n")

cfg(activation_polling_interval_CFG, "polling_interval", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_INTERVAL, vsn(2, 2, 63), NULL, 0, NULL,
	"Check pvmove or lvconvert progress at this interval (seconds)\n"
	"When pvmove or lvconvert must wait for the kernel to finish\n"
	"synchronising or merging data, they check and report progress\n"
	"at intervals of this number of seconds.\n"
	"If this is set to 0 and there is only one thing to wait for,\n"
	"there are no progress reports, but the process is awoken\n"
	"immediately once the operation is complete.\n")

cfg(activation_auto_set_activation_skip_CFG, "auto_set_activation_skip", activation_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_AUTO_SET_ACTIVATION_SKIP, vsn(2,2,99), NULL, 0, NULL,
	"Set the activation skip flag on new thin snapshot LVs.\n"
	"An LV can have a persistent 'activation skip' flag.\n"
	"The flag causes the LV to be skipped during normal activation.\n"
	"The lvchange/vgchange -K option is required to activate LVs\n"
	"that have the activation skip flag set.\n"
	"When this setting is enabled, the activation skip flag is\n"
	"set on new thin snapshot LVs.\n"
	"The '--setactivationskip y|n' option overrides this setting.\n")

cfg(activation_mode_CFG, "activation_mode", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_ACTIVATION_MODE, vsn(2,2,108), NULL, 0, NULL,
	"How LVs with missing devices are activated.\n"
	"Possible options are: complete, degraded, partial.\n"
	"complete - Only allow activation of an LV if all of\n"
	"the Physical Volumes it uses are present.  Other PVs\n"
	"in the Volume Group may be missing.\n"
	"degraded - Like complete, but additionally RAID LVs of\n"
	"segment type raid1, raid4, raid5, radid6 and raid10 will\n"
	"be activated if there is no data loss, i.e. they have\n"
	"sufficient redundancy to present the entire addressable\n"
	"range of the Logical Volume.\n"
	"partial - Allows the activation of any LV even if a\n"
	"missing or failed PV could cause data loss with a\n"
	"portion of the Logical Volume inaccessible.\n"
	"This setting should not normally be used, but may\n"
	"sometimes assist with data recovery.\n"
	"The '--activationmode' option overrides this setting.\n")

cfg(metadata_pvmetadatacopies_CFG, "pvmetadatacopies", metadata_CFG_SECTION, CFG_ADVANCED | CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, DEFAULT_PVMETADATACOPIES, vsn(1, 0, 0), NULL, 0, NULL,
	"Number of copies of metadata to store on each PV.\n"
	"Possible options are: 0, 1, 2.\n"
	"If set to 2, two copies of the VG metadata are stored on\n"
	"the PV, one at the front of the PV, and one at the end.\n"
	"If set to 1, one copy is stored at the front of the PV.\n"
	"If set to 0, no copies are stored on the PV. This may\n"
	"be useful with VGs containing large numbers of PVs.\n"
	"The '--pvmetadatacopies' option overrides this setting.\n")

cfg(metadata_vgmetadatacopies_CFG, "vgmetadatacopies", metadata_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, DEFAULT_VGMETADATACOPIES, vsn(2, 2, 69), NULL, 0, NULL,
	"Number of copies of metadata to maintain for each VG.\n"
	"If set to a non-zero value, LVM automatically chooses which of\n"
	"the available metadata areas to use to achieve the requested\n"
	"number of copies of the VG metadata.  If you set a value larger\n"
	"than the the total number of metadata areas available, then\n"
	"metadata is stored in them all.\n"
	"The value 0 (unmanaged) disables this automatic management\n"
	"and allows you to control which metadata areas are used at\n"
	"the individual PV level using 'pvchange --metadataignore y|n'.\n"
	"The '--vgmetadatacopies' option overrides this setting.\n")

cfg(metadata_pvmetadatasize_CFG, "pvmetadatasize", metadata_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, DEFAULT_PVMETADATASIZE, vsn(1, 0, 0), NULL, 0, NULL,
	"Approximate number of sectors to use for each metadata copy.\n"
	"VGs with large numbers of PVs or LVs, or VGs containing\n"
	"complex LV structures, may need additional space for VG\n"
	"metadata. The metadata areas are treated as circular buffers,\n"
	"so unused space becomes filled with an archive of the most\n"
	"recent previous versions of the metadata.\n")

cfg(metadata_pvmetadataignore_CFG, "pvmetadataignore", metadata_CFG_SECTION, CFG_ADVANCED | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_PVMETADATAIGNORE, vsn(2, 2, 69), NULL, 0, NULL,
	"Ignore metadata areas on a new PV.\n"
	"If metadata areas on a PV are ignored, LVM will not store\n"
	"metadata in them.\n"
	"The '--metadataignore' option overrides this setting.\n")

cfg(metadata_stripesize_CFG, "stripesize", metadata_CFG_SECTION, CFG_ADVANCED | CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, DEFAULT_STRIPESIZE, vsn(1, 0, 0), NULL, 0, NULL, NULL)

cfg_array(metadata_dirs_CFG, "dirs", metadata_CFG_SECTION, CFG_ADVANCED | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL,
	"Directories holding live copies of text format metadata.\n"
	"These directories must not be on logical volumes!\n"
	"It's possible to use LVM with a couple of directories here,\n"
	"preferably on different (non-LV) filesystems, and with no other\n"
	"on-disk metadata (pvmetadatacopies = 0). Or this can be in\n"
	"addition to on-disk metadata areas.\n"
	"The feature was originally added to simplify testing and is not\n"
	"supported under low memory situations - the machine could lock up.\n"
	"Never edit any files in these directories by hand unless you\n"
	"you are absolutely sure you know what you are doing! Use\n"
	"the supplied toolset to make changes (e.g. vgcfgrestore).\n"
	"Example:\n"
	"dirs = [ \"/etc/lvm/metadata\", \"/mnt/disk2/lvm/metadata2\" ]\n")

cfg_section(metadata_disk_areas_CFG_SUBSECTION, "disk_areas", metadata_CFG_SECTION, CFG_UNSUPPORTED | CFG_DEFAULT_COMMENTED, vsn(1, 0, 0), 0, NULL, NULL)
cfg_section(disk_area_CFG_SUBSECTION, "disk_area", metadata_disk_areas_CFG_SUBSECTION, CFG_NAME_VARIABLE | CFG_UNSUPPORTED | CFG_DEFAULT_COMMENTED, vsn(1, 0, 0), 0, NULL, NULL)
cfg(disk_area_start_sector_CFG, "start_sector", disk_area_CFG_SUBSECTION, CFG_UNSUPPORTED | CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, 0, vsn(1, 0, 0), NULL, 0, NULL, NULL)
cfg(disk_area_size_CFG, "size", disk_area_CFG_SUBSECTION, CFG_UNSUPPORTED | CFG_DEFAULT_COMMENTED, CFG_TYPE_INT, 0, vsn(1, 0, 0), NULL, 0, NULL, NULL)
cfg(disk_area_id_CFG, "id", disk_area_CFG_SUBSECTION, CFG_UNSUPPORTED | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL, 0, NULL, NULL)

cfg(report_compact_output_CFG, "compact_output", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_REP_COMPACT_OUTPUT, vsn(2, 2, 115), NULL, 0, NULL,
	"Do not print empty report fields.\n"
	"Fields that don't have a value set for any of the rows\n"
	"reported are skipped and not printed. Compact output is\n"
	"applicable only if report/buffered is enabled.\n")

cfg(report_aligned_CFG, "aligned", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_REP_ALIGNED, vsn(1, 0, 0), NULL, 0, NULL,
	"Align columns in report output.\n")

cfg(report_buffered_CFG, "buffered", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_REP_BUFFERED, vsn(1, 0, 0), NULL, 0, NULL,
	"Buffer report output.\n"
	"When buffered reporting is used, the report's content is appended\n"
	"incrementally to include each object being reported until the report\n"
	"is flushed to output which normally happens at the end of command\n"
	"execution. Otherwise, if buffering is not used, each object is\n"
	"reported as soon as its processing is finished.\n")

cfg(report_headings_CFG, "headings", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_REP_HEADINGS, vsn(1, 0, 0), NULL, 0, NULL,
	"Show headings for columns on report.\n")

cfg(report_separator_CFG, "separator", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_REP_SEPARATOR, vsn(1, 0, 0), NULL, 0, NULL,
	"A separator to use on report after each field.\n")

cfg(report_list_item_separator_CFG, "list_item_separator", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_REP_LIST_ITEM_SEPARATOR, vsn(2, 2, 108), NULL, 0, NULL,
	"A separator to use for list items when reported.\n")

cfg(report_prefixes_CFG, "prefixes", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_REP_PREFIXES, vsn(2, 2, 36), NULL, 0, NULL,
	"Use a field name prefix for each field reported.\n")

cfg(report_quoted_CFG, "quoted", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_REP_QUOTED, vsn(2, 2, 39), NULL, 0, NULL,
	"Quote field values when using field name prefixes.\n")

cfg(report_colums_as_rows_CFG, "colums_as_rows", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_REP_COLUMNS_AS_ROWS, vsn(1, 0, 0), NULL, 0, NULL,
	"Output each column as a row.\n"
	"If set, this also implies report/prefixes=1.\n")

cfg(report_binary_values_as_numeric_CFG, "binary_values_as_numeric", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, 0, vsn(2, 2, 108), NULL, 0, NULL,
	"Use binary values 0 or 1 instead of descriptive literal values.\n"
	"For columns that have exactly two valid values to report\n"
	"(not counting the 'unknown' value which denotes that the\n"
	"value could not be determined).\n")

cfg(report_devtypes_sort_CFG, "devtypes_sort", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_DEVTYPES_SORT, vsn(2, 2, 101), NULL, 0, NULL,
	"List of columns to sort by when reporting 'lvm devtypes' command.\n"
	"See 'lvm devtypes -o help' for the list of possible fields.\n")

cfg(report_devtypes_cols_CFG, "devtypes_cols", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_DEVTYPES_COLS, vsn(2, 2, 101), NULL, 0, NULL,
	"List of columns to report for 'lvm devtypes' command.\n"
	"See 'lvm devtypes -o help' for the list of possible fields.\n")

cfg(report_devtypes_cols_verbose_CFG, "devtypes_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_DEVTYPES_COLS_VERB, vsn(2, 2, 101), NULL, 0, NULL,
	"List of columns to report for 'lvm devtypes' command in verbose mode.\n"
	"See 'lvm devtypes -o help' for the list of possible fields.\n")

cfg(report_lvs_sort_CFG, "lvs_sort", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_LVS_SORT, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to sort by when reporting 'lvs' command.\n"
	"See 'lvs -o help' for the list of possible fields.\n")

cfg(report_lvs_cols_CFG, "lvs_cols", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_LVS_COLS, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'lvs' command.\n"
	"See 'lvs -o help' for the list of possible fields.\n")

cfg(report_lvs_cols_verbose_CFG, "lvs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_LVS_COLS_VERB, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'lvs' command in verbose mode.\n"
	"See 'lvs -o help' for the list of possible fields.\n")

cfg(report_vgs_sort_CFG, "vgs_sort", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_VGS_SORT, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to sort by when reporting 'vgs' command.\n"
	"See 'vgs -o help' for the list of possible fields.\n")

cfg(report_vgs_cols_CFG, "vgs_cols", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_VGS_COLS, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'vgs' command.\n"
	"See 'vgs -o help' for the list of possible fields.\n")

cfg(report_vgs_cols_verbose_CFG, "vgs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_VGS_COLS_VERB, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'vgs' command in verbose mode.\n"
	"See 'vgs -o help' for the list of possible fields.\n")

cfg(report_pvs_sort_CFG, "pvs_sort", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_PVS_SORT, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to sort by when reporting 'pvs' command.\n"
	"See 'pvs -o help' for the list of possible fields.\n")

cfg(report_pvs_cols_CFG, "pvs_cols", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_PVS_COLS, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'pvs' command.\n"
	"See 'pvs -o help' for the list of possible fields.\n")

cfg(report_pvs_cols_verbose_CFG, "pvs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_PVS_COLS_VERB, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'pvs' command in verbose mode.\n"
	"See 'pvs -o help' for the list of possible fields.\n")

cfg(report_segs_sort_CFG, "segs_sort", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_SEGS_SORT, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to sort by when reporting 'lvs --segments' command.\n"
	"See 'lvs --segments -o help' for the list of possible fields.\n")

cfg(report_segs_cols_CFG, "segs_cols", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_SEGS_COLS, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'lvs --segments' command.\n"
	"See 'lvs --segments  -o help' for the list of possible fields.\n")

cfg(report_segs_cols_verbose_CFG, "segs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_SEGS_COLS_VERB, vsn(1, 0, 0), NULL, 0, NULL,
	"List of columns to report for 'lvs --segments' command in verbose mode.\n"
	"See 'lvs --segments -o help' for the list of possible fields.\n")

cfg(report_pvsegs_sort_CFG, "pvsegs_sort", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_PVSEGS_SORT, vsn(1, 1, 3), NULL, 0, NULL,
	"List of columns to sort by when reporting 'pvs --segments' command.\n"
	"See 'pvs --segments -o help' for the list of possible fields.\n")

cfg(report_pvsegs_cols_CFG, "pvsegs_cols", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_PVSEGS_COLS, vsn(1, 1, 3), NULL, 0, NULL,
	"List of columns to sort by when reporting 'pvs --segments' command.\n"
	"See 'pvs --segments -o help' for the list of possible fields.\n")

cfg(report_pvsegs_cols_verbose_CFG, "pvsegs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_PVSEGS_COLS_VERB, vsn(1, 1, 3), NULL, 0, NULL,
	"List of columns to sort by when reporting 'pvs --segments' command in verbose mode.\n"
	"See 'pvs --segments -o help' for the list of possible fields.\n")

cfg(dmeventd_mirror_library_CFG, "mirror_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_MIRROR_LIB, vsn(1, 2, 3), NULL, 0, NULL,
	"The library dmeventd uses when monitoring a mirror device.\n"
	"libdevmapper-event-lvm2mirror.so attempts to recover from\n"
	"failures.  It removes failed devices from a volume group and\n"
	"reconfigures a mirror as necessary. If no mirror library is\n"
	"provided, mirrors are not monitored through dmeventd.\n")

cfg(dmeventd_raid_library_CFG, "raid_library", dmeventd_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_DMEVENTD_RAID_LIB, vsn(2, 2, 87), NULL, 0, NULL, NULL)

cfg(dmeventd_snapshot_library_CFG, "snapshot_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_SNAPSHOT_LIB, vsn(1, 2, 26), NULL, 0, NULL,
	"The library dmeventd uses when monitoring a snapshot device.\n"
	"libdevmapper-event-lvm2snapshot.so monitors the filling of\n"
	"snapshots and emits a warning through syslog when the usage\n"
	"exceeds 80%. The warning is repeated when 85%, 90% and\n"
	"95% of the snapshot is filled.\n")

cfg(dmeventd_thin_library_CFG, "thin_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_THIN_LIB, vsn(2, 2, 89), NULL, 0, NULL,
	"The library dmeventd uses when monitoring a thin device.\n"
	"libdevmapper-event-lvm2thin.so monitors the filling of\n"
	"a pool and emits a warning through syslog when the usage\n"
	"exceeds 80%. The warning is repeated when 85%, 90% and\n"
	"95% of the pool is filled.\n")

cfg(dmeventd_executable_CFG, "executable", dmeventd_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, DEFAULT_DMEVENTD_PATH, vsn(2, 2, 73), "@DMEVENTD_PATH@", 0, NULL,
	"The full path to the dmeventd binary.\n")

cfg(tags_hosttags_CFG, "hosttags", tags_CFG_SECTION, CFG_DEFAULT_COMMENTED, CFG_TYPE_BOOL, DEFAULT_HOSTTAGS, vsn(1, 0, 18), NULL, 0, NULL,
	"Create a host tag using the machine name.\n"
	"The machine name is nodename returned by uname(2).\n")

cfg_section(tag_CFG_SUBSECTION, "tag", tags_CFG_SECTION, CFG_NAME_VARIABLE | CFG_DEFAULT_COMMENTED, vsn(1, 0, 18), 0, NULL,
	"Replace this subsection name with a custom tag name.\n"
	"Multiple subsections like this can be created.\n"
	"The '@' prefix for tags is optional.\n"
	"This subsection can contain host_list, which is a\n"
	"list of machine names. If the name of the local\n"
	"machine is found in host_list, then the name of\n"
	"this subsection is used as a tag and is applied\n"
	"to the local machine as a 'host tag'.\n"
	"If this subsection is empty (has no host_list), then\n"
	"the subsection name is always applied as a 'host tag'.\n"
	"Example:\n"
	"The host tag foo is given to all hosts, and the host tag\n"
	"bar is given to the hosts named machine1 and machine2.\n"
	"tags { foo { } bar { host_list = [ \"machine1\", \"machine2\" ] } }\n")

cfg_array(tag_host_list_CFG, "host_list", tag_CFG_SUBSECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL, 0, NULL,
	"A list of machine names.\n"
	"These machine names are compared to the nodename\n"
	"returned by uname(2). If the local machine name\n"
	"matches an entry in this list, the name of the\n"
	"subsection is applied to the machine as a 'host tag'.\n")

cfg(local_system_id_CFG, "system_id", local_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, NULL, vsn(2, 2, 117), NULL, 0, NULL,
	"Defines the local system ID for lvmlocal mode.\n"
	"This is used when global/system_id_source is set\n"
	"to 'lvmlocal' in the main configuration file,\n"
	"e.g. lvm.conf.\n"
	"When used, it must be set to a unique value\n"
	"among all hosts sharing access to the storage,\n"
	"e.g. a host name.\n"
	"Example:\n"
	"Set no system ID.\n"
	"system_id = \"\"\n"
	"Example:\n"
	"Set the system_id to the string 'host1'.\n"
	"system_id = \"host1\"\n")

cfg_array(local_extra_system_ids_CFG, "extra_system_ids", local_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_COMMENTED, CFG_TYPE_STRING, "#S", vsn(2, 2, 117), NULL, 0, NULL,
	"A list of extra VG system IDs the local host can access.\n"
	"VGs with the system IDs listed here (in addition\n"
	"to the host's own system ID) can be fully accessed\n"
	"by the local host.  (These are system IDs that the\n"
	"host sees in VGs, not system IDs that identify the\n"
	"local host, which is determined by system_id_source.)\n"
	"Use this only after consulting 'man lvmsystemid'\n"
	"to be certain of correct usage and possible dangers.\n")

cfg(CFG_COUNT, NULL, root_CFG_SECTION, 0, CFG_TYPE_INT, 0, vsn(0, 0, 0), NULL, 0, NULL, NULL)
