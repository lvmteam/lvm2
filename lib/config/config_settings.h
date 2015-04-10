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
 *   cfg_section(id, name, parent, flags, since_version, comment)
 *
 * - define a configuration setting of simple type:
 *   cfg(id, name, parent, flags, type, default_value, since_version, comment)
 *
 * - define a configuration array of one or more types:
 *   cfg_array(id, name, parent, flags, types, default_value, since_version, comment)
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
 * id:			unique identifier
 * name:		configuration node name
 * parent:		id of parent configuration node
 * flags:		configuration item flags:
 * 				CFG_NAME_VARIABLE - configuration node name is variable
 * 				CFG_ALLOW_EMPTY - node value can be emtpy
 * 				CFG_ADVANCED - this node belongs to advanced config set
 * 				CFG_UNSUPPORTED - this node belongs to unsupported config set
 * 				CFG_PROFILABLE - this node is customizable by a profile
 * 				CFG_PROFILABLE_METADATA - profilable and attachable to VG/LV metadata
 * 				CFG_DEFAULT_UNDEFINED - node's default value is undefined
 * 				CFG_DISABLED - configuration is disabled (defaults always used)
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
 *			Use "vsn" macro to translate the "major.minor.release" version
 *			into a single number that is being stored internally in memory.
 */
#include "defaults.h"

cfg_section(root_CFG_SECTION, "(root)", root_CFG_SECTION, 0, vsn(0, 0, 0), NULL)

cfg_section(config_CFG_SECTION, "config", root_CFG_SECTION, 0, vsn(2, 2, 99),
	"This section allows you to set the way the configuration settings are handled.\n")

cfg_section(devices_CFG_SECTION, "devices", root_CFG_SECTION, 0, vsn(1, 0, 0),
	"This section allows you to configure which block devices should\n"
	"be used by the LVM system.\n")

cfg_section(allocation_CFG_SECTION, "allocation", root_CFG_SECTION, CFG_PROFILABLE, vsn(2, 2, 77),
	"This section allows you to configure the way in which LVM selects\n"
	"free space for its Logical Volumes.\n")

cfg_section(log_CFG_SECTION, "log", root_CFG_SECTION, 0, vsn(1, 0, 0),
	"This section that allows you to configure the nature of the\n"
	"information that LVM reports.\n")

cfg_section(backup_CFG_SECTION, "backup", root_CFG_SECTION, 0, vsn(1, 0, 0),
	"Configuration of metadata backups and archiving.  In LVM when we\n"
	"talk about a 'backup' we mean making a copy of the metadata for the\n"
	"*current* system.  The 'archive' contains old metadata configurations.\n"
	"Backups are stored in a human readable text format.\n")

cfg_section(shell_CFG_SECTION, "shell", root_CFG_SECTION, 0, vsn(1, 0, 0),
	"Settings for the running LVM in shell (readline) mode.\n")

cfg_section(global_CFG_SECTION, "global", root_CFG_SECTION, CFG_PROFILABLE, vsn(1, 0, 0),
	"Miscellaneous global LVM settings.\n")

cfg_section(activation_CFG_SECTION, "activation", root_CFG_SECTION, CFG_PROFILABLE, vsn(1, 0, 0), NULL)
cfg_section(metadata_CFG_SECTION, "metadata", root_CFG_SECTION, CFG_ADVANCED, vsn(1, 0, 0), NULL)
cfg_section(report_CFG_SECTION, "report", root_CFG_SECTION, CFG_ADVANCED | CFG_PROFILABLE, vsn(1, 0, 0), NULL)
cfg_section(dmeventd_CFG_SECTION, "dmeventd", root_CFG_SECTION, 0, vsn(1, 2, 3), NULL)
cfg_section(tags_CFG_SECTION, "tags", root_CFG_SECTION, 0, vsn(1, 0, 18), NULL)
cfg_section(local_CFG_SECTION, "local", root_CFG_SECTION, 0, vsn(2, 2, 117), NULL)

		
cfg(config_checks_CFG, "checks", config_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(2, 2, 99),
	"If enabled, any LVM configuration mismatch is reported.\n"
	"This implies checking that the configuration key is understood\n"
	"by LVM and that the value of the key is of a proper type.\n"
	"If disabled, any configuration mismatch is ignored and default\n"
	"value is used instead without any warning (a message about the\n"
	"configuration key not being found is issued in verbose mode only).\n")

cfg(config_abort_on_errors_CFG, "abort_on_errors", config_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(2,2,99),
	"If enabled, any configuration mismatch aborts the LVM process.\n")

cfg_runtime(config_profile_dir_CFG, "profile_dir", config_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(2, 2, 99),
	"Directory where LVM looks for configuration profiles.\n")

cfg(devices_dir_CFG, "dir", devices_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DEV_DIR, vsn(1, 0, 0),
	"Where do you want your volume groups to appear ?")

cfg_array(devices_scan_CFG, "scan", devices_CFG_SECTION, 0, CFG_TYPE_STRING, "#S/dev", vsn(1, 0, 0),
	"An array of directories that contain the device nodes you wish\n"
	"to use with LVM.\n")

cfg_array(devices_loopfiles_CFG, "loopfiles", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 2, 0), NULL)

cfg(devices_obtain_device_list_from_udev_CFG, "obtain_device_list_from_udev", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_OBTAIN_DEVICE_LIST_FROM_UDEV, vsn(2, 2, 85),
	"If set, the cache of block device nodes with all associated symlinks\n"
	"will be constructed out of the existing udev database content.\n"
	"This avoids using and opening any inapplicable non-block devices or\n"
	"subdirectories found in the device directory. This setting is applied\n"
	"to udev-managed device directory only, other directories will be scanned\n"
	"fully. LVM needs to be compiled with udev support for this setting to\n"
	"take effect. N.B. Any device node or symlink not managed by udev in\n"
	"udev directory will be ignored with this setting on.\n")

cfg(devices_external_device_info_source_CFG, "external_device_info_source", devices_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_EXTERNAL_DEVICE_INFO_SOURCE, vsn(2, 2, 116),
	"Select external device information source to use for further and more\n"
	"detailed device determination. Some information may already be available\n"
	"in the system and LVM can use this information to determine the exact\n"
	"type or use of the device it processes. Using existing external device\n"
	"information source can speed up device processing as LVM does not need\n"
	"to run its own native routines to acquire this information. For example,\n"
	"such information is used to drive LVM filtering like MD component\n"
	"detection, multipath component detection, partition detection and others.\n"
	"Possible options are: none, udev.\n"
	"none - No external device information source is used.\n"
	"udev - Reuse existing udev database records. Applicable\n"
	"only if LVM is compiled with udev support.\n")

cfg_array(devices_preferred_names_CFG, "preferred_names", devices_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 2, 19),
	"If several entries in the scanned directories correspond to the\n"
	"same block device and the tools need to display a name for device,\n"
	"all the pathnames are matched against each item in the following\n"
	"list of regular expressions in turn and the first match is used.\n"
	"Try to avoid using undescriptive /dev/dm-N names, if present.\n"
	"In case no prefererred name matches or if preferred_names are not\n"
	"defined at all, builtin rules are used to determine the preference.\n"
	"The first builtin rule checks path prefixes and it gives preference\n"
	"based on this ordering (where /dev depends on devices/dev setting):\n"
	"/dev/mapper > /dev/disk > /dev/dm-* > /dev/block\n"
	"If the ordering above cannot be applied, the path with fewer slashes\n"
	"gets preference then.\n"
	"If the number of slashes is the same, a symlink gets preference.\n"
	"Finally, if all the rules mentioned above are not applicable,\n"
	"lexicographical order is used over paths and the smallest one\n"
	"of all gets preference.\n"
	"Example:\n"
	"preferred_names = [ \"^/dev/mpath/\", \"^/dev/mapper/mpath\", \"^/dev/[hs]d\" ]\n")

cfg_array(devices_filter_CFG, "filter", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0),
	"A filter that tells LVM to only use a restricted set of devices.\n"
	"The filter consists of an array of regular expressions.  These\n"
	"expressions can be delimited by a character of your choice, and\n"
	"prefixed with either an 'a' (for accept) or 'r' (for reject).\n"
	"The first expression found to match a device name determines if\n"
	"the device will be accepted or rejected (ignored).  Devices that\n"
	"don't match any patterns are accepted.\n"
	"Be careful if there there are symbolic links or multiple filesystem\n"
	"entries for the same device as each name is checked separately against\n"
	"the list of patterns.  The effect is that if the first pattern in the\n"
	"list to match a name is an 'a' pattern for any of the names, the device\n"
	"is accepted; otherwise if the first pattern in the list to match a name\n"
	"is an 'r' pattern for any of the names it is rejected; otherwise it is\n"
	"accepted.\n"
	"Don't have more than one filter line active at once: only one gets used.\n"
	"Run vgscan after you change this parameter to ensure that\n"
	"the cache file gets regenerated (see below).\n"
	"If it doesn't do what you expect, check the output of 'vgscan -vvvv'.\n"
	"If lvmetad is used, then see \"A note about device filtering while\n"
	"lvmetad is used\" comment that is attached to global/use_lvmetad setting.\n"
	"Examples:\n"
	"By default we accept every block device:\n"
	"filter = [ \"a/.*/\" ]\n"
	"Exclude the cdrom drive:\n"
	"filter = [ \"r|/dev/cdrom|\" ]\n"
	"When testing I like to work with just loopback devices:\n"
	"filter = [ \"a/loop/\", \"r/.*/\" ]\n"
	"Or maybe all loops and ide drives except hdc:\n"
	"filter =[ \"a|loop|\", \"r|/dev/hdc|\", \"a|/dev/ide|\", \"r|.*|\" ]\n"
	"Use anchors if you want to be really specific:\n"
	"filter = [ \"a|^/dev/hda8$|\", \"r/.*/\" ]\n")

cfg_array(devices_global_filter_CFG, "global_filter", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 98),
	"Since filter is often overridden from command line, it is not suitable\n"
	"for system-wide device filtering (udev rules, lvmetad). To hide devices\n"
	"from LVM-specific udev processing and/or from lvmetad, you need to set\n"
	"global_filter. The syntax is the same as for normal filter\n"
	"above. Devices that fail the global_filter are not even opened by LVM.\n")

