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
   "[-d|--debug] "
   "[-h|--help] "
   "[-n|--nofsck]\n\t"
   "{[-l|--extents] [+|-]LogicalExtentsNumber |\n\t"
   " [-L|--size] [+|-]LogicalVolumeSize[kKmMgGtT]}\n\t"
   "[-t|--test] "
   "[-v|--verbose] "
   "[--version] "
   "LogicalVolumePath\n",

    extents_ARG, size_ARG, nofsck_ARG, test_ARG)

xx(help,
   "Display help for commands",
   "help <command>\n")

xx(lvactivate,
   "Activate logical volume on given partition(s)",
  "lvactivate "
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
  "Physical Volume(s)\n")

xx(lvchange,
   "Change the attributes of logical volume(s)",
   "lvchange\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-a/--available y/n]\n"
   "\t[-C/--contiguous y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-p/--permission r/rw]\n"
   "\t[-r/--readahead ReadAheadSectors]\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

   autobackup_ARG, available_ARG, contiguous_ARG,
   permission_ARG, readahead_ARG)

xx(lvcreate,
   "Create a logical volume",
   "lvcreate "
   "[-A|--autobackup {y|n}] "
   "[-C|--contiguous {y|n}] "
   "[-d|--debug]\n"
   "\t[-h|--help] "
   "[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n\t"
   "{-l|--extents LogicalExtentsNumber |\n\t"
   " -L|--size LogicalVolumeSize[kKmMgGtT]} "
   "[-n|--name LogicalVolumeName]\n\t"
   "[-p|--permission {r|rw}] "
   "[-r|--readahead ReadAheadSectors]\n\t"
   "[-v|--verbose] "
   "[-Z|--zero {y|n}] "
   "[--version]\n\t"
   "VolumeGroupName [PhysicalVolumePath...]\n\n"
   "lvcreate "
   "-s|--snapshot "
   "[-c|--chunksize ChunkSize]\n\t"
   "{-l|--extents LogicalExtentsNumber |\n\t"
   " -L|--size LogicalVolumeSize[kKmMgGtT]}\n\t"
   "-n|--name SnapshotLogicalVolumeName\n\t"
   "LogicalVolume[Path] [PhysicalVolumePath...]\n",

   autobackup_ARG, chunksize_ARG, contiguous_ARG,
   stripes_ARG, stripesize_ARG, extents_ARG, size_ARG, name_ARG,
   permission_ARG, readahead_ARG, snapshot_ARG, zero_ARG)

xx(lvdisplay,
   "Display information about a logical volume",
   "lvdisplay\n"
   "\t[-c/--colon]\n"
   "\t[-d/--debug]\n"
   "\t[-D/--disk]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v[v]/--verbose [--verbose]]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

    colon_ARG, disk_ARG)

xx(lvextend,
   "Add space to a logical volume",
   "lvextend\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t{-l/--extents [+]LogicalExtentsNumber |\n"
   "\t -L/--size [+]LogicalVolumeSize[kKmMgGtT]}\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [ PhysicalVolumePath... ]\n",

   autobackup_ARG, extents_ARG, size_ARG)

xx(lvmchange,
   "Reset the LVM driver - not for general use",
   "lvmchange\n" 
   "\t[-d/--debug]\n"
   "\t[-f/--force]\n"
   "\t[-h/-?/--help]\n"
   "\t[-i/-?/--iop_version]\n"
   "\t[-R/--reset]\n"
   "\t[-v/--verbose]\n",

    force_ARG, reset_ARG)

xx(lvmdiskscan,
   "List devices that may be used as physical volumes",
   "lvmdiskscan\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-l/--lvmpartition]\n"
   "\t[-v/--verbose]\n",

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
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path]\n",

   autobackup_ARG, force_ARG,  extents_ARG,
   size_ARG, yes_ARG)

xx(lvremove,
   "Remove logical volume(s) from the system",
   "lvremove\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-f/--force]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

   autobackup_ARG, force_ARG)

xx(lvrename,
   "Rename a logical volume",
   "lvrename "
   "[-A|--autobackup {y|n}] "
   "[-d|--debug] "
   "[-h|--help] "
   "[-v|--verbose]\n\t"
   "[--version] "
   "{ OldLogicalVolumePath NewLogicalVolumePath |\n\t"
   "  VolumeGroupName OldLogicalVolumeName NewLogicalVolumeName }\n",

   autobackup_ARG)

