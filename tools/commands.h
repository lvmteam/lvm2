/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
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

xx(config,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(devtypes,
   "Display recognised built-in block device types",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(dumpconfig,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(formats,
   "List available metadata formats",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(fullreport,
   "Display full report",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(help,
   "Display help for commands",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(lastlog,
   "Display last command's log report",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(lvchange,
   "Change the attributes of logical volume(s)",
   PERMITTED_READ_ONLY | ALLOW_HINTS)

xx(lvconvert,
   "Change logical volume layout",
   GET_VGNAME_FROM_OPTIONS)

xx(lvcreate,
   "Create a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS)

xx(lvdisplay,
   "Display information about a logical volume",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(lvextend,
   "Add space to a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS)

xx(lvmchange,
   "With the device mapper, this is obsolete and does nothing.",
   0)

xx(lvmconfig,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(lvmdevices,
   "Manage the devices file",
   DEVICE_ID_NOT_FOUND)

xx(lvmdiskscan,
   "List devices that may be used as physical volumes",
   PERMITTED_READ_ONLY | ENABLE_ALL_DEVS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(lvmsadc,
   "Collect activity data",
   0)

xx(lvmsar,
   "Create activity report",
   0)

xx(lvpoll,
   "Continue already initiated poll operation on a logical volume",
   0)

xx(lvreduce,
   "Reduce the size of a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS)

xx(lvremove,
   "Remove logical volume(s) from the system",
   ALL_VGS_IS_DEFAULT | ALLOW_HINTS) /* all VGs only with --select */

xx(lvrename,
   "Rename a logical volume",
   ALLOW_HINTS)

xx(lvresize,
   "Resize a logical volume",
   ALLOW_HINTS | ALTERNATIVE_EXTENTS)

xx(lvs,
   "Display information about logical volumes",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(lvscan,
   "List all logical volumes in all volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(pvchange,
   "Change attributes of physical volume(s)",
   0)

xx(pvck,
   "Check metadata on physical volumes",
   LOCKD_VG_SH | ALLOW_EXPORTED)

xx(pvcreate,
   "Initialize physical volume(s) for use by LVM",
   ENABLE_ALL_DEVS)

xx(pvdata,
   "Display the on-disk metadata for physical volume(s)",
   0)

xx(pvdisplay,
   "Display various attributes of physical volume(s)",
   PERMITTED_READ_ONLY | ENABLE_ALL_DEVS | ENABLE_DUPLICATE_DEVS | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

/* ALL_VGS_IS_DEFAULT is for polldaemon to find pvmoves in-progress using process_each_vg. */

xx(pvmove,
   "Move extents from one physical volume to another",
   ALL_VGS_IS_DEFAULT | DISALLOW_TAG_ARGS)

xx(pvremove,
   "Remove LVM label(s) from physical volume(s)",
   ENABLE_ALL_DEVS)

xx(pvresize,
   "Resize physical volume(s)",
   0)

xx(pvs,
   "Display information about physical volumes",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_ALL_DEVS | ENABLE_DUPLICATE_DEVS | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(pvscan,
   "List all physical volumes",
   PERMITTED_READ_ONLY | LOCKD_VG_SH | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(segtypes,
   "List available segment types",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(systemid,
   "Display the system ID, if any, currently set on this host",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(tags,
   "List tags defined on this host",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(version,
   "Display software and driver version information",
   PERMITTED_READ_ONLY | NO_METADATA_PROCESSING)

xx(vgcfgbackup,
   "Backup volume group configuration(s)",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | ALLOW_EXPORTED)

xx(vgcfgrestore,
   "Restore volume group configuration",
   ALLOW_EXPORTED)

xx(vgchange,
   "Change volume group attributes",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ALLOW_HINTS)

xx(vgck,
   "Check the consistency of volume group(s)",
   ALL_VGS_IS_DEFAULT | LOCKD_VG_SH)

xx(vgconvert,
   "Change volume group metadata format",
   0)

xx(vgcreate,
   "Create a volume group",
   MUST_USE_ALL_ARGS | ENABLE_ALL_DEVS)

xx(vgdisplay,
   "Display volume group information",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(vgexport,
   "Unregister volume group(s) from the system",
   ALL_VGS_IS_DEFAULT)

xx(vgextend,
   "Add physical volumes to a volume group",
   MUST_USE_ALL_ARGS | ENABLE_ALL_DEVS)

xx(vgimport,
   "Register exported volume group with system",
   ALL_VGS_IS_DEFAULT | ALLOW_EXPORTED)

xx(vgimportclone,
   "Import a VG from cloned PVs",
   ALLOW_EXPORTED)

xx(vgimportdevices,
   "Add devices for a VG to the devices file.",
   ALL_VGS_IS_DEFAULT | ALLOW_EXPORTED)

xx(vgmerge,
   "Merge volume groups",
   0)

xx(vgmknodes,
   "Create the special files for volume group devices in /dev",
   ALL_VGS_IS_DEFAULT)

xx(vgreduce,
   "Remove physical volume(s) from a volume group",
   ALL_VGS_IS_DEFAULT)

xx(vgremove,
   "Remove volume group(s)",
   ALL_VGS_IS_DEFAULT) /* all VGs only with select */

xx(vgrename,
   "Rename a volume group",
   ALLOW_UUID_AS_NAME | ALLOW_EXPORTED)

xx(vgs,
   "Display information about volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | CAN_USE_ONE_SCAN | ALLOW_HINTS | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(vgscan,
   "Search for all volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | LOCKD_VG_SH | ALLOW_EXPORTED | CHECK_DEVS_USED | DEVICE_ID_NOT_FOUND)

xx(vgsplit,
   "Move physical volumes into a new or existing volume group",
   0)