cfg_runtime(devices_cache_CFG, "cache", devices_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 0, 0),
	"This setting has been replaced by the cache_dir setting.\n")

cfg_runtime(devices_cache_dir_CFG, "cache_dir", devices_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 2, 19),
	"The results of the filtering are cached on disk to avoid\n"
	"rescanning dud devices (which can take a very long time).\n"
	"By default this cache is stored in a file named .cache\n"
	"in the directory specified by this setting.\n"
	"It is safe to delete the contents: the tools regenerate it.\n"
	"(The old setting 'cache' is still respected if neither of\n"
	"these new ones is present.)\n"
	"N.B. If obtain_device_list_from_udev is set to 1 the list of\n"
	"devices is instead obtained from udev and any existing .cache\n"
	"file is removed.\n")

cfg(devices_cache_file_prefix_CFG, "cache_file_prefix", devices_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, DEFAULT_CACHE_FILE_PREFIX, vsn(1, 2, 19),
	"A prefix used before the .cache file name.\n")

cfg(devices_write_cache_state_CFG, "write_cache_state", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(1, 0, 0),
	"Enable/disable writing the cache file.\n")

cfg_array(devices_types_CFG, "types", devices_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_INT | CFG_TYPE_STRING, NULL, vsn(1, 0, 0),
	"List of pairs of additional acceptable block device types found\n"
	"in /proc/devices with maximum (non-zero) number of partitions.\n"
	"Example:\n"
	"types = [ \"fd\", 16 ]\n")

cfg(devices_sysfs_scan_CFG, "sysfs_scan", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SYSFS_SCAN, vsn(1, 0, 8),
	"If sysfs is mounted (2.6 kernels) restrict device scanning to\n"
	"the block devices it believes are valid.\n")

cfg(devices_multipath_component_detection_CFG, "multipath_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MULTIPATH_COMPONENT_DETECTION, vsn(2, 2, 89),
	"Ignore devices used as component paths of device-mapper multipath devices.\n")

cfg(devices_md_component_detection_CFG, "md_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MD_COMPONENT_DETECTION, vsn(1, 0, 18),
	"Ignore devices used as components of software RAID (md) devices by looking\n"
	"for md superblocks.\n")

cfg(devices_fw_raid_component_detection_CFG, "fw_raid_component_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FW_RAID_COMPONENT_DETECTION, vsn(2, 2, 112),
	"Ignore devices used as components of firmware RAID devices.\n"
	"N.B. LVM itself is not detecting firmware RAID - an\n"
	"external_device_info_source other than none must be used for\n"
	"this detection to execute.\n")

cfg(devices_md_chunk_alignment_CFG, "md_chunk_alignment", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MD_CHUNK_ALIGNMENT, vsn(2, 2, 48),
	"If a PV is placed directly upon an md device, align its data blocks\n"
	"with the md device's stripe-width.\n")

cfg(devices_default_data_alignment_CFG, "default_data_alignment", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_DATA_ALIGNMENT, vsn(2, 2, 75),
	"Default alignment of the start of a data area in MB. If set to 0,\n"
	"a value of 64KB will be used.  Set to 1 for 1MiB, 2 for 2MiB, etc.\n")

cfg(devices_data_alignment_detection_CFG, "data_alignment_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DATA_ALIGNMENT_DETECTION, vsn(2, 2, 51),
	"The start of a PV's data area will be a multiple of the minimum_io_size\n"
	"or optimal_io_size exposed in sysfs.\n"
	"minimum_io_size - the smallest request the device can perform\n"
	"without incurring a read-modify-write penalty (e.g. MD's chunk size)\n"
	"optimal_io_size - the device's preferred unit of receiving I/O\n"
	"(e.g. MD's stripe width)\n"
	"minimum_io_size is used if optimal_io_size is undefined (0).\n"
	"If md_chunk_alignment is enabled, that detects the optimal_io_size.\n"
	"This setting takes precedence over md_chunk_alignment.\n")

cfg(devices_data_alignment_CFG, "data_alignment", devices_CFG_SECTION, 0, CFG_TYPE_INT, 0, vsn(2, 2, 45),
	"Alignment (in KB) of start of data area when creating a new PV.\n"
	"md_chunk_alignment and data_alignment_detection are disabled if set.\n"
	"Set to 0 for the default alignment (see: data_alignment_default)\n"
	"or page size, if larger.\n")

cfg(devices_data_alignment_offset_detection_CFG, "data_alignment_offset_detection", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DATA_ALIGNMENT_OFFSET_DETECTION, vsn(2, 2, 50),
	"The start of the PV's aligned data area will be shifted by\n"
	"the alignment_offset exposed in sysfs.  This offset is often 0 but\n"
	"may be non-zero; e.g.: certain 4KB sector drives that compensate for\n"
	"windows partitioning will have an alignment_offset of 3584 bytes\n"
	"(sector 7 is the lowest aligned logical block, the 4KB sectors start\n"
	"at LBA -1, and consequently sector 63 is aligned on a 4KB boundary).\n"
	"But note that pvcreate --dataalignmentoffset will skip this detection.\n")

cfg(devices_ignore_suspended_devices_CFG, "ignore_suspended_devices", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_IGNORE_SUSPENDED_DEVICES, vsn(1, 2, 19),
	"While scanning the system for PVs, skip a device-mapper device\n"
	"that has its I/O suspended. Otherwise, LVM waits for the device\n"
	"to become accessible. This should only be needed in recovery situations.\n")

cfg(devices_ignore_lvm_mirrors_CFG, "ignore_lvm_mirrors", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_IGNORE_LVM_MIRRORS, vsn(2, 2, 104),
	"This setting determines whether logical volumes of 'mirror' segment\n"
	"type are scanned for LVM labels.  This affects the ability of\n"
	"mirrors to be used as physical volumes.  If ignore_lvm_mirrors\n"
	"is enabled, it becomes impossible to create volume groups on top\n"
	"of mirror logical volumes - i.e. to stack volume groups on mirrors.\n"
	"Allowing mirror logical volumes to be scanned (ignore_lvm_mirrors is\n"
	"disabled) can potentially cause LVM processes and I/O to the mirror to\n"
	"become blocked.  This is due to the way that the 'mirror' segment type\n"
	"handles failures.  In order for the hang to manifest itself, an LVM\n"
	"command must be run just after a failure and before the automatic LVM\n"
	"repair process takes place OR there must be failures in multiple mirrors\n"
	"in the same volume group at the same time with write failures occurring\n"
	"moments before a scan of the mirror's labels.\n"
	"Note that these scanning limitations do not apply to the LVM RAID\n"
	"types, like 'raid1'.  The RAID segment types handle failures in a\n"
	"different way and are not subject to possible process or I/O blocking.\n"
	"It is encouraged that users enable ignore_lvm_mirrors if they are using\n"
	"the 'mirror' segment type.  Users that require volume group stacking on\n"
	"mirrored logical volumes should consider using the 'raid1' segment type.\n"
	"The 'raid1' segment type is not available for active/active clustered\n"
	"volume groups.  Enable ignore_lvm_mirrors to disallow stacking and\n"
	"thereby avoid a possible deadlock.\n")

cfg(devices_disable_after_error_count_CFG, "disable_after_error_count", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_DISABLE_AFTER_ERROR_COUNT, vsn(2, 2, 75),
	"During each LVM operation errors received from each device are counted.\n"
	"If the counter of a particular device exceeds the limit set here, no\n"
	"further I/O is sent to that device for the remainder of the respective\n"
	"operation. Setting the parameter to 0 disables the counters altogether.\n")

cfg(devices_require_restorefile_with_uuid_CFG, "require_restorefile_with_uuid", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_REQUIRE_RESTOREFILE_WITH_UUID, vsn(2, 2, 73),
	"Allow use of pvcreate --uuid without requiring --restorefile.\n")

cfg(devices_pv_min_size_CFG, "pv_min_size", devices_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_PV_MIN_SIZE_KB, vsn(2, 2, 85),
	"Minimum size (in KB) of block devices which can be used as PVs.\n"
	"In a clustered environment all nodes must use the same value.\n"
	"Any value smaller than 512KB is ignored.\n"
	"The original built-in setting was 512 up to and including version 2.02.84.\n")

cfg(devices_issue_discards_CFG, "issue_discards", devices_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ISSUE_DISCARDS, vsn(2, 2, 85),
	"Issue discards to a logical volumes's underlying physical volume(s) when\n"
	"the logical volume is no longer using the physical volumes' space (e.g.\n"
	"lvremove, lvreduce, etc).  Discards inform the storage that a region is\n"
	"no longer in use.  Storage that supports discards advertise the protocol\n"
	"specific way discards should be issued by the kernel (TRIM, UNMAP, or\n"
	"WRITE SAME with UNMAP bit set).  Not all storage will support or benefit\n"
	"from discards but SSDs and thinly provisioned LUNs generally do.  If enabled,\n"
	"discards will only be issued if both the storage and kernel provide support.\n")

cfg_array(allocation_cling_tag_list_CFG, "cling_tag_list", allocation_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 77),
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

cfg(allocation_maximise_cling_CFG, "maximise_cling", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MAXIMISE_CLING, vsn(2, 2, 85),
	"Changes made in version 2.02.85 extended the reach of the 'cling'\n"
	"policies to detect more situations where data can be grouped onto\n"
	"the same disks.  This setting can be used to disable the changes\n"
	"and revert to the previous algorithm.\n")

cfg(allocation_use_blkid_wiping_CFG, "use_blkid_wiping", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(2, 2, 105),
	"Enable use of the blkid library instead of native LVM code to detect\n"
	"any existing signatures while creating new Physical Volumes and\n"
	"Logical Volumes. LVM needs to be compiled with blkid wiping support\n"
	"for this setting to take effect.\n"
	"LVM native detection code is currently able to recognize:\n"
	"MD device signatures, swap signature, and LUKS signatures.\n"
	"To see the list of signatures recognized by blkid, check the output\n"
	"of 'blkid -k' command. The blkid can recognize more signatures than\n"
	"LVM native detection code, but due to this higher number of signatures\n"
	"to be recognized, it can take more time to complete the signature scan.\n")