xx(lvscan,
   "List all logical volumes in all volume groups",
   "lvscan "
   "[-b|--blockdevice] "
   "[-d|--debug] "
   "[-D|--disk]\n\t"
   "[-h|--help] "
   "[-v|--verbose] "
   "[--version]\n",

   blockdevice_ARG, disk_ARG)

xx(pvchange,
   "Change attributes of physical volume(s)",
   "pvchange\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\t[-a/--all]\n"
   "\t[-x/--allocation y/n]\n"
   "\t[PhysicalVolumePath...]\n",

   all_ARG, autobackup_ARG, allocation_ARG)

xx(pvcreate,
   "Initialize physical volume(s) for use by LVM",
   "pvcreate "
   "[-d|--debug]"
   "[-f[f]|--force [--force]] "
   "[-h|--help] "
   "[-y|--yes]\n\t"
   "[-v|--verbose] "
   "[--version] "
   "PhysicalVolume [PhysicalVolume...]\n",

   force_ARG, yes_ARG)

xx(pvdata,
   "Display the on-disk metadata for physical volume(s)",
   "pvdata "
   "[-a|--all] "
   "[-d|--debug] "
   "[-E|--physicalextent] "
   "[-h|--help]\n\t"
   "[-L|--logicalvolume] "
   "[-P[P]|--physicalvolume [--physicalvolume]]\n\t"
   "[-U|--uuidlist] "
   "[-v[v]|--verbose [--verbose]] "
   "[-V|--volumegroup]\n\t"
   "[--version] "
   "PhysicalVolume [PhysicalVolume...]\n",

   all_ARG,  logicalextent_ARG, physicalextent_ARG,
   physicalvolume_ARG, uuidlist_ARG, volumegroup_ARG)

xx(pvdisplay,
   "Display various attributes of logical volume(s)",
   "pvdisplay\n"
   "\t[-c/--colon]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-s/--short]\n"
   "\t[-v[v]/--verbose [--verbose]]\n"
   "\tPhysicalVolumePath [PhysicalVolumePath...]\n",

   colon_ARG, short_ARG)

#if 0
xx(pvmove,
   "Move extents from one physical volume to another",
   "pvmove "
   "[-A|--autobackup {y|n}] "
   "[-d|--debug] "
   "[-f|--force]"
   "[-h|--help]\n\t"
   "[-t|--test] "
   "[-v[v]|--verbose [--verbose]] "
   "[--version]\n\t"
   "[{-n|--name} LogicalVolume[:LogicalExtent[-LogicalExtent]...]]\n\t"
   "SourcePhysicalVolume[:PhysicalExtent[-PhysicalExtent]...]}\n\t"
   "[DestinationPhysicalVolume[:PhysicalExtent[-PhysicalExtent]...]...]\n",

   autobackup_ARG, force_ARG,  name_ARG, test_ARG)
#endif

xx(pvscan,
   "List all physical volumes",
   "pvscan "
   "[-d|--debug] "
   "{-e|--exported | -n/--novolumegroup} "
   "[-h|--help]\n\t"
   "[-s|--short] "
   "[-u|--uuid] "
   "[-v[v]|--verbose [--verbose]] "
   "[--version]\n",

   exported_ARG,  novolumegroup_ARG, short_ARG, uuid_ARG)

xx(vgcfgbackup,
   "Backup volume group configuration(s)",
   "vgcfgbackup "
   "[-d|--debug] "
   "[-h|--help] "
   "[-v|--verbose]\n\t"
   "[-V|--version] "
   "[VolumeGroupName...]\n" )

xx(vgcfgrestore,
   "Restore volume group configuration",
   "vgcfgrestore "
   "[-d|--debug] "
   "[-f|--file VGConfPath] "
   "[-l[l]|--list [--list]]\n\t"
   "[-n|--name VolumeGroupName] "
   "[-h|--help]\n\t"
   "[-o|--oldpath OldPhysicalVolumePath] "
   "[-t|--test] "
   "[-v|--verbose]\n\t"
   "[--version] "
   "[PhysicalVolumePath]\n",

   file_ARG, list_ARG, name_ARG, oldpath_ARG, test_ARG)

