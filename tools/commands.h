/*
 * Copyright (C) 2001 Sistina Software
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * LVM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LVM; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

xx(e2fsadm,
   "Resize logical volume and ext2 filesystem",
   "e2fsadm "
   "[-d|--debug] " "[-h|--help] " "[-n|--nofsck]" "\n"
   "\t{[-l|--extents] [+|-]LogicalExtentsNumber |" "\n"
   "\t [-L|--size] [+|-]LogicalVolumeSize[kKmMgGtT]}" "\n"
   "\t[-t|--test] "  "\n"
   "\t[-v|--verbose] "  "\n"
   "\t[--version] " "\n"
   "\tLogicalVolumePath" "\n",

    extents_ARG, size_ARG, nofsck_ARG, test_ARG)

xx(help,
   "Display help for commands",
   "help <command>" "\n")

/*********
xx(lvactivate,
   "Activate logical volume on given partition(s)",
   "lvactivate "
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "Logical Volume(s)\n")
***********/

xx(lvchange,
   "Change the attributes of logical volume(s)",
   "lvchange\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-a/--available y/n]\n"
   "\t[-C/--contiguous y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-M/--persistent y/n] [--minor minor]\n"
   "\t[-P/--partial] " "\n"
   "\t[-p/--permission r/rw]\n"
   "\t[-r/--readahead ReadAheadSectors]\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

   autobackup_ARG, available_ARG, contiguous_ARG,
   minor_ARG, persistent_ARG, partial_ARG,
   permission_ARG, readahead_ARG, test_ARG)

xx(lvcreate,
   "Create a logical volume",
   "lvcreate " "\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[-c|--chunksize]\n"
   "\t[-C|--contiguous {y|n}]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n"
   "\t{-l|--extents LogicalExtentsNumber |\n"
   "\t -L|--size LogicalVolumeSize[kKmMgGtT]}\n"
   "\t[-M|--persistent {y|n}] [--minor minor]\n"
   "\t[-n|--name LogicalVolumeName]\n"
   "\t[-p|--permission {r|rw}]\n"
   "\t[-r|--readahead ReadAheadSectors]\n"
   "\t[-s|--snapshot]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[-Z|--zero {y|n}]\n"
   "\t[--version]\n"
   "\tVolumeGroupName [PhysicalVolumePath...]\n\n",

   autobackup_ARG, contiguous_ARG, extents_ARG, minor_ARG, name_ARG,
   permission_ARG, persistent_ARG, readahead_ARG, size_ARG,
   snapshot_ARG, stripes_ARG, stripesize_ARG, test_ARG, zero_ARG)

xx(lvdisplay,
   "Display information about a logical volume",
   "lvdisplay\n"
   "\t[-c/--colon]\n"
   "\t[-d/--debug]\n"
   "\t[-D/--disk]\n"
   "\t[-h/-?/--help]\n"
   "\t[-m/--maps]\n"
   "\t[-P/--partial] " "\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

    colon_ARG, disk_ARG, maps_ARG, partial_ARG)

xx(lvextend,
   "Add space to a logical volume",
   "lvextend\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n"
   "\t{-l/--extents [+]LogicalExtentsNumber |\n"
   "\t -L/--size [+]LogicalVolumeSize[kKmMgGtT]}\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [ PhysicalVolumePath... ]\n",

   autobackup_ARG, extents_ARG, size_ARG, stripes_ARG, stripesize_ARG,
   test_ARG)

xx(lvmchange,
   "With the device mapper, this is obsolete and does nothing.",
   "lvmchange\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-R/--reset]\n"
   "\t[-v/--verbose]\n",

    reset_ARG)

xx(lvmdiskscan,
   "List devices that may be used as physical volumes",
   "lvmdiskscan\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-l/--lvmpartition]\n",

   lvmpartition_ARG)

xx(lvmsadc,
   "Collect activity data",
   "lvmsadc\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\t[LogFilePath]\n" )

xx(lvmsar,
   "Create activity report",
   "lvmsar\n"
   "\t[-d/--debug]\n"
   "\t[-f/--full]\n"
   "\t[-h/-?/--help]\n"
   "\t[-s/--stdin]\n"
   "\t[-v/--verbose]\n"
   "\tLogFilePath\n",

   full_ARG,  stdin_ARG)