cfg(allocation_wipe_signatures_when_zeroing_new_lvs_CFG, "wipe_signatures_when_zeroing_new_lvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, 1, vsn(2, 2, 105),
	"Look for any signatures on a newly-created Logical Volume\n"
	"whenever zeroing of the LV is done (zeroing is controlled by -Z/--zero\n"
	"option and if not specified, zeroing is used by default if possible).\n"
	"While zeroing simply overwrites first 4 KiB of the LV with zeroes without\n"
	"doing any signature detection, signature wiping goes beyond that and it\n"
	"can detect exact type and position of signature within the whole LV.\n"
	"As such, it provides cleaner LV for use after creation as all known\n"
	"signatures are wiped so that the LV is not claimed by other tools\n"
	"incorrectly by the existence of old signature from any previous use.\n"
	"The number of signatures that LVM can detect depends on detection\n"
	"code that is selected - see also use_blkid_wiping option.\n"
	"Wiping of each detected signature must be confirmed.\n"
	"The command line option -W/--wipesignatures takes precedence\n"
	"over this setting.\n"
	"When disabled, signatures on newly-created Logical Volumes\n"
	"are never detected and wiped and you always need to use\n"
	"-W/--wipesignatures y option directly to enable this feature\n"
	"no matter whether zeroing is used or not.\n")

cfg(allocation_mirror_logs_require_separate_pvs_CFG, "mirror_logs_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_MIRROR_LOGS_REQUIRE_SEPARATE_PVS, vsn(2, 2, 85),
	"Guarantees that mirror logs will always be placed on\n"
	"different PVs from the mirror images.\n"
	"The default setting changed in version 2.02.85.\n")

cfg(allocation_cache_pool_metadata_require_separate_pvs_CFG, "cache_pool_metadata_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_CACHE_POOL_METADATA_REQUIRE_SEPARATE_PVS, vsn(2, 2, 106),
	"Guarantees that cache_pool metadata will always be\n"
	"placed on different PVs from the cache_pool data.\n")

cfg(allocation_cache_pool_cachemode_CFG, "cache_pool_cachemode", allocation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_CACHE_POOL_CACHEMODE, vsn(2, 2, 113),
	"The default cache mode used for new cache pools.\n"
	"Possible options are: writethrough, writeback.\n"
	"writethrough - Data blocks are immediately written from the cache to disk.\n"
	"writeback - Data blocks are written from the cache back to disk after\n"
	"some delay to improve performance.\n")

cfg_runtime(allocation_cache_pool_chunk_size_CFG, "cache_pool_chunk_size", allocation_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_INT, vsn(2, 2, 106),
	"The minimal chunk size (in kiB) for cache pool volumes.\n"
	"Using a chunk_size that is too large can result in wasteful use of\n"
	"the cache, where small reads and writes can cause large sections of\n"
	"an LV to be mapped into the cache.  However, choosing a chunk_size\n"
	"that is too small can result in more overhead trying to manage the\n"
	"numerous chunks that become mapped into the cache.  The former is\n"
	"more of a problem than the latter in most cases, so we default to\n"
	"a value that is on the smaller end of the spectrum.  Supported values\n"
	"range from 32(kiB) to 1048576 in multiples of 32.\n")

cfg(allocation_thin_pool_metadata_require_separate_pvs_CFG, "thin_pool_metadata_require_separate_pvs", allocation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_THIN_POOL_METADATA_REQUIRE_SEPARATE_PVS, vsn(2, 2, 89),
	"Guarantees that thin pool metadata will always\n"
	"be placed on different PVs from the pool data.\n")

cfg(allocation_thin_pool_zero_CFG, "thin_pool_zero", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA, CFG_TYPE_BOOL, DEFAULT_THIN_POOL_ZERO, vsn(2, 2, 99),
	"Enable/disable zeroing of thin pool data chunks before their first use.\n"
	"N.B. zeroing larger thin pool chunk size degrades performance.\n")

cfg(allocation_thin_pool_discards_CFG, "thin_pool_discards", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA, CFG_TYPE_STRING, DEFAULT_THIN_POOL_DISCARDS, vsn(2, 2, 99),
	"The discards behaviour of thin pool volumes.\n"
	"Possible options are: ignore, nopassdown, passdown.\n")

cfg(allocation_thin_pool_chunk_size_policy_CFG, "thin_pool_chunk_size_policy", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA, CFG_TYPE_STRING, DEFAULT_THIN_POOL_CHUNK_SIZE_POLICY, vsn(2, 2, 101),
	"The chunk size calculation policy for thin pool volumes.\n"
	"Possible options are: generic, performance.\n"
	"generic - if thin_pool_chunk_size is defined, use it.\n"
	"Otherwise, calculate the chunk size based on estimation and\n"
	"device hints exposed in sysfs - the minimum_io_size.\n"
	"The chunk size is always at least 64KiB.\n"
	"performance - if thin_pool_chunk_size is defined, use it.\n"
	"Otherwise, calculate the chunk size for performance based on\n"
	"device hints exposed in sysfs - the optimal_io_size.\n"
	"The chunk size is always at least 512KiB.\n")

cfg_runtime(allocation_thin_pool_chunk_size_CFG, "thin_pool_chunk_size", allocation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA | CFG_DEFAULT_UNDEFINED, CFG_TYPE_INT, vsn(2, 2, 99),
	"The minimal chunk size (in KB) for thin pool volumes.\n"
	"Use of the larger chunk size may improve performance for plain\n"
	"thin volumes, however using them for snapshot volumes is less efficient,\n"
	"as it consumes more space and takes extra time for copying.\n"
	"When unset, lvm tries to estimate chunk size starting from 64KB\n"
	"Supported values are in range from 64 to 1048576.\n")

cfg(allocation_physical_extent_size_CFG, "physical_extent_size", allocation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_EXTENT_SIZE, vsn(2, 2, 112),
	"Default physical extent size to use for newly created VGs (in KB).\n")

cfg(log_verbose_CFG, "verbose", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_VERBOSE, vsn(1, 0, 0),
	"Controls the messages sent to stdout or stderr.\n")

cfg(log_silent_CFG, "silent", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SILENT, vsn(2, 2, 98),
	"Suppress all non-essential messages from stdout.\n"
	"This has the same effect as -qq.\n"
	"When enabled, the following commands still produce output:\n"
	"dumpconfig, lvdisplay, lvmdiskscan, lvs, pvck, pvdisplay,\n"
	"pvs, version, vgcfgrestore -l, vgdisplay, vgs.\n"
	"Non-essential messages are shifted from log level 4 to log level 5\n"
	"for syslog and lvm2_log_fn purposes.\n"
	"Any 'yes' or 'no' questions not overridden by other arguments\n"
	"are suppressed and default to 'no'.\n")

cfg(log_syslog_CFG, "syslog", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_SYSLOG, vsn(1, 0, 0),
	"Send log messages through syslog.\n")

cfg(log_file_CFG, "file", log_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0),
	"Write error and debug log messages to a file specified here.\n")

cfg(log_overwrite_CFG, "overwrite", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_OVERWRITE, vsn(1, 0, 0),
	"Overwrite the log file each time the program is run.\n")

cfg(log_level_CFG, "level", log_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_LOGLEVEL, vsn(1, 0, 0),
	"The level of log messages that are sent to the log file and/or syslog.\n"
	"There are 6 syslog-like log levels currently in use - 2 to 7 inclusive.\n"
	"7 is the most verbose (LOG_DEBUG).\n")

cfg(log_indent_CFG, "indent", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_INDENT, vsn(1, 0, 0),
	"Format of output messages: indent messages according to their severity.\n")

cfg(log_command_names_CFG, "command_names", log_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_CMD_NAME, vsn(1, 0, 0),
	"Format of output messages: display the command name on each line output.\n")

cfg(log_prefix_CFG, "prefix", log_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, DEFAULT_MSG_PREFIX, vsn(1, 0, 0),
	"Format of output messages: a prefix to use before the message text.\n"
	"(After the command name, if selected).\n"
	"Two spaces allows you to see/grep the severity of each message.\n"
	"To make the messages look similar to the original LVM tools use:\n"
	"indent = 0, command_names = 1, prefix = \" -- \"\n")

cfg(log_activation_CFG, "activation", log_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(1, 0, 0),
	"Log messages during activation.\n"
	"Don't use this in low memory situations (can deadlock).\n")

cfg(log_activate_file_CFG, "activate_file", log_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)

cfg_array(log_debug_classes_CFG, "debug_classes", log_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, "#Smemory#Sdevices#Sactivation#Sallocation#Slvmetad#Smetadata#Scache#Slocking", vsn(2, 2, 99),
	"Some debugging messages are assigned to a class and only appear\n"
	"in debug output if the class is listed here.\n"
	"Classes currently available:\n"
	"memory, devices, activation, allocation, lvmetad, metadata, cache, locking.\n"
	"Use \"all\" to see everything.\n")

cfg(backup_backup_CFG, "backup", backup_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_BACKUP_ENABLED, vsn(1, 0, 0),
	"Maintain a backup of the current metadata configuration.\n"
	"Think very hard before turning this off!\n")

cfg_runtime(backup_backup_dir_CFG, "backup_dir", backup_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 0, 0),
	"Location of the metadata backup files.\n"
	"Remember to back up this directory regularly!\n")

cfg(backup_archive_CFG, "archive", backup_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ARCHIVE_ENABLED, vsn(1, 0, 0),
	"Maintain an archive of old metadata configurations.\n"
	"Think very hard before turning this off.\n")

cfg_runtime(backup_archive_dir_CFG, "archive_dir", backup_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(1, 0, 0),
	"Location of the metdata archive files.\n"
	"Remember to back up this directory regularly!\n")

cfg(backup_retain_min_CFG, "retain_min", backup_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_ARCHIVE_NUMBER, vsn(1, 0, 0),
	"The minimum number of archive files you wish to keep.\n")

cfg(backup_retain_days_CFG, "retain_days", backup_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_ARCHIVE_DAYS, vsn(1, 0, 0),
	"The minimum time you wish to keep an archive file.\n")