xx(vgchange,
   "Change volume group attributes",
   "vgchange "
   "[-A|--autobackup {y|n}] "
   "[-d|--debug] "
   "[-h|--help]\n\t"
   "{-a|--available {y|n} [VolumeGroupName...] |\n\t "
   " -x|--allocation {y|n} [VolumeGroupName...]\n\t"
   " -l|--logicalvolume MaxLogicalVolumes}\n\t"
   "[-v|--verbose] "
   "[--version]\n",

   autobackup_ARG, available_ARG, logicalvolume_ARG, allocation_ARG )

xx(vgck,
   "Check the consistency of volume group(s)",
   "vgck "
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\t[VolumeGroupName...]\n" )

xx(vgcreate,
   "Create a volume group",
   "vgcreate"
   "[-A|--autobackup {y|n}] "
   "[-d|--debug]\n\t"
   "[-l|--maxlogicalvolumes MaxLogicalVolumes]\n\t"
   "[-p|--maxphysicalvolumes MaxPhysicalVolumes] "
   "[-h|--help]\n\t"
   "[-s|--physicalextentsize PhysicalExtentSize[kKmMgGtT]] "
   "[-v|--verbose]\n\t"
   "[--version] "
   "VolumeGroupName "
   "PhysicalVolume [PhysicalVolume...]\n",

   autobackup_ARG, maxlogicalvolumes_ARG, maxphysicalvolumes_ARG,
   physicalextentsize_ARG)

xx(vgdisplay,
   "Display volume group information",
   "vgdisplay "
   "[-c|--colon | -s|--short | -v[v]|--verbose [--verbose]]\n\t"
   "[-d|--debug] "
   "[-h|--help] "
   "[--version]\n\t"
   "[-A|--activevolumegroups | [-D|--disk] [VolumeGroupName...] ]\n",

   activevolumegroups_ARG, colon_ARG, disk_ARG, short_ARG)

xx(vgexport,
   "Unregister volume group(s) from the system",
   "vgexport "
   "[-a|--all] "
   "[-d|--debug] "
   "[-h|--help]\n\t"
   "[-v|--verbose] "
   "[--version] "
   "VolumeGroupName [VolumeGroupName...]\n",

   all_ARG)

xx(vgextend,
   "Add physical volumes to a volume group",
   "vgextend\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\tVolumeGroupName\n"
   "\tPhysicalDevicePath [PhysicalDevicePath...]\n",

   autobackup_ARG)

xx(vgimport,
   "Register exported volume group with system",
   "vgimport "
   "[-d|--debug] "
   "[-f|--force] "
   "[-h|--help] "
   "[-v|--verbose]\n\t"
   "VolumeGroupName PhysicalVolumePath "
   "[PhysicalVolumePath...]\n",

   force_ARG)

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
   "\t[-v/--verbose]\n"
   "\tVolumeGroupName\n"
   "\t[PhysicalVolumePath...]\n",

   all_ARG, autobackup_ARG)

xx(vgremove,
   "Remove volume group(s)",
   "vgremove\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\tVolumeGroupName [VolumeGroupName...]\n" )

xx(vgrename,
   "Rename a volume group",
   "vgrename\n"
   "\t[-A/--autobackup y/n]\n"
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n"
   "\tOldVolumeGroupPath NewVolumeGroupPath /\n"
   "\tOldVolumeGroupName NewVolumeGroupName\n",

   autobackup_ARG)

xx(vgscan,
   "Search for all volume groups",
   "vgscan "
   "\t[-d/--debug]\n"
   "\t[-h/-?/--help]\n"
   "\t[-v/--verbose]\n" )

xx(vgsplit,
   "Move physical volumes into a new volume group",
   "vgsplit "
   "[-A|--autobackup {y|n}] "
   "[-d|--debug] "
   "[-h|--help] "
   "[-l|--list]\n\t"
   "[-t|--test] "
   "[-v|--verbose] "
   "[--version]\n\t"
   "ExistingVolumeGroupName NewVolumeGroupName\n\t"
   "PhysicalVolumePath [PhysicalVolumePath...]\n",

   autobackup_ARG, list_ARG, test_ARG)