xx(lvreduce,
   "Reduce the size of a logical volume",
   "lvreduce\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-f/--force]\n"
   "\t[-h/-?/--help]\n"
   "\t{-l/--extents [-]LogicalExtentsNumber |\n"
   "\t -L/--size [-]LogicalVolumeSize[kKmMgGtT]}\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path]\n",

   autobackup_ARG, force_ARG,  extents_ARG,
   size_ARG, test_ARG, yes_ARG)

xx(lvremove,
   "Remove logical volume(s) from the system",
   "lvremove\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-f/--force]\n"
   "\t[-h/-?/--help]\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

   autobackup_ARG, force_ARG, test_ARG)

xx(lvrename,
   "Rename a logical volume",
   "lvrename "
   "\t[-A|--autobackup {y|n}] " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-h|--help] " "\n"
   "\t[-t|--test] " "\n"
   "\t[-v|--verbose]" "\n"
   "\t[--version] " "\n"
   "\t{ OldLogicalVolumePath NewLogicalVolumePath |" "\n"
   "\t  VolumeGroupName OldLogicalVolumeName NewLogicalVolumeName }\n",

   autobackup_ARG, test_ARG)

xx(lvresize,
   "Resize a logical volume",
   "lvresize\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n"
   "\t{-l/--extents [+/-]LogicalExtentsNumber |\n"
   "\t -L/--size [+/-]LogicalVolumeSize[kKmMgGtT]}\n"
   "\t[-t|--test]\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [ PhysicalVolumePath... ]\n",

   autobackup_ARG, extents_ARG, size_ARG, stripes_ARG, stripesize_ARG,
   test_ARG)

xx(lvscan,
   "List all logical volumes in all volume groups",
   "lvscan " "\n"
   "\t[-b|--blockdevice] " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-D|--disk]" "\n"
   "\t[-h|--help] " "\n"
   "\t[-P|--partial] " "\n"
   "\t[-v|--verbose] " "\n"
   "\t[--version]\n",

   blockdevice_ARG, disk_ARG, partial_ARG)

xx(pvchange,
   "Change attributes of physical volume(s)",
   "pvchange\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\t[-a/--all]\n"
   "\t[-t|--test]\n"
   "\t[-x/--allocatable y/n]\n"
   "\t[PhysicalVolumePath...]\n",

   all_ARG, autobackup_ARG, allocatable_ARG, allocation_ARG, test_ARG)

xx(pvcreate,
   "Initialize physical volume(s) for use by LVM",
   "pvcreate " "\n"
   "\t[-d|--debug]" "\n"
   "\t[-f[f]|--force [--force]] " "\n"
   "\t[-h|--help] " "\n"
   "\t[-y|--yes]" "\n"
   "\t[-s|--size PhysicalVolumeSize[kKmMgGtT]" "\n"
   "\t[-t|--test] " "\n"
   "\t[-u|--uuid uuid] " "\n"
   "\t[-v|--verbose] " "\n"
   "\t[--version] " "\n"
   "\tPhysicalVolume [PhysicalVolume...]\n",

   force_ARG, test_ARG, physicalvolumesize_ARG, uuidstr_ARG, yes_ARG)

xx(pvdata,
   "Display the on-disk metadata for physical volume(s)",
   "pvdata " "\n"
   "\t[-a|--all] " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-E|--physicalextent] " "\n"
   "\t[-h|--help]" "\n"
   "\t[-L|--logicalvolume] " "\n"
   "\t[-P[P]|--physicalvolume [--physicalvolume]]" "\n"
   "\t[-U|--uuidlist] " "\n"
   "\t[-v[v]|--verbose [--verbose]] " "\n"
   "\t[-V|--volumegroup]" "\n"
   "\t[--version] " "\n"
   "\tPhysicalVolume [PhysicalVolume...]\n",

   all_ARG,  logicalextent_ARG, physicalextent_ARG,
   physicalvolume_ARG, uuidlist_ARG, volumegroup_ARG)