cfg(shell_history_size_CFG, "history_size", shell_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_MAX_HISTORY, vsn(1, 0, 0),
	"Number of lines of history to store in ~/.lvm_history.\n")

cfg(global_umask_CFG, "umask", global_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_UMASK, vsn(1, 0, 0),
	"The file creation mask for any files and directories created.\n"
	"Interpreted as octal if the first digit is zero.\n")

cfg(global_test_CFG, "test", global_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(1, 0, 0),
	"Enabling test mode means that no changes to the on disk metadata\n"
	"will be made.  Equivalent to having the -t option on every command.\n")

cfg(global_units_CFG, "units", global_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_UNITS, vsn(1, 0, 0),
	"Default value for --units argument.\n")

cfg(global_si_unit_consistency_CFG, "si_unit_consistency", global_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_SI_UNIT_CONSISTENCY,  vsn(2, 2, 54),
	"The tools distinguish between powers of 1024 bytes\n"
	"(e.g. KiB, MiB, GiB) and powers of 1000 bytes (e.g. KB, MB, GB).\n"
	"If you have scripts that depend on the old behaviour, disable\n"
	"this setting temporarily until you update them.\n")

cfg(global_suffix_CFG, "suffix", global_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_SUFFIX, vsn(1, 0, 0),
	"Display unit suffix for sizes. This setting has no effect if the\n"
	"units are in human-readable form (global/units=\"h\") in which case\n"
	"the suffix is always displayed.\n")

cfg(global_activation_CFG, "activation", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ACTIVATION, vsn(1, 0, 0),
	"Enable/disable communication with the kernel device-mapper.\n"
	"Disable if you want to use the tools to manipulate LVM metadata\n"
	"without activating any logical volumes.\n"
	"If the device-mapper kernel driver is not present in your kernel,\n"
	"disabling this should suppress the error messages.\n")

cfg(global_fallback_to_lvm1_CFG, "fallback_to_lvm1", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_LVM1, vsn(1, 0, 18),
	"If we cannot communicate with device-mapper, try running the LVM1 tools.\n"
	"This option only applies to 2.4 kernels and is provided to help you\n"
	"switch between device-mapper kernels and LVM1 kernels.\n"
	"The LVM1 tools need to be installed with .lvm1 suffices\n"
	"e.g. vgscan.lvm1 and they will stop working after you start using\n"
	"the new lvm2 on-disk metadata format.\n")

cfg(global_format_CFG, "format", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_FORMAT, vsn(1, 0, 0),
	"The default metadata format that commands should use - \"lvm1\" or \"lvm2\".\n"
	"The command line override is -M1 or -M2.\n")

cfg_array(global_format_libraries_CFG, "format_libraries", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0),
	"If support for LVM1 metadata was compiled as a shared library use\n"
	"format_libraries = \"liblvm2format1.so\"\n")

cfg_array(global_segment_libraries_CFG, "segment_libraries", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL)

cfg(global_proc_CFG, "proc", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_PROC_DIR, vsn(1, 0, 0),
	"Location of proc filesystem.\n")

cfg(global_etc_CFG, "etc", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_ETC_DIR, vsn(2, 2, 117),
	"Location of /etc system configuration directory.\n")

cfg(global_locking_type_CFG, "locking_type", global_CFG_SECTION, 0, CFG_TYPE_INT, 1, vsn(1, 0, 0),
	"Type of locking to use. Defaults to local file-based locking (1).\n"
	"Turn locking off by setting to 0 (dangerous: risks metadata corruption\n"
	"if LVM commands get run concurrently).\n"
	"Type 2 uses the external shared library locking_library.\n"
	"Type 3 uses built-in clustered locking.\n"
	"Type 4 uses read-only locking which forbids any operations that might\n"
	"change metadata.\n"
	"Type 5 offers dummy locking for tools that do not need any locks.\n"
	"You should not need to set this directly: the tools will select when\n"
	"to use it instead of the configured locking_type.  Do not use lvmetad or\n"
	"the kernel device-mapper driver with this locking type.\n"
	"It is used by the --readonly option that offers read-only access to\n"
	"Volume Group metadata that cannot be locked safely because it belongs to\n"
	"an inaccessible domain and might be in use, for example a virtual machine\n"
	"image or a disk that is shared by a clustered machine.\n"
	"N.B. Don't use lvmetad with locking type 3 as lvmetad is not yet\n"
	"supported in clustered environment. If use_lvmetad=1 and locking_type=3\n"
	"is set at the same time, LVM always issues a warning message about this\n"
	"and then it automatically disables lvmetad use.\n")

cfg(global_wait_for_locks_CFG, "wait_for_locks", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_WAIT_FOR_LOCKS, vsn(2, 2, 50),
	"When disabled, fail if a lock request cannot be satisfied immediately.\n")

cfg(global_fallback_to_clustered_locking_CFG, "fallback_to_clustered_locking", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_CLUSTERED_LOCKING, vsn(2, 2, 42),
	"If using external locking (type 2) and initialisation fails,\n"
	"with this enabled, an attempt will be made to use the built-in\n"
	"clustered locking.\n"
	"If you are using a customised locking_library you should disable this.\n")

cfg(global_fallback_to_local_locking_CFG, "fallback_to_local_locking", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_FALLBACK_TO_LOCAL_LOCKING, vsn(2, 2, 42),
	"If an attempt to initialise type 2 or type 3 locking failed, perhaps\n"
	"because cluster components such as clvmd are not running, with this\n"
	"enabled, an attempt will be made to use local file-based locking (type 1).\n"
	"If this succeeds, only commands against local volume groups will proceed.\n"
	"Volume Groups marked as clustered will be ignored.\n")

cfg(global_locking_dir_CFG, "locking_dir", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_LOCK_DIR, vsn(1, 0, 0),
	"Local non-LV directory that holds file-based locks while commands are\n"
	"in progress.  A directory like /tmp that may get wiped on reboot is OK.\n")

cfg(global_prioritise_write_locks_CFG, "prioritise_write_locks", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_PRIORITISE_WRITE_LOCKS, vsn(2, 2, 52),
	"Whenever there are competing read-only and read-write access requests for\n"
	"a volume group's metadata, instead of always granting the read-only\n"
	"requests immediately, delay them to allow the read-write requests to be\n"
	"serviced.  Without this setting, write access may be stalled by a high\n"
	"volume of read-only requests.\n"
	"NB. This option only affects locking_type = 1 viz. local file-based locking.\n")

cfg(global_library_dir_CFG, "library_dir", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0),
	"Search this directory first for shared libraries.\n")

cfg(global_locking_library_CFG, "locking_library", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, DEFAULT_LOCKING_LIB, vsn(1, 0, 0),
	"The external locking library to load if locking_type is set to 2.\n")

cfg(global_abort_on_internal_errors_CFG, "abort_on_internal_errors", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ABORT_ON_INTERNAL_ERRORS, vsn(2, 2, 57),
	"Treat any internal errors as fatal errors, aborting the process that\n"
	"encountered the internal error. Please only enable for debugging.\n")

cfg(global_detect_internal_vg_cache_corruption_CFG, "detect_internal_vg_cache_corruption", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DETECT_INTERNAL_VG_CACHE_CORRUPTION, vsn(2, 2, 96),
	"Check whether CRC is matching when parsed VG is used multiple times.\n"
	"This is useful to catch unexpected internal cached volume group\n"
	"structure modification. Please only enable for debugging.\n")

cfg(global_metadata_read_only_CFG, "metadata_read_only", global_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_METADATA_READ_ONLY, vsn(2, 2, 75),
	"If enabled, no operations that change on-disk metadata will be permitted.\n"
	"Additionally, read-only commands that encounter metadata in need of repair\n"
	"will still be allowed to proceed exactly as if the repair had been\n"
	"performed (except for the unchanged vg_seqno).\n"
	"Inappropriate use could mess up your system, so seek advice first!\n")

cfg(global_mirror_segtype_default_CFG, "mirror_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_SEGTYPE, vsn(2, 2, 87),
	"Defines which segtype will be used when the shorthand -m option\n"
	"is used for mirroring.\n"
	"Possible options are: mirror, raid1.\n"
	"mirror - The original RAID1 implementation provided by LVM/DM.\n"
	"It is characterized by a flexible log solution (core, disk, mirrored)\n"
	"and by the necessity to block I/O while reconfiguring in the\n"
	"event of a failure.\n"
	"There is an inherent race in the dmeventd failure handling\n"
	"logic with snapshots of devices using this type of RAID1 that\n"
	"in the worst case could cause a deadlock.\n"
	"raid1 - This implementation leverages MD's RAID1 personality through\n"
	"device-mapper.  It is characterized by a lack of log options.\n"
	"(A log is always allocated for every device and they are placed\n"
	"on the same device as the image - no separate devices are\n"
	"required.)  This mirror implementation does not require I/O\n"
	"to be blocked in the kernel in the event of a failure.\n"
	"This mirror implementation is not cluster-aware and cannot be\n"
	"used in a shared (active/active) fashion in a cluster.\n"
	"Use '--type <mirror|raid1>' to override this default setting.\n")

cfg(global_raid10_segtype_default_CFG, "raid10_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_RAID10_SEGTYPE, vsn(2, 2, 99),
	"Determines the segment types used by default when\n"
	"the '--stripes/-i' and '--mirrors/-m' arguments are both specified\n"
	"during the creation of a logical volume.\n"
	"Possible options are: raid10, mirror.\n"
	"raid10 - This implementation leverages MD's RAID10 personality through\n"
	"device-mapper.\n"
	"mirror - LVM will layer the 'mirror' and 'stripe' segment types.  It\n"
	"will do this by creating a mirror on top of striped sub-LVs;\n"
	"effectively creating a RAID 0+1 array.  This is suboptimal\n"
	"in terms of providing redundancy and performance. Changing to\n"
	"this setting is not advised.\n"
	"Use '--type <raid10|mirror>' to override this default setting.\n")

cfg(global_sparse_segtype_default_CFG, "sparse_segtype_default", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_SPARSE_SEGTYPE, vsn(2, 2, 112),
	"Defines which segtype will be used when the shorthand '-V and -L' option\n"
	"is used for sparse volume creation.\n"
	"Possible options are: snapshot, thin.\n"
	"snapshot - The original snapshot implementation provided by LVM/DM.\n"
	"It is using old snashot that mixes data and metadata within\n"
	"a single COW storage volume and has poor performs when\n"
	"the size of stored data passes hundereds of MB.\n"
	"thin - Newer implementation leverages thin provisioning target.\n"
	"It has bigger minimal chunk size (64KiB) and uses separate volume\n"
	"for metadata. It has better performance especially in case of\n"
	"bigger data uses. This device type has also full snapshot support.\n"
	"Use '--type <snapshot|thin>' to override this default setting.\n")

cfg(global_lvdisplay_shows_full_device_path_CFG, "lvdisplay_shows_full_device_path", global_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_LVDISPLAY_SHOWS_FULL_DEVICE_PATH, vsn(2, 2, 89),
	"The default format for displaying LV names in lvdisplay was changed\n"
	"in version 2.02.89 to show the LV name and path separately.\n"
	"Previously this was always shown as /dev/vgname/lvname even when that\n"
	"was never a valid path in the /dev filesystem.\n"
	"Enable this option to reinstate the previous format.\n")

cfg(global_use_lvmetad_CFG, "use_lvmetad", global_CFG_SECTION, 0, CFG_TYPE_BOOL, 0, vsn(2, 2, 93),
	"Whether to use (trust) a running instance of lvmetad. If this is disabled,\n"
	"all commands fall back to the usual scanning mechanisms. When enabled,\n"
	"*and* when lvmetad is running (automatically instantiated by making use of\n"
	"systemd's socket-based service activation or run as an initscripts service\n"
	"or run manually), the volume group metadata and PV state flags are obtained\n"
	"from the lvmetad instance and no scanning is done by the individual\n"
	"commands. In a setup with lvmetad, lvmetad udev rules *must* be set up for\n"
	"LVM to work correctly. Without proper udev rules, all changes in block\n"
	"device configuration will be *ignored* until a manual 'pvscan --cache'\n"
	"is performed. These rules are installed by default.\n"
	"If lvmetad has been running while use_lvmetad was disabled, it MUST be\n"
	"stopped before enabled use_lvmetad and started again afterwards.\n"
	"If using lvmetad, volume activation is also switched to automatic\n"
	"event-based mode. In this mode, the volumes are activated based on\n"
	"incoming udev events that automatically inform lvmetad about new PVs that\n"
	"appear in the system. Once a VG is complete (all the PVs are present), it\n"
	"is auto-activated. The activation/auto_activation_volume_list setting\n"
	"controls which volumes are auto-activated (all by default).\n"
	"A note about device filtering while lvmetad is used:\n"
	"When lvmetad is updated (either automatically based on udev events or\n"
	"directly by a pvscan --cache <device> call), devices/filter is ignored and\n"
	"all devices are scanned by default -- lvmetad always keeps unfiltered\n"
	"information which is then provided to LVM commands and then each LVM\n"
	"command does the filtering based on devices/filter setting itself.  This\n"
	"does not apply to non-regexp filters though: component filters such as\n"
	"multipath and MD are checked at pvscan --cache time.\n"
	"In order to completely prevent LVM from scanning a device, even when using\n"
	"lvmetad, devices/global_filter must be used.\n"
	"N.B. Don't use lvmetad with locking type 3 as lvmetad is not yet\n"
	"supported in clustered environment. If use_lvmetad=1 and locking_type=3\n"
	"is set at the same time, LVM always issues a warning message about this\n"
	"and then it automatically disables use_lvmetad.\n")

cfg(global_thin_check_executable_CFG, "thin_check_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, THIN_CHECK_CMD, vsn(2, 2, 94),
	"Full path of the utility called to check that a thin metadata device\n"
	"is in a state that allows it to be used.\n"
	"Each time a thin pool needs to be activated or after it is deactivated\n"
	"this utility is executed. The activation will only proceed if the utility\n"
	"has an exit status of 0.\n"
	"Set to \"\" to skip this check.  (Not recommended.)\n"
	"The thin tools are available as part of the device-mapper-persistent-data\n"
	"package from https://github.com/jthornber/thin-provisioning-tools.\n")

cfg_array(global_thin_check_options_CFG, "thin_check_options", global_CFG_SECTION, 0, CFG_TYPE_STRING, "#S" DEFAULT_THIN_CHECK_OPTIONS, vsn(2, 2, 96),
	"Array of string options passed with thin_check command. By default,\n"
	"option -q is for quiet output.\n"
	"With thin_check version 2.1 or newer you can add --ignore-non-fatal-errors\n"
	"to let it pass through ignorable errors and fix them later.\n"
	"With thin_check version 3.2 or newer you should add --clear-needs-check-flag.\n")

cfg_array(global_thin_disabled_features_CFG, "thin_disabled_features", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, NULL, vsn(2, 2, 99),
	"The specified features are not used by thin driver.\n"
	"This can be helpful not just for testing, but i.e. allows to avoid\n"
	"using problematic implementation of some thin feature.\n"
	"Features: block_size, discards, discards_non_power_2, external_origin,\n"
	"metadata_resize, external_origin_extend, error_if_no_space.\n"
	"Example:\n"
	"thin_disabled_features = [ \"discards\", \"block_size\" ]\n")

cfg(global_thin_dump_executable_CFG, "thin_dump_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, THIN_DUMP_CMD, vsn(2, 2, 100),
	"Full path of the utility called to dump thin metadata content.\n"
	"See thin_check_executable how to obtain binaries.\n")

cfg(global_thin_repair_executable_CFG, "thin_repair_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, THIN_REPAIR_CMD, vsn(2, 2, 100),
	"Full path of the utility called to repair a thin metadata device\n"
	"is in a state that allows it to be used.\n"
	"Each time a thin pool needs repair this utility is executed.\n"
	"See thin_check_executable how to obtain binaries.\n")

cfg_array(global_thin_repair_options_CFG, "thin_repair_options", global_CFG_SECTION, 0, CFG_TYPE_STRING, "#S" DEFAULT_THIN_REPAIR_OPTIONS, vsn(2, 2, 100),
	"Array of extra string options passed with thin_repair command.\n")

cfg(global_cache_check_executable_CFG, "cache_check_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, CACHE_CHECK_CMD, vsn(2, 2, 108),
	"Full path of the utility called to check that a cache metadata device\n"
	"is in a state that allows it to be used.\n"
	"Each time a cached LV needs to be used or after it is deactivated\n"
	"this utility is executed. The activation will only proceed if the utility\n"
	"has an exit status of 0.\n"
	"Set to \"\" to skip this check.  (Not recommended.)\n"
	"The cache tools are available as part of the device-mapper-persistent-data\n"
	"package from https://github.com/jthornber/thin-provisioning-tools.\n")

cfg_array(global_cache_check_options_CFG, "cache_check_options", global_CFG_SECTION, 0, CFG_TYPE_STRING, "#S" DEFAULT_CACHE_CHECK_OPTIONS, vsn(2, 2, 108),
	"Array of string options passed with cache_check command. By default,\n"
	"option -q is for quiet output.\n")

cfg(global_cache_dump_executable_CFG, "cache_dump_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, CACHE_DUMP_CMD, vsn(2, 2, 108),
	"Full path of the utility called to dump cache metadata content.\n"
	"See cache_check_executable how to obtain binaries.\n")

cfg(global_cache_repair_executable_CFG, "cache_repair_executable", global_CFG_SECTION, CFG_ALLOW_EMPTY, CFG_TYPE_STRING, CACHE_REPAIR_CMD, vsn(2, 2, 108),
	"Full path of the utility called to repair a cache metadata device.\n"
	"Each time a cache metadata needs repair this utility is executed.\n"
	"See cache_check_executable how to obtain binaries.\n")

cfg_array(global_cache_repair_options_CFG, "cache_repair_options", global_CFG_SECTION, 0, CFG_TYPE_STRING, "#S" DEFAULT_CACHE_REPAIR_OPTIONS, vsn(2, 2, 108),
	"Array of extra string options passed with cache_repair command.\n")

cfg(global_system_id_source_CFG, "system_id_source", global_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_SYSTEM_ID_SOURCE, vsn(2, 2, 117),
	"The method lvm will use to set the system ID of the local host.\n"
	"Volume Groups can also be given a system ID.  A VG on shared storage\n"
	"devices will be accessible only to the host with a matching system ID.\n"
	"See 'man lvmsystemid' for information on limitations and correct usage.\n"
	"Possible options are: none, lvmlocal, uname, machineid, file.\n"
	"none - The host has no system ID.\n"
	"lvmlocal - Obtain the system ID from the system_id setting in the\n"
	"'local' section of an lvm configuration file, e.g. lvmlocal.conf.\n"
	"uname - Set the system ID from the hostname (uname) of the system.\n"
	"System IDs beginning localhost are not permitted.\n"
	"machineid - Use the contents of the file /etc/machine-id to set the\n"
	"system ID.  Some systems create this file at installation time.\n"
	"See 'man machine-id'.\n"
	"file - Use the contents of an alternative file (system_id_file) to\n"
	"set the system ID.\n")

cfg(global_system_id_file_CFG, "system_id_file", global_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 117),
	"Full path of a file containing a system ID.\n"
	"This is used when system_id_source is set to file.\n"
	"Comments starting with the character # are ignored.\n")

cfg(activation_checks_CFG, "checks", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ACTIVATION_CHECKS, vsn(2, 2, 86),
	"Perform internal checks on the operations issued to\n"
	"libdevmapper.  Useful for debugging problems with activation.\n"
	"Some of the checks may be expensive, so it's best to use this\n"
	"only when there seems to be a problem.\n")

cfg(activation_udev_sync_CFG, "udev_sync", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_UDEV_SYNC, vsn(2, 2, 51),
	"Enable/disable udev synchronisation (if compiled into the binaries).\n"
	"Processes will not wait for notification from udev.\n"
	"They will continue irrespective of any possible udev processing\n"
	"in the background.  You should only use this if udev is not running\n"
	"or has rules that ignore the devices LVM creates.\n"
	"The command line argument --nodevsync takes precedence over this setting.\n"
	"If enabled when udev is not running, and there are LVM processes\n"
	"waiting for udev, run 'dmsetup udevcomplete_all' manually to wake them up.\n")