xx(pvdisplay,
   "Display various attributes of logical volume(s)",
   "pvdisplay\n"
   "\t[-c/--colon]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-m/--maps]\n"
   "\t[-s/--short]\n"
   "\t[-v/--verbose]\n"
   "\tPhysicalVolumePath [PhysicalVolumePath...]\n",

   colon_ARG, maps_ARG, short_ARG)

xx(pvmove,
   "Move extents from one physical volume to another",
   "pvmove "
   "[-A|--autobackup {y|n}] "
   "[-d|--debug] "
   "[-f|--force]"
   "[-h|--help]\n\t"
   "[-t|--test] "
   "[-v|--verbose] "
   "[--version]\n\t"
   "[{-n|--name} LogicalVolume[:LogicalExtent[-LogicalExtent]...]]\n\t"
   "SourcePhysicalVolume[:PhysicalExtent[-PhysicalExtent]...]}\n\t"
   "[DestinationPhysicalVolume[:PhysicalExtent[-PhysicalExtent]...]...]\n",

   autobackup_ARG, force_ARG,  name_ARG, test_ARG)

xx(pvresize,
   "Resize a physical volume in use by a volume group",
   "pvmove "
   "[-A|--autobackup {y|n}] "
   "[-d|--debug] "
   "[-h|--help]\n\t"
   "[-s|--size PhysicalVolumeSize[kKmMgGtT]" "\n"
   "[-v|--verbose] "
   "[--version]\n\t"
   "\tPhysicalVolumePath [PhysicalVolumePath...]\n",

   autobackup_ARG, physicalvolumesize_ARG)

xx(pvscan,
   "List all physical volumes",
   "pvscan " "\n"
   "\t[-d|--debug] " "\n"
   "\t{-e|--exported | -n/--novolumegroup} " "\n"
   "\t[-h|--help]" "\n"
   "\t[-P|--partial] " "\n"
   "\t[-s|--short] " "\n"
   "\t[-u|--uuid] " "\n"
   "\t[-v|--verbose] " "\n"
   "\t[--version]\n",

   exported_ARG,  novolumegroup_ARG, partial_ARG, short_ARG, uuid_ARG)

xx(vgcfgbackup,
   "Backup volume group configuration(s)",
   "vgcfgbackup " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-f|--file filename] " "\n"
   "\t[-h|--help] " "\n"
   "\t[-v|--verbose]" "\n"
   "\t[-V|--version] " "\n"
   "\t[VolumeGroupName...]\n",
   file_ARG)

xx(vgcfgrestore,
   "Restore volume group configuration",
   "vgcfgrestore " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-f|--file filename] " "\n"
   "\t[-l[l]|--list [--list]]" "\n"
   "\t[-n|--name VolumeGroupName] " "\n"
   "\t[-h|--help]" "\n"
   "\t[-t|--test] " "\n"
   "\t[-v|--verbose]" "\n"
   "\t[--version] " "\n"
   "\tVolumeGroupName",

   file_ARG, list_ARG, name_ARG, test_ARG)

xx(vgchange,
   "Change volume group attributes",
   "vgchange" "\n"
   "\t[-A|--autobackup {y|n}] " "\n"
   "\t[-P|--partial] " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-h|--help] " "\n"
   "\t[-t|--test]" "\n"
   "\t[-v|--verbose] " "\n"
   "\t[--version]" "\n"
   "\t{-a|--available {y|n}  |" "\n"
   "\t -x|--resizeable {y|n} |" "\n"
   "\t -l|--logicalvolume MaxLogicalVolumes}" "\n"
   "\t[VolumeGroupName...]\n",

   autobackup_ARG, available_ARG, logicalvolume_ARG, partial_ARG,
   resizeable_ARG, resizable_ARG, allocation_ARG,
   test_ARG)