cfg(activation_udev_rules_CFG, "udev_rules", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_UDEV_RULES, vsn(2, 2, 57),
	"Enable/disable the udev rules installed by LVM (if built with\n"
	"--enable-udev_rules). LVM will then manage the /dev nodes and symlinks\n"
	"for active logical volumes directly itself.\n"
	"N.B. Manual intervention may be required if this setting is changed\n"
	"while any logical volumes are active.\n")

cfg(activation_verify_udev_operations_CFG, "verify_udev_operations", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_VERIFY_UDEV_OPERATIONS, vsn(2, 2, 86),
	"Verify operations performed by udev.  This turns on additional checks\n"
	"(and if necessary, repairs) on entries in the device directory after udev\n"
	"has completed processing its events.\n"
	"Useful for diagnosing problems with LVM/udev interactions.\n")

cfg(activation_retry_deactivation_CFG, "retry_deactivation", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_RETRY_DEACTIVATION, vsn(2, 2, 89),
	"Retry deactivation operations for a few seconds before failing\n"
	"if deactivation of an LV fails, perhaps because a process run\n"
	"from a quick udev rule temporarily opened the device.\n")

cfg(activation_missing_stripe_filler_CFG, "missing_stripe_filler", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_STRIPE_FILLER, vsn(1, 0, 0),
	"How to fill in missing stripes if activating an incomplete volume.\n"
	"Using 'error' will make inaccessible parts of the device return\n"
	"I/O errors on access.  You can instead use a device path, in which\n"
	"case, that device will be used to in place of missing stripes.\n"
	"But note that using anything other than 'error' with mirrored\n"
	"or snapshotted volumes is likely to result in data corruption.\n")

cfg(activation_use_linear_target_CFG, "use_linear_target", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_LINEAR_TARGET, vsn(2, 2, 89),
	"Enable/disable the linear target optimization.\n"
	"When disabled, the striped target is used. The linear target is\n"
	"an optimised version of the striped target that only handles a\n"
	"single stripe.\n")

cfg(activation_reserved_stack_CFG, "reserved_stack", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RESERVED_STACK, vsn(1, 0, 0),
	"How much stack (in KB) to reserve for use while devices suspended.\n"
	"Prior to version 2.02.89 this used to be set to 256KB.\n")

cfg(activation_reserved_memory_CFG, "reserved_memory", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RESERVED_MEMORY, vsn(1, 0, 0),
	"How much memory (in KB) to reserve for use while devices suspended.\n")

cfg(activation_process_priority_CFG, "process_priority", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_PROCESS_PRIORITY, vsn(1, 0, 0),
	"Nice value used while devices suspended.\n")

cfg_array(activation_volume_list_CFG, "volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY|CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 18),
	"If volume_list is defined, each LV is only activated if there is a\n"
	"match against the list.\n"
	"vgname and vgname/lvname are matched exactly.\n"
	"@tag matches any tag set in the LV or VG.\n"
	"@* matches if any tag defined on the host is also set in the LV or VG.\n"
	"If any host tags exist but volume_list is not defined, a default\n"
	"single-entry list containing '@*' is assumed.\n"
	"Example:\n"
	"volume_list = [ \"vg1\", \"vg2/lvol1\", \"@tag1\", \"@*\" ]\n")

cfg_array(activation_auto_activation_volume_list_CFG, "auto_activation_volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 97),
	"If auto_activation_volume_list is defined, each LV that is to be\n"
	"activated with the autoactivation option (--activate ay/-a ay) is\n"
	"first checked against the list. There are two scenarios in which\n"
	"the autoactivation option is used:\n"
	"1. automatic activation of volumes based on incoming PVs. If all the\n"
	"PVs making up a VG are present in the system, the autoactivation\n"
	"is triggered. This requires lvmetad (global/use_lvmetad=1) and udev\n"
	"to be running. In this case, 'pvscan --cache -aay' is called\n"
	"automatically without any user intervention while processing\n"
	"udev events. Please, make sure you define auto_activation_volume_list\n"
	"properly so only the volumes you want and expect are autoactivated.\n"
	"2. direct activation on command line with the autoactivation option.\n"
	"In this case, the user calls 'vgchange --activate ay/-a ay' or\n"
	"'lvchange --activate ay/-a ay' directly.\n"
	"By default, the auto_activation_volume_list is not defined and all\n"
	"volumes will be activated either automatically or by using --activate ay/-a ay.\n"
	"N.B. The activation/volume_list is still honoured in all cases so even\n"
	"if the VG/LV passes the auto_activation_volume_list, it still needs to\n"
	"pass the volume_list for it to be activated in the end.\n"
	"If auto_activation_volume_list is defined but empty, no volumes will be\n"
	"activated automatically and --activate ay/-a ay will do nothing.\n"
	"Example:\n"
	"auto_activation_volume_list = []\n"
	"If auto_activation_volume_list is defined and it's not empty, only matching\n"
	"volumes will be activated either automatically or by using --activate ay/-a ay.\n"
	"vgname and vgname/lvname are matched exactly.\n"
	"@tag matches any tag set in the LV or VG.\n"
	"@* matches if any tag defined on the host is also set in the LV or VG.\n"
	"Example:\n"
	"auto_activation_volume_list = [ \"vg1\", \"vg2/lvol1\", \"@tag1\", \"@*\" ]\n")

cfg_array(activation_read_only_volume_list_CFG, "read_only_volume_list", activation_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 89),
	"If read_only_volume_list is defined, each LV that is to be activated\n"
	"is checked against the list, and if it matches, it is activated\n"
	"in read-only mode.  (This overrides '--permission rw' stored in the\n"
	"metadata.)\n"
	"vgname and vgname/lvname are matched exactly.\n"
	"@tag matches any tag set in the LV or VG.\n"
	"@* matches if any tag defined on the host is also set in the LV or VG.\n"
	"Example:\n"
	"read_only_volume_list = [ \"vg1\", \"vg2/lvol1\", \"@tag1\", \"@*\" ]\n")

cfg(activation_mirror_region_size_CFG, "mirror_region_size", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RAID_REGION_SIZE, vsn(1, 0, 0), NULL)

cfg(activation_raid_region_size_CFG, "raid_region_size", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_RAID_REGION_SIZE, vsn(2, 2, 99),
	"For RAID or 'mirror' segment types, raid_region_size is the\n"
	"size (in KiB) of:\n"
	"each synchronization operation when initializing, and\n"
	"each copy operation when performing a pvmove (using 'mirror' segtype).\n"
	"This setting has replaced mirror_region_size since version 2.02.99.\n")

cfg(activation_error_when_full_CFG, "error_when_full", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_ERROR_WHEN_FULL, vsn(2, 2, 115),
	"Control error behavior when provisioned device becomes full.  This\n"
	"determines the default --errorwhenfull setting of new thin pools.\n"
	"The command line option --errorwhenfull takes precedence over this\n"
	"setting.  error_when_full disabled (0) means --errorwhenfull n.\n")

cfg(activation_readahead_CFG, "readahead", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_READ_AHEAD, vsn(1, 0, 23),
	"Setting to use when there is no readahead value stored in the metadata.\n"
	"Possible options are: none, auto.\n"
	"none - Disable readahead.\n"
	"auto - Use default value chosen by kernel.\n")

cfg(activation_raid_fault_policy_CFG, "raid_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_RAID_FAULT_POLICY, vsn(2, 2, 89),
	"Defines how a device failure in a RAID logical volume is handled.\n"
	"This includes logical volumes that have the following segment types:\n"
	"raid1, raid4, raid5*, and raid6*.\n"
	"In the event of a failure, the following policies will determine what\n"
	"actions are performed during the automated response to failures (when\n"
	"dmeventd is monitoring the RAID logical volume) and when 'lvconvert' is\n"
	"called manually with the options '--repair' and '--use-policies'.\n"
	"Possible options are: warn, allocate.\n"
	"warn - Use the system log to warn the user that a device in the RAID\n"
	"logical volume has failed.  It is left to the user to run\n"
	"'lvconvert --repair' manually to remove or replace the failed\n"
	"device.  As long as the number of failed devices does not\n"
	"exceed the redundancy of the logical volume (1 device for\n"
	"raid4/5, 2 for raid6, etc) the logical volume will remain usable.\n"
	"allocate - Attempt to use any extra physical volumes in the volume\n"
	"group as spares and replace faulty devices.\n")

cfg_runtime(activation_mirror_image_fault_policy_CFG, "mirror_image_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, vsn(2, 2, 57),
	"Defines how a device failure affecting a mirror (of 'mirror' segment type) is\n"
	"handled.  A mirror is composed of mirror images (copies) and a log.\n"
	"A disk log ensures that a mirror does not need to be re-synced\n"
	"(all copies made the same) every time a machine reboots or crashes.\n"
	"In the event of a failure, the specified policy will be used to determine\n"
	"what happens. This applies to automatic repairs (when the mirror is being\n"
	"monitored by dmeventd) and to manual lvconvert --repair when\n"
	"--use-policies is given.\n"
	"Possible options are: remove, allocate, allocate_anywhere.\n"
	"remove - Simply remove the faulty device and run without it.\n"
	"If the log device fails, the mirror would convert to using\n"
	"an in-memory log.  This means the mirror will not\n"
	"remember its sync status across crashes/reboots and\n"
	"the entire mirror will be re-synced.\n"
	"If a mirror image fails, the mirror will convert to a\n"
	"non-mirrored device if there is only one remaining good copy.\n"
	"allocate - Remove the faulty device and try to allocate space on\n"
	"a new device to be a replacement for the failed device.\n"
	"Using this policy for the log is fast and maintains the\n"
	"ability to remember sync state through crashes/reboots.\n"
	"Using this policy for a mirror device is slow, as it\n"
	"requires the mirror to resynchronize the devices, but it\n"
	"will preserve the mirror characteristic of the device.\n"
	"This policy acts like 'remove' if no suitable device and\n"
	"space can be allocated for the replacement.\n"
	"allocate_anywhere - Not yet implemented. Useful to place the log device\n"
	"temporarily on same physical volume as one of the mirror\n"
	"images. This policy is not recommended for mirror devices\n"
	"since it would break the redundant nature of the mirror. This\n"
	"policy acts like 'remove' if no suitable device and space can\n"
	"be allocated for the replacement.\n")

cfg(activation_mirror_log_fault_policy_CFG, "mirror_log_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_LOG_FAULT_POLICY, vsn(1, 2, 18),
	"The description of mirror_image_fault_policy also applies to this setting.\n")

cfg(activation_mirror_device_fault_policy_CFG, "mirror_device_fault_policy", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_MIRROR_DEVICE_FAULT_POLICY, vsn(1, 2, 10),
	"This setting has been replaced by the mirror_image_fault_policy setting.\n")

cfg(activation_snapshot_autoextend_threshold_CFG, "snapshot_autoextend_threshold", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_SNAPSHOT_AUTOEXTEND_THRESHOLD, vsn(2, 2, 75),
	"Defines when a snapshot should be automatically extended.\n"
	"When its space usage exceeds the percent set here, it is\n"
	"extended.  Setting this to 100 disables automatic extension.\n"
	"The minimum value is 50 (a setting below 50 will be treated as 50.)\n")

cfg(activation_snapshot_autoextend_percent_CFG, "snapshot_autoextend_percent", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_SNAPSHOT_AUTOEXTEND_PERCENT, vsn(2, 2, 75),
	"Defines how much extra space should be allocated for a snapshot\n"
	"when it is automatically extended, as a percent of its current size.\n"
	"Example:\n"
	"With snapshot_autoextend_threshold 70 and snapshot_autoextend_percent 20,\n"
	"whenever a snapshot exceeds 70% usage, it will be extended by another 20%.\n"
	"For a 1G snapshot, using up 700M will trigger a resize to 1.2G.\n"
	"When the usage exceeds 840M, the snapshot will be extended to 1.44G,\n"
	"and so on.\n")

cfg(activation_thin_pool_autoextend_threshold_CFG, "thin_pool_autoextend_threshold", activation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA, CFG_TYPE_INT, DEFAULT_THIN_POOL_AUTOEXTEND_THRESHOLD, vsn(2, 2, 89),
	"Defines when a thin pool should be automatically extended.\n"
	"When its space usage exceeds the percent set here, it is\n"
	"extended.  Setting this to 100 disables automatic extension.\n"
	"The minimum value is 50 (a setting below 50 will be treated as 50.)\n")

cfg(activation_thin_pool_autoextend_percent_CFG, "thin_pool_autoextend_percent", activation_CFG_SECTION, CFG_PROFILABLE | CFG_PROFILABLE_METADATA, CFG_TYPE_INT, DEFAULT_THIN_POOL_AUTOEXTEND_PERCENT, vsn(2, 2, 89),
	"Defines how much extra space should be allocated for a thin pool\n"
	"when it is automatically extended, as a percent of its current size.\n"
	"Example:\n"
	"With thin_pool_autoextend_threshold 70 and thin_pool_autoextend_percent 20,\n"
	"whenever a thin pool exceeds 70% usage, it will be extended by another 20%.\n"
	"For a 1G thin pool, using up 700M will trigger a resize to 1.2G.\n"
	"When the usage exceeds 840M, the thin pool will be extended to 1.44G,\n"
	"and so on.\n")

cfg_array(activation_mlock_filter_CFG, "mlock_filter", activation_CFG_SECTION, CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 62),
	"While activating devices, I/O to devices being (re)configured is\n"
	"suspended, and as a precaution against deadlocks, LVM needs to pin\n"
	"any memory it is using so it is not paged out.  Groups of pages that\n"
	"are known not to be accessed during activation need not be pinned\n"
	"into memory.  Each string listed in this setting is compared against\n"
	"each line in /proc/self/maps, and the pages corresponding to any\n"
	"lines that match are not pinned.  On some systems locale-archive was\n"
	"found to make up over 80% of the memory used by the process.\n"
	"Example:\n"
	"mlock_filter = [ \"locale/locale-archive\", \"gconv/gconv-modules.cache\" ]\n")

cfg(activation_use_mlockall_CFG, "use_mlockall", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_USE_MLOCKALL, vsn(2, 2, 62),
	"Enable to revert to the default behaviour prior to version 2.02.62 which\n"
	"used mlockall() to pin the whole process's memory while activating devices.\n")

cfg(activation_monitoring_CFG, "monitoring", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_DMEVENTD_MONITOR, vsn(2, 2, 63),
	"Enable/disable monitoring when activating logical volumes.\n"
	"Disabling can also be done with the --ignoremonitoring option.\n")

cfg(activation_polling_interval_CFG, "polling_interval", activation_CFG_SECTION, 0, CFG_TYPE_INT, DEFAULT_INTERVAL, vsn(2, 2, 63),
	"When pvmove or lvconvert must wait for the kernel to finish\n"
	"synchronising or merging data, they check and report progress\n"
	"at intervals of this number of seconds.\n"
	"If this is set to 0 and there is only one thing to wait for, there\n"
	"are no progress reports, but the process is awoken immediately the\n"
	"operation is complete.\n")

cfg(activation_auto_set_activation_skip_CFG, "auto_set_activation_skip", activation_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_AUTO_SET_ACTIVATION_SKIP, vsn(2,2,99),
	"Each LV can have an 'activation skip' flag stored persistently against it.\n"
	"During activation, this flag is used to decide whether such an LV is skipped.\n"
	"The 'activation skip' flag can be set during LV creation and by default it\n"
	"is automatically set for thin snapshot LVs. The auto_set_activation_skip\n"
	"enables or disables this automatic setting of the flag while LVs are created.\n")

cfg(activation_mode_CFG, "activation_mode", activation_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_ACTIVATION_MODE, vsn(2,2,108),
	"Determines how Logical Volumes are activated if any devices are missing.\n"
	"Possible options are: complete, degraded, partial.\n"
	"complete - Only allow activation of an LV if all of the Physical Volumes\n"
	"it uses are present.  Other PVs in the Volume Group may be missing.\n"
	"degraded - Like complete, but additionally RAID Logical Volumes of\n"
	"segment type raid1, raid4, raid5, radid6 and raid10 will\n"
	"be activated if there is no data loss, i.e. they have\n"
	"sufficient redundancy to present the entire addressable\n"
	"range of the Logical Volume.\n"
	"partial - Allows the activation of any Logical Volume even if\n"
	"a missing or failed PV could cause data loss with a\n"
	"portion of the Logical Volume inaccessible.\n"
	"This setting should not normally be used, but may\n"
	"sometimes assist with data recovery.\n"
	"This setting corresponds with the --activationmode option for\n"
	"lvchange and vgchange.\n")

cfg(metadata_pvmetadatacopies_CFG, "pvmetadatacopies", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_PVMETADATACOPIES, vsn(1, 0, 0),
	"Default number of copies of metadata to hold on each PV.  0, 1 or 2.\n"
	"You might want to override it from the command line with 0\n"
	"when running pvcreate on new PVs which are to be added to large VGs.\n")

cfg(metadata_vgmetadatacopies_CFG, "vgmetadatacopies", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_VGMETADATACOPIES, vsn(2, 2, 69),
	"Default number of copies of metadata to maintain for each VG.\n"
	"If set to a non-zero value, LVM automatically chooses which of\n"
	"the available metadata areas to use to achieve the requested\n"
	"number of copies of the VG metadata.  If you set a value larger\n"
	"than the the total number of metadata areas available then\n"
	"metadata is stored in them all.\n"
	"The default value of 0 (unmanaged) disables this automatic\n"
	"management and allows you to control which metadata areas\n"
	"are used at the individual PV level using 'pvchange metadataignore y/n'.\n")

cfg(metadata_pvmetadatasize_CFG, "pvmetadatasize", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_PVMETADATASIZE, vsn(1, 0, 0),
	"Approximate default size of on-disk metadata areas in sectors.\n"
	"You should increase this if you have large volume groups or\n"
	"you want to retain a large on-disk history of your metadata changes.\n")

cfg(metadata_pvmetadataignore_CFG, "pvmetadataignore", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_BOOL, DEFAULT_PVMETADATAIGNORE, vsn(2, 2, 69), NULL)
cfg(metadata_stripesize_CFG, "stripesize", metadata_CFG_SECTION, CFG_ADVANCED, CFG_TYPE_INT, DEFAULT_STRIPESIZE, vsn(1, 0, 0), NULL)

cfg_array(metadata_dirs_CFG, "dirs", metadata_CFG_SECTION, CFG_ADVANCED | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0),
	"List of directories holding live copies of text format metadata.\n"
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

cfg_section(metadata_disk_areas_CFG_SUBSECTION, "disk_areas", metadata_CFG_SECTION, CFG_ADVANCED | CFG_UNSUPPORTED | CFG_DEFAULT_UNDEFINED, vsn(1, 0, 0), NULL)
cfg_section(disk_area_CFG_SUBSECTION, "disk_area", metadata_disk_areas_CFG_SUBSECTION, CFG_NAME_VARIABLE | CFG_ADVANCED | CFG_UNSUPPORTED | CFG_DEFAULT_UNDEFINED, vsn(1, 0, 0), NULL)
cfg(disk_area_start_sector_CFG, "start_sector", disk_area_CFG_SUBSECTION, CFG_ADVANCED | CFG_UNSUPPORTED | CFG_DEFAULT_UNDEFINED, CFG_TYPE_INT, 0, vsn(1, 0, 0), NULL)
cfg(disk_area_size_CFG, "size", disk_area_CFG_SUBSECTION, CFG_ADVANCED | CFG_UNSUPPORTED | CFG_DEFAULT_UNDEFINED, CFG_TYPE_INT, 0, vsn(1, 0, 0), NULL)
cfg(disk_area_id_CFG, "id", disk_area_CFG_SUBSECTION, CFG_ADVANCED | CFG_UNSUPPORTED | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 0), NULL)