xx(vgck,
   "Check the consistency of volume group(s)",
   "vgck "
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\t[VolumeGroupName...]\n" )

xx(vgcreate,
   "Create a volume group",
   "vgcreate" "\n"
   "\t[-A|--autobackup {y|n}] " "\n"
   "\t[-d|--debug]" "\n"
   "\t[-h|--help]" "\n"
   "\t[-l|--maxlogicalvolumes MaxLogicalVolumes]" "\n"
   "\t[-M|--metadatatype lvm1/text] " "\n"
   "\t[-p|--maxphysicalvolumes MaxPhysicalVolumes] " "\n"
   "\t[-s|--physicalextentsize PhysicalExtentSize[kKmMgGtT]] " "\n"
   "\t[-t|--test] " "\n"
   "\t[-v|--verbose]" "\n"
   "\t[--version] " "\n"
   "\tVolumeGroupName PhysicalVolume [PhysicalVolume...]\n",

   autobackup_ARG, maxlogicalvolumes_ARG, maxphysicalvolumes_ARG,
   metadatatype_ARG, physicalextentsize_ARG, test_ARG)

xx(vgdisplay,
   "Display volume group information",
   "vgdisplay " "\n"
   "\t[-c|--colon | -s|--short | -v|--verbose]" "\n"
   "\t[-d|--debug] " "\n"
   "\t[-h|--help] " "\n"
   "\t[-P|--partial] " "\n"
   "\t[-A|--activevolumegroups | [-D|--disk]" "\n"
   "\t[--version]" "\n"
   "\t[VolumeGroupName...] ]\n",

   activevolumegroups_ARG, colon_ARG, disk_ARG, short_ARG, partial_ARG)

xx(vgexport,
   "Unregister volume group(s) from the system",
   "vgexport " "\n"
   "\t[-a|--all] " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-h|--help]" "\n"
   "\t[-v|--verbose] " "\n"
   "\t[--version] " "\n"
   "\tVolumeGroupName [VolumeGroupName...]\n",

   all_ARG, test_ARG)

xx(vgextend,
   "Add physical volumes to a volume group",
   "vgextend\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tVolumeGroupName PhysicalDevicePath [PhysicalDevicePath...]\n",

   autobackup_ARG, test_ARG)

xx(vgimport,
   "Register exported volume group with system",
   "vgimport " "\n"
   "\t[-a/--all]\n"
   "\t[-d|--debug] " "\n"
   "\t[-f|--force] " "\n"
   "\t[-h|--help] " "\n"
   "\t[-t|--test] " "\n"
   "\t[-v|--verbose]" "\n"
   "\tVolumeGroupName..." "\n",

   all_ARG, force_ARG, test_ARG)

xx(vgmerge,
   "Merge volume groups",
   "vgmerge\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-l/--list]\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tDestinationVolumeGroupName SourceVolumeGroupName\n",

   autobackup_ARG, list_ARG, test_ARG)

xx(vgmknodes,
   "Create the special files for volume group devices in /dev",
   "vgmknodes\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\t[VolumeGroupName...]\n" )

xx(vgreduce,
   "Remove physical volume(s) from a volume group",
   "vgreduce\n"
   "\t[-a/--all]\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tVolumeGroupName\n"
   "\t[PhysicalVolumePath...]\n",

   all_ARG, autobackup_ARG, test_ARG)

xx(vgremove,
   "Remove volume group(s)",
   "vgremove\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tVolumeGroupName [VolumeGroupName...]\n",

   test_ARG)

xx(vgrename,
   "Rename a volume group",
   "vgrename\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-f/--force]\n"
   "\t[-h/-?/--help]\n"
   "\t[-t/--test]\n"
   "\t[-v/--verbose]\n"
   "\tOldVolumeGroupPath NewVolumeGroupPath |\n"
   "\tOldVolumeGroupName NewVolumeGroupName\n",

   autobackup_ARG, force_ARG, test_ARG)

xx(vgscan,
   "Search for all volume groups",
   "vgscan "
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-P/--partial] " "\n"
   "\t[-v/--verbose]\n" ,
   partial_ARG)

xx(vgsplit,
   "Move physical volumes into a new volume group",
   "vgsplit " "\n"
   "\t[-A|--autobackup {y|n}] " "\n"
   "\t[-d|--debug] " "\n"
   "\t[-h|--help] " "\n"
   "\t[-l|--list]" "\n"
   "\t[-t|--test] " "\n"
   "\t[-v|--verbose] " "\n"
   "\t[--version]" "\n"
   "\tExistingVolumeGroupName NewVolumeGroupName" "\n"
   "\tPhysicalVolumePath [PhysicalVolumePath...]\n",

   autobackup_ARG, list_ARG, test_ARG)

xx(version,
   "Display software and driver version information",
   "version\n" )