cfg(report_compact_output_CFG, "compact_output", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_REP_COMPACT_OUTPUT, vsn(2, 2, 115),
	"If enabled, fields which don't have value set for any of the rows\n"
	"reported are skipped on output. Compact output is applicable only\n"
	"if report is buffered (report/buffered=1).\n")

cfg(report_aligned_CFG, "aligned", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_REP_ALIGNED, vsn(1, 0, 0),
	"Align columns on report output.\n")

cfg(report_buffered_CFG, "buffered", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_REP_BUFFERED, vsn(1, 0, 0),
	"When buffered reporting is used, the report's content is appended\n"
	"incrementally to include each object being reported until the report\n"
	"is flushed to output which normally happens at the end of command\n"
	"execution. Otherwise, if buffering is not used, each object is\n"
	"reported as soon as its processing is finished.\n")

cfg(report_headings_CFG, "headings", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_REP_HEADINGS, vsn(1, 0, 0),
	"Show headings for columns on report.\n")

cfg(report_separator_CFG, "separator", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_REP_SEPARATOR, vsn(1, 0, 0),
	"A separator to use on report after each field.\n")

cfg(report_list_item_separator_CFG, "list_item_separator", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_REP_LIST_ITEM_SEPARATOR, vsn(2, 2, 108),
	"A separator to use for list items when reported.\n")

cfg(report_prefixes_CFG, "prefixes", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_REP_PREFIXES, vsn(2, 2, 36),
	"Use a field name prefix for each field reported.\n")

cfg(report_quoted_CFG, "quoted", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_REP_QUOTED, vsn(2, 2, 39),
	"Quote field values when using field name prefixes.\n")

cfg(report_colums_as_rows_CFG, "colums_as_rows", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, DEFAULT_REP_COLUMNS_AS_ROWS, vsn(1, 0, 0),
	"Output each column as a row. If set, this also implies report/prefixes=1.\n")

cfg(report_binary_values_as_numeric_CFG, "binary_values_as_numeric", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_BOOL, 0, vsn(2, 2, 108),
	"Use binary values 0 or 1 instead of descriptive literal values for\n"
	"columns that have exactly two valid values to report (not counting the\n"
	"'unknown' value which denotes that the value could not be determined).\n")

cfg(report_devtypes_sort_CFG, "devtypes_sort", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_DEVTYPES_SORT, vsn(2, 2, 101),
	"Comma separated list of columns to sort by when reporting 'lvm devtypes' command.\n"
	"See 'lvm devtypes -o help' for the list of possible fields.\n")

cfg(report_devtypes_cols_CFG, "devtypes_cols", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_DEVTYPES_COLS, vsn(2, 2, 101),
	"Comma separated list of columns to report for 'lvm devtypes' command.\n"
	"See 'lvm devtypes -o help' for the list of possible fields.\n")

cfg(report_devtypes_cols_verbose_CFG, "devtypes_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_DEVTYPES_COLS_VERB, vsn(2, 2, 101),
	"Comma separated list of columns to report for 'lvm devtypes' command in verbose mode.\n"
	"See 'lvm devtypes -o help' for the list of possible fields.\n")

cfg(report_lvs_sort_CFG, "lvs_sort", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_LVS_SORT, vsn(1, 0, 0),
	"Comma separated list of columns to sort by when reporting 'lvs' command.\n"
	"See 'lvs -o help' for the list of possible fields.\n")

cfg(report_lvs_cols_CFG, "lvs_cols", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_LVS_COLS, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'lvs' command.\n"
	"See 'lvs -o help' for the list of possible fields.\n")

cfg(report_lvs_cols_verbose_CFG, "lvs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_LVS_COLS_VERB, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'lvs' command in verbose mode.\n"
	"See 'lvs -o help' for the list of possible fields.\n")

cfg(report_vgs_sort_CFG, "vgs_sort", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_VGS_SORT, vsn(1, 0, 0),
	"Comma separated list of columns to sort by when reporting 'vgs' command.\n"
	"See 'vgs -o help' for the list of possible fields.\n")

cfg(report_vgs_cols_CFG, "vgs_cols", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_VGS_COLS, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'vgs' command.\n"
	"See 'vgs -o help' for the list of possible fields.\n")

cfg(report_vgs_cols_verbose_CFG, "vgs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_VGS_COLS_VERB, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'vgs' command in verbose mode.\n"
	"See 'vgs -o help' for the list of possible fields.\n")

cfg(report_pvs_sort_CFG, "pvs_sort", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_PVS_SORT, vsn(1, 0, 0),
	"Comma separated list of columns to sort by when reporting 'pvs' command.\n"
	"See 'pvs -o help' for the list of possible fields.\n")

cfg(report_pvs_cols_CFG, "pvs_cols", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_PVS_COLS, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'pvs' command.\n"
	"See 'pvs -o help' for the list of possible fields.\n")

cfg(report_pvs_cols_verbose_CFG, "pvs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_PVS_COLS_VERB, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'pvs' command in verbose mode.\n"
	"See 'pvs -o help' for the list of possible fields.\n")

cfg(report_segs_sort_CFG, "segs_sort", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_SEGS_SORT, vsn(1, 0, 0),
	"Comma separated list of columns to sort by when reporting 'lvs --segments' command.\n"
	"See 'lvs --segments -o help' for the list of possible fields.\n")

cfg(report_segs_cols_CFG, "segs_cols", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_SEGS_COLS, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'lvs --segments' command.\n"
	"See 'lvs --segments  -o help' for the list of possible fields.\n")

cfg(report_segs_cols_verbose_CFG, "segs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_SEGS_COLS_VERB, vsn(1, 0, 0),
	"Comma separated list of columns to report for 'lvs --segments' command in verbose mode.\n"
	"See 'lvs --segments -o help' for the list of possible fields.\n")

cfg(report_pvsegs_sort_CFG, "pvsegs_sort", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_PVSEGS_SORT, vsn(1, 1, 3),
	"Comma separated list of columns to sort by when reporting 'pvs --segments' command.\n"
	"See 'pvs --segments -o help' for the list of possible fields.\n")

cfg(report_pvsegs_cols_CFG, "pvsegs_cols", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_PVSEGS_COLS, vsn(1, 1, 3),
	"Comma separated list of columns to sort by when reporting 'pvs --segments' command.\n"
	"See 'pvs --segments -o help' for the list of possible fields.\n")

cfg(report_pvsegs_cols_verbose_CFG, "pvsegs_cols_verbose", report_CFG_SECTION, CFG_PROFILABLE, CFG_TYPE_STRING, DEFAULT_PVSEGS_COLS_VERB, vsn(1, 1, 3),
	"Comma separated list of columns to sort by when reporting 'pvs --segments' command in verbose mode.\n"
	"See 'pvs --segments -o help' for the list of possible fields.\n")

cfg(dmeventd_mirror_library_CFG, "mirror_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_MIRROR_LIB, vsn(1, 2, 3),
	"The library used when monitoring a mirror device.\n"
	"libdevmapper-event-lvm2mirror.so attempts to recover from\n"
	"failures.  It removes failed devices from a volume group and\n"
	"reconfigures a mirror as necessary. If no mirror library is\n"
	"provided, mirrors are not monitored through dmeventd.\n")

cfg(dmeventd_raid_library_CFG, "raid_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_RAID_LIB, vsn(2, 2, 87), NULL)

cfg(dmeventd_snapshot_library_CFG, "snapshot_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_SNAPSHOT_LIB, vsn(1, 2, 26),
	"The library used when monitoring a snapshot device.\n"
	"libdevmapper-event-lvm2snapshot.so monitors the filling of\n"
	"snapshots and emits a warning through syslog when the use of\n"
	"the snapshot exceeds 80%. The warning is repeated when 85%, 90% and\n"
	"95% of the snapshot is filled.\n")

cfg(dmeventd_thin_library_CFG, "thin_library", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_THIN_LIB, vsn(2, 2, 89),
	"The library used when monitoring a thin device.\n"
	"libdevmapper-event-lvm2thin.so monitors the filling of\n"
	"pool and emits a warning through syslog when the use of\n"
	"the pool exceeds 80%. The warning is repeated when 85%, 90% and\n"
	"95% of the pool is filled.\n")

cfg(dmeventd_executable_CFG, "executable", dmeventd_CFG_SECTION, 0, CFG_TYPE_STRING, DEFAULT_DMEVENTD_PATH, vsn(2, 2, 73),
	"Full path of the dmeventd binary.\n")

cfg(tags_hosttags_CFG, "hosttags", tags_CFG_SECTION, 0, CFG_TYPE_BOOL, DEFAULT_HOSTTAGS, vsn(1, 0, 18), NULL)

cfg_section(tag_CFG_SUBSECTION, "tag", tags_CFG_SECTION, CFG_NAME_VARIABLE | CFG_DEFAULT_UNDEFINED, vsn(1, 0, 18), NULL)
cfg(tag_host_list_CFG, "host_list", tag_CFG_SUBSECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(1, 0, 18), NULL)

cfg(local_system_id_CFG, "system_id", local_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 117),
	"Defines the system ID of the local host.  This is used\n"
	"when global/system_id_source is set to 'lvmlocal' in the main\n"
	"configuration file, e.g. lvm.conf.\n"
	"When used, it must be set to a unique value - often a hostname -\n"
	"across all the hosts sharing access to the storage.\n"
	"Example:\n"
	"Set no system ID.\n"
	"system_id = \"\"\n"
	"Example:\n"
	"Set the system_id to the string 'host1'.\n"
	"system_id = \"host1\"\n")

cfg_array(local_extra_system_ids_CFG, "extra_system_ids", local_CFG_SECTION, CFG_ALLOW_EMPTY | CFG_DEFAULT_UNDEFINED, CFG_TYPE_STRING, NULL, vsn(2, 2, 117),
	"Defines a list of extra system_ids other than the local\n"
	"system_id that the local host is allowed to access.  These are\n"
	"used for all values of global/system_id_source except 'none'.\n"
	"Only use this if you have read 'man lvmsystemid' and you are sure\n"
	"you understand why you need to use it!\n")

cfg(CFG_COUNT, NULL, root_CFG_SECTION, 0, CFG_TYPE_INT, 0, vsn(0, 0, 0), NULL)
