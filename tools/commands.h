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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/***********  Replace with script?
xx(e2fsadm,
   "Resize logical volume and ext2 filesystem",
   "e2fsadm "
   "[-d|--debug] " "[-h|--help] " "[-n|--nofsck]\n"
   "\t{[-l|--extents] [+|-]LogicalExtentsNumber |\n"
   "\t [-L|--size] [+|-]LogicalVolumeSize[bBsSkKmMgGtTpPeE]}\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tLogicalVolumePath\n",

    extents_ARG, size_ARG, nofsck_ARG, test_ARG)
*********/

xx(config,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY,
   "config\n"
   "\t[-f|--file filename]\n"
   "\t[--type {current|default|diff|list|missing|new|profilable|profilable-command|profilable-metadata}\n"
   "\t[--atversion version]]\n"
   "\t[--ignoreadvanced]\n"
   "\t[--ignoreunsupported]\n"
   "\t[--ignorelocal]\n"
   "\t[-l|--list]\n"
   "\t[--config ConfigurationString]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[--profile ProfileName]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[--mergedconfig]\n"
   "\t[--showdeprecated]\n"
   "\t[--showunsupported]\n"
   "\t[--validate]\n"
   "\t[--withsummary]\n"
   "\t[--withcomments]\n"
   "\t[--unconfigured]\n"
   "\t[--withversions]\n"
   "\t[ConfigurationNode...]\n",
   atversion_ARG, configtype_ARG, file_ARG, ignoreadvanced_ARG,
   ignoreunsupported_ARG, ignorelocal_ARG, list_ARG, mergedconfig_ARG, metadataprofile_ARG,
   showdeprecated_ARG, showunsupported_ARG, validate_ARG, withsummary_ARG, withcomments_ARG,
   unconfigured_ARG, withversions_ARG)

xx(devtypes,
   "Display recognised built-in block device types",
   PERMITTED_READ_ONLY,
   "devtypes\n"
   "\t[--aligned]\n"
   "\t[--binary]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[--nameprefixes]\n"
   "\t[--noheadings]\n"
   "\t[--nosuffix]\n"
   "\t[-o|--options [+]Field[,Field]]\n"
   "\t[-O|--sort [+|-]key1[,[+|-]key2[,...]]]\n"
   "\t[--rows]\n"
   "\t[-S|--select Selection]\n"
   "\t[--separator Separator]\n"
   "\t[--unbuffered]\n"
   "\t[--unquoted]\n"
   "\t[--version]\n",

   aligned_ARG, binary_ARG, nameprefixes_ARG,
   noheadings_ARG, nosuffix_ARG, options_ARG,
   rows_ARG, select_ARG, separator_ARG, sort_ARG,
   unbuffered_ARG, unquoted_ARG)

xx(dumpconfig,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY,
   "dumpconfig\n"
   "\t[-f|--file filename]\n"
   "\t[--type {current|default|diff|list|missing|new|profilable|profilable-command|profilable-metadata}\n"
   "\t[--atversion version]]\n"
   "\t[--ignoreadvanced]\n"
   "\t[--ignoreunsupported]\n"
   "\t[--ignorelocal]\n"
   "\t[-l|--list]\n"
   "\t[--config ConfigurationString]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[--profile ProfileName]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[--mergedconfig]\n"
   "\t[--showdeprecated]\n"
   "\t[--showunsupported]\n"
   "\t[--validate]\n"
   "\t[--withsummary]\n"
   "\t[--withcomments]\n"
   "\t[--unconfigured]\n"
   "\t[--withversions]\n"
   "\t[ConfigurationNode...]\n",
   atversion_ARG, configtype_ARG, file_ARG, ignoreadvanced_ARG,
   ignoreunsupported_ARG, ignorelocal_ARG, list_ARG, mergedconfig_ARG, metadataprofile_ARG,
   showdeprecated_ARG, showunsupported_ARG, validate_ARG, withsummary_ARG, withcomments_ARG,
   unconfigured_ARG, withversions_ARG)

xx(formats,
   "List available metadata formats",
   PERMITTED_READ_ONLY,
   "formats\n")

xx(help,
   "Display help for commands",
   PERMITTED_READ_ONLY,
   "help <command>\n")

/*********
xx(lvactivate,
   "Activate logical volume on given partition(s)",
   "lvactivate "
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-v|--verbose]\n"
   "Logical Volume(s)\n")
***********/

xx(lvchange,
   "Change the attributes of logical volume(s)",
   CACHE_VGMETADATA | PERMITTED_READ_ONLY,
   "lvchange\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[-a|--activate [a|e|l]{y|n}]\n"
   "\t[--activationmode {complete|degraded|partial}"
   "\t[--addtag Tag]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[-C|--contiguous y|n]\n"
   "\t[--cachepolicy policyname] [--cachesettings parameter=value]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--deltag Tag]\n"
   "\t[--detachprofile]\n"
   "\t[--errorwhenfull {y|n}]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[--discards {ignore|nopassdown|passdown}]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoremonitoring]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[-k|--setactivationskip {y|n}]\n"
   "\t[-K|--ignoreactivationskip]\n"
   "\t[--monitor {y|n}]\n"
   "\t[--poll {y|n}]\n"
   "\t[--noudevsync]\n"
   "\t[-M|--persistent y|n] [-j|--major major] [--minor minor]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[-P|--partial]\n"
   "\t[-p|--permission r|rw]\n"
   "\t[--[raid]minrecoveryrate Rate]\n"
   "\t[--[raid]maxrecoveryrate Rate]\n"
   "\t[--[raid]syncaction {check|repair}\n"
   "\t[--[raid]writebehind IOCount]\n"
   "\t[--[raid]writemostly PhysicalVolume[:{t|n|y}]]\n"
   "\t[-r|--readahead ReadAheadSectors|auto|none]\n"
   "\t[--refresh]\n"
   "\t[--resync]\n"
   "\t[-S|--select Selection]\n"
   "\t[--sysinit]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[-y|--yes]\n"
   "\t[-Z|--zero {y|n}]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

   activationmode_ARG, addtag_ARG, alloc_ARG, autobackup_ARG, activate_ARG,
   available_ARG, cachepolicy_ARG, cachesettings_ARG, contiguous_ARG, deltag_ARG,
   discards_ARG, detachprofile_ARG, errorwhenfull_ARG, force_ARG,
   ignorelockingfailure_ARG, ignoremonitoring_ARG, ignoreactivationskip_ARG,
   ignoreskippedcluster_ARG, major_ARG, metadataprofile_ARG, minor_ARG,
   monitor_ARG, minrecoveryrate_ARG, maxrecoveryrate_ARG, noudevsync_ARG,
   partial_ARG, permission_ARG, persistent_ARG, poll_ARG,
   raidminrecoveryrate_ARG, raidmaxrecoveryrate_ARG, raidsyncaction_ARG,
   raidwritebehind_ARG, raidwritemostly_ARG, readahead_ARG, resync_ARG,
   refresh_ARG, select_ARG, setactivationskip_ARG, syncaction_ARG, sysinit_ARG,
   test_ARG, writebehind_ARG, writemostly_ARG, zero_ARG)

#define COMMON_OPTS \
	"\t[--commandprofile ProfileName] [-d|--debug] [-h|-?|--help]\n" \
	"\t[--noudevsync] [-t|--test] [-v|--verbose] [--version] [-y|--yes]\n"

xx(lvconvert,
   "Change logical volume layout",
   0,
   "lvconvert "
   "[-m|--mirrors Mirrors [{--mirrorlog {disk|core|mirrored}|--corelog}]]\n"
   "\t[--type SegmentType]\n"
   "\t[--repair [--use-policies]]\n"
   "\t[--replace PhysicalVolume]\n"
   "\t[-R|--regionsize MirrorLogRegionSize]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[-b|--background]\n"
   "\t[-f|--force]\n"
   "\t[-i|--interval seconds]\n"
   "\t[--stripes Stripes [-I|--stripesize StripeSize]]\n"
   COMMON_OPTS
   "\tLogicalVolume[Path] [PhysicalVolume[Path]...]\n\n"

   "lvconvert "
   "[--splitmirrors Images --trackchanges]\n"
   "\t[--splitmirrors Images --name SplitLogicalVolumeName]\n"
   COMMON_OPTS
   "\tLogicalVolume[Path] [SplittablePhysicalVolume[Path]...]\n\n"

   "lvconvert "
   "--splitsnapshot\n"
   COMMON_OPTS
   "\tSnapshotLogicalVolume[Path]\n\n"

   "lvconvert "
   "--splitcache\n"
   COMMON_OPTS
   "\tCacheLogicalVolume[Path]\n\n"

   "lvconvert "
   "--split\n"
   "\t[--name SplitLogicalVolumeName]\n"
   COMMON_OPTS
   "\tSplitableLogicalVolume[Path]\n\n"

   "lvconvert "
   "--uncache\n"
   COMMON_OPTS
   "\tCacheLogicalVolume[Path]\n\n"

   "lvconvert "
   "[--type snapshot|-s|--snapshot]\n"
   "\t[-c|--chunksize]\n"
   "\t[-Z|--zero {y|n}]\n"
   COMMON_OPTS
   "\tOriginalLogicalVolume[Path] SnapshotLogicalVolume[Path]\n\n"

   "lvconvert "
   "--merge\n"
   "\t[-b|--background]\n"
   "\t[-i|--interval seconds]\n"
   COMMON_OPTS
   "\tLogicalVolume[Path]\n\n"

   "lvconvert "
   "[--type thin[-pool]|-T|--thin]\n"
   "\t[--thinpool ThinPoolLogicalVolume[Path]]\n"
   "\t[--chunksize size]\n"
   "\t[--discards {ignore|nopassdown|passdown}]\n"
   "\t[--poolmetadataspare {y|n}]\n"
   "\t[{--poolmetadata ThinMetadataLogicalVolume[Path] |\n"
   "\t --poolmetadatasize size}]\n"
   "\t[-r|--readahead ReadAheadSectors|auto|none]\n"
   "\t[--stripes Stripes [-I|--stripesize StripeSize]]]\n"
   "\t[--originname NewExternalOriginVolumeName]]\n"
   "\t[-Z|--zero {y|n}]\n"
   COMMON_OPTS
   "\t[ExternalOrigin|ThinDataPool]LogicalVolume[Path] [PhysicalVolumePath...]\n\n"

   "lvconvert "
   "[--type cache[-pool]|-H|--cache]\n"
   "\t[--cachepool CacheDataLogicalVolume[Path]]\n"
   "\t[--cachemode CacheMode]\n"
   "\t[--chunksize size]\n"
   "\t[--poolmetadataspare {y|n}]]\n"
   "\t[{--poolmetadata CacheMetadataLogicalVolume[Path] |\n"
   "\t --poolmetadatasize size}]\n"
   COMMON_OPTS
   "\t[Cache|CacheDataPool]LogicalVolume[Path] [PhysicalVolumePath...]\n\n",

   alloc_ARG, background_ARG, cache_ARG, cachemode_ARG, cachepool_ARG, chunksize_ARG,
   corelog_ARG, discards_ARG, force_ARG, interval_ARG, merge_ARG, mirrorlog_ARG,
   mirrors_ARG, name_ARG, noudevsync_ARG, originname_ARG, poolmetadata_ARG,
   poolmetadatasize_ARG, poolmetadataspare_ARG, readahead_ARG, regionsize_ARG,
   repair_ARG, replace_ARG, snapshot_ARG,
   split_ARG, splitcache_ARG, splitmirrors_ARG, splitsnapshot_ARG,
   stripes_long_ARG, stripesize_ARG, test_ARG, thin_ARG, thinpool_ARG,
   trackchanges_ARG, type_ARG, uncache_ARG, use_policies_ARG, zero_ARG)

xx(lvcreate,
   "Create a logical volume",
   0,
   "lvcreate\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[-a|--activate [a|e|l]{y|n}]\n"
   "\t[--addtag Tag]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[-H|--cache\n"
   "\t  [--cachemode {writeback|writethrough}]\n"
   "\t[--cachepool CachePoolLogicalVolume{Name|Path}]\n"
   "\t[-c|--chunksize ChunkSize]\n"
   "\t[-C|--contiguous {y|n}]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|-?|--help]\n"
   "\t[--errorwhenfull {y|n}]\n"
   "\t[--ignoremonitoring]\n"
   "\t[--monitor {y|n}]\n"
   "\t[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n"
   "\t[-k|--setactivationskip {y|n}]\n"
   "\t[-K|--ignoreactivationskip]\n"
   "\t{-l|--extents LogicalExtentsNumber[%{VG|PVS|FREE}] |\n"
   "\t -L|--size LogicalVolumeSize[bBsSkKmMgGtTpPeE]}\n"
   "\t[-M|--persistent {y|n}] [-j|--major major] [--minor minor]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[-m|--mirrors Mirrors [--nosync]\n"
   "\t  [{--mirrorlog {disk|core|mirrored}|--corelog}]]\n"
   "\t[-n|--name LogicalVolumeName]\n"
   "\t[--noudevsync]\n"
   "\t[-p|--permission {r|rw}]\n"
   //"\t[--pooldatasize DataSize[bBsSkKmMgGtTpPeE]]\n"
   "\t[--poolmetadatasize MetadataSize[bBsSkKmMgG]]\n"
   "\t[--poolmetadataspare {y|n}]]\n"
   "\t[--[raid]minrecoveryrate Rate]\n"
   "\t[--[raid]maxrecoveryrate Rate]\n"
   "\t[-r|--readahead {ReadAheadSectors|auto|none}]\n"
   "\t[-R|--regionsize MirrorLogRegionSize]\n"
   "\t[-T|--thin\n"
   "\t  [--discards {ignore|nopassdown|passdown}]\n"
   "\t[--thinpool ThinPoolLogicalVolume{Name|Path}]\n"
   "\t[-t|--test]\n"
   "\t[--type VolumeType]\n"
   "\t[-v|--verbose]\n"
   "\t[-W|--wipesignatures {y|n}]\n"
   "\t[-Z|--zero {y|n}]\n"
   "\t[--version]\n"
   "\tVolumeGroupName [PhysicalVolumePath...]\n\n"

   "lvcreate\n"
   "\t{ {-s|--snapshot} OriginalLogicalVolume[Path] |\n"
   "\t  [-s|--snapshot] VolumeGroupName[Path] -V|--virtualsize VirtualSize}\n"
   "\t  {-H|--cache} VolumeGroupName[Path][/OriginalLogicalVolume]\n"
   "\t  {-T|--thin} VolumeGroupName[Path][/PoolLogicalVolume]\n"
   "\t              -V|--virtualsize VirtualSize}\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[--addtag Tag]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[--cachepolicy Policy] [--cachesettings Key=Value]\n"
   "\t[-c|--chunksize]\n"
   "\t[-C|--contiguous {y|n}]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--discards {ignore|nopassdown|passdown}]\n"
   "\t[-h|-?|--help]\n"
   "\t[--ignoremonitoring]\n"
   "\t[--monitor {y|n}]\n"
   "\t[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n"
   "\t[-k|--setactivationskip {y|n}]\n"
   "\t[-K|--ignoreactivationskip]\n"
   "\t{-l|--extents LogicalExtentsNumber[%{VG|FREE|ORIGIN}] |\n"
   "\t -L|--size LogicalVolumeSize[bBsSkKmMgGtTpPeE]}\n"
   //"\t[--pooldatasize DataVolumeSize[bBsSkKmMgGtTpPeE]]\n"
   "\t[--poolmetadatasize MetadataVolumeSize[bBsSkKmMgG]]\n"
   "\t[-M|--persistent {y|n}] [-j|--major major] [--minor minor]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[-n|--name LogicalVolumeName]\n"
   "\t[--noudevsync]\n"
   "\t[-p|--permission {r|rw}]\n"
   "\t[-r|--readahead ReadAheadSectors|auto|none]\n"
   "\t[-t|--test]\n"
   "\t[{--thinpool ThinPoolLogicalVolume[Path] |\n"
   "\t  --cachepool CachePoolLogicalVolume[Path]}]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[PhysicalVolumePath...]\n\n",

   addtag_ARG, alloc_ARG, autobackup_ARG, activate_ARG, available_ARG,
   cache_ARG, cachemode_ARG, cachepool_ARG, cachepolicy_ARG, cachesettings_ARG,
   chunksize_ARG, contiguous_ARG, corelog_ARG, discards_ARG, errorwhenfull_ARG,
   extents_ARG, ignoreactivationskip_ARG, ignoremonitoring_ARG, major_ARG,
   metadataprofile_ARG, minor_ARG, mirrorlog_ARG, mirrors_ARG, monitor_ARG,
   minrecoveryrate_ARG, maxrecoveryrate_ARG, name_ARG, nosync_ARG,
   noudevsync_ARG, permission_ARG, persistent_ARG,
   //pooldatasize_ARG,
   poolmetadatasize_ARG, poolmetadataspare_ARG,
   raidminrecoveryrate_ARG, raidmaxrecoveryrate_ARG,
   readahead_ARG, regionsize_ARG, setactivationskip_ARG, size_ARG, snapshot_ARG,
   stripes_ARG, stripesize_ARG, test_ARG, thin_ARG, thinpool_ARG, type_ARG,
   virtualoriginsize_ARG, virtualsize_ARG, wipesignatures_ARG, zero_ARG)

xx(lvdisplay,
   "Display information about a logical volume",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_FOREIGN_VGS,
   "lvdisplay\n"
   "\t[-a|--all]\n"
   "\t[-c|--colon]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[-m|--maps]\n"
   "\t[--nosuffix]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[-S|--select Selection]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[LogicalVolume[Path] [LogicalVolume[Path]...]]\n"
   "\n"
   "lvdisplay --columns|-C\n"
   "\t[--aligned]\n"
   "\t[-a|--all]\n"
   "\t[--binary]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--noheadings]\n"
   "\t[--nosuffix]\n"
   "\t[-o|--options [+]Field[,Field]]\n"
   "\t[-O|--sort [+|-]key1[,[+|-]key2[,...]]]\n"
   "\t[-S|--select Selection]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[--segments]\n"
   "\t[--separator Separator]\n"
   "\t[--unbuffered]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[LogicalVolume[Path] [LogicalVolume[Path]...]]\n",

    aligned_ARG, all_ARG, binary_ARG, colon_ARG, columns_ARG, foreign_ARG,
    ignorelockingfailure_ARG, ignoreskippedcluster_ARG, maps_ARG,
    noheadings_ARG, nosuffix_ARG, options_ARG, sort_ARG, partial_ARG,
    readonly_ARG, segments_ARG, select_ARG, separator_ARG,
    unbuffered_ARG, units_ARG)

xx(lvextend,
   "Add space to a logical volume",
   0,
   "lvextend\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n"
   "\t{-l|--extents [+]LogicalExtentsNumber[%{VG|LV|PVS|FREE|ORIGIN}] |\n"
   "\t -L|--size [+]LogicalVolumeSize[bBsSkKmMgGtTpPeE]}\n"
   "\t --poolmetadatasize [+]MetadataVolumeSize[bBsSkKmMgG]}\n"
   "\t[-m|--mirrors Mirrors]\n"
   "\t[--nosync]\n"
   "\t[--use-policies]\n"
   "\t[-n|--nofsck]\n"
   "\t[--noudevsync]\n"
   "\t[-r|--resizefs]\n"
   "\t[-t|--test]\n"
   "\t[--type VolumeType]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tLogicalVolume[Path] [ PhysicalVolumePath... ]\n",

   alloc_ARG, autobackup_ARG, extents_ARG, force_ARG, mirrors_ARG,
   nofsck_ARG, nosync_ARG, noudevsync_ARG, poolmetadatasize_ARG,
   resizefs_ARG, size_ARG, stripes_ARG,
   stripesize_ARG, test_ARG, type_ARG, use_policies_ARG)

xx(lvmchange,
   "With the device mapper, this is obsolete and does nothing.",
   0,
   "lvmchange\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-R|--reset]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n",

    reset_ARG)

xx(lvmconfig,
   "Display and manipulate configuration information",
   PERMITTED_READ_ONLY,
   "lvmconfig\n"
   "\t[-f|--file filename]\n"
   "\t[--type {current|default|diff|list|missing|new|profilable|profilable-command|profilable-metadata}\n"
   "\t[--atversion version]]\n"
   "\t[--ignoreadvanced]\n"
   "\t[--ignoreunsupported]\n"
   "\t[--ignorelocal]\n"
   "\t[-l|--list]\n"
   "\t[--config ConfigurationString]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[--profile ProfileName]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[--mergedconfig]\n"
   "\t[--showdeprecated]\n"
   "\t[--showunsupported]\n"
   "\t[--validate]\n"
   "\t[--withsummary]\n"
   "\t[--withcomments]\n"
   "\t[--unconfigured]\n"
   "\t[--withversions]\n"
   "\t[ConfigurationNode...]\n",
   atversion_ARG, configtype_ARG, file_ARG, ignoreadvanced_ARG,
   ignoreunsupported_ARG, ignorelocal_ARG, list_ARG, mergedconfig_ARG, metadataprofile_ARG,
   showdeprecated_ARG, showunsupported_ARG, validate_ARG, withsummary_ARG, withcomments_ARG,
   unconfigured_ARG, withversions_ARG)

xx(lvmdiskscan,
   "List devices that may be used as physical volumes",
   PERMITTED_READ_ONLY | ENABLE_ALL_DEVS,
   "lvmdiskscan\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-l|--lvmpartition]\n"
   "\t[--readonly]\n"
   "\t[--version]\n",

   lvmpartition_ARG, readonly_ARG)

xx(lvmsadc,
   "Collect activity data",
   0,
   "lvmsadc\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[LogFilePath]\n")

xx(lvmsar,
   "Create activity report",
   0,
   "lvmsar\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--full]\n"
   "\t[-h|--help]\n"
   "\t[-s|--stdin]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tLogFilePath\n",

   full_ARG, stdin_ARG)

xx(lvreduce,
   "Reduce the size of a logical volume",
   0,
   "lvreduce\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t{-l|--extents [-]LogicalExtentsNumber[%{VG|LV|FREE|ORIGIN}] |\n"
   "\t -L|--size [-]LogicalVolumeSize[bBsSkKmMgGtTpPeE]}\n"
   "\t[-n|--nofsck]\n"
   "\t[--noudevsync]\n"
   "\t[-r|--resizefs]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[-y|--yes]\n"
   "\tLogicalVolume[Path]\n",

   autobackup_ARG, force_ARG,  extents_ARG, nofsck_ARG, noudevsync_ARG,
   resizefs_ARG, size_ARG, test_ARG)

xx(lvremove,
   "Remove logical volume(s) from the system",
   ALL_VGS_IS_DEFAULT, /* all VGs only with --select */
   "lvremove\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[--noudevsync]\n"
   "\t[-S|--select Selection]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tLogicalVolume[Path] [LogicalVolume[Path]...]\n",

   autobackup_ARG, force_ARG, noudevsync_ARG, select_ARG, test_ARG)

xx(lvrename,
   "Rename a logical volume",
   0,
   "lvrename\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|-?|--help]\n"
   "\t[--noudevsync]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t{ OldLogicalVolumePath NewLogicalVolumePath |\n"
   "\t  VolumeGroupName OldLogicalVolumeName NewLogicalVolumeName }\n",

   autobackup_ARG, noudevsync_ARG, test_ARG)

xx(lvresize,
   "Resize a logical volume",
   0,
   "lvresize\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[-i|--stripes Stripes [-I|--stripesize StripeSize]]\n"
   "\t{-l|--extents [+|-]LogicalExtentsNumber[%{VG|LV|PVS|FREE|ORIGIN}] |\n"
   "\t -L|--size [+|-]LogicalVolumeSize[bBsSkKmMgGtTpPeE]}\n"
   "\t --poolmetadatasize [+]MetadataVolumeSize[bBsSkKmMgG]}\n"
   "\t[-n|--nofsck]\n"
   "\t[--noudevsync]\n"
   "\t[-r|--resizefs]\n"
   "\t[-t|--test]\n"
   "\t[--type VolumeType]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tLogicalVolume[Path] [ PhysicalVolumePath... ]\n",

   alloc_ARG, autobackup_ARG, extents_ARG, force_ARG, nofsck_ARG,
   noudevsync_ARG, resizefs_ARG, poolmetadatasize_ARG,
   size_ARG, stripes_ARG, stripesize_ARG,
   test_ARG, type_ARG)

xx(lvs,
   "Display information about logical volumes",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_FOREIGN_VGS,
   "lvs\n"
   "\t[-a|--all]\n"
   "\t[--aligned]\n"
   "\t[--binary]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--nameprefixes]\n"
   "\t[--noheadings]\n"
   "\t[--nosuffix]\n"
   "\t[-o|--options [+]Field[,Field]]\n"
   "\t[-O|--sort [+|-]key1[,[+|-]key2[,...]]]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[--rows]\n"
   "\t[--segments]\n"
   "\t[-S|--select Selection]\n"
   "\t[--separator Separator]\n"
   "\t[--trustcache]\n"
   "\t[--unbuffered]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[--unquoted]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[LogicalVolume[Path] [LogicalVolume[Path]...]]\n",

   aligned_ARG, all_ARG, binary_ARG, foreign_ARG, ignorelockingfailure_ARG,
   ignoreskippedcluster_ARG, nameprefixes_ARG, noheadings_ARG,
   nolocking_ARG, nosuffix_ARG, options_ARG, partial_ARG,
   readonly_ARG, rows_ARG, segments_ARG, select_ARG, separator_ARG,
   sort_ARG, trustcache_ARG, unbuffered_ARG, units_ARG, unquoted_ARG)

xx(lvscan,
   "List all logical volumes in all volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT,
   "lvscan\n"
   "\t[-a|--all]\n"
   "\t[-b|--blockdevice]\n"
   "\t[--cache]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|-?|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n",

   all_ARG, blockdevice_ARG, ignorelockingfailure_ARG, partial_ARG,
   readonly_ARG, cache_long_ARG)

xx(pvchange,
   "Change attributes of physical volume(s)",
   0,
   "pvchange\n"
   "\t[-a|--all]\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--metadataignore y|n]\n"
   "\t[-S|--select Selection]\n"
   "\t[-t|--test]\n"
   "\t[-u|--uuid]\n"
   "\t[-x|--allocatable y|n]\n"
   "\t[-v|--verbose]\n"
   "\t[--addtag Tag]\n"
   "\t[--deltag Tag]\n"
   "\t[--version]\n"
   "\t[PhysicalVolumePath...]\n",

   all_ARG, allocatable_ARG, allocation_ARG, autobackup_ARG, deltag_ARG,
   addtag_ARG, force_ARG, ignoreskippedcluster_ARG, metadataignore_ARG,
   select_ARG, test_ARG, uuid_ARG)

xx(pvresize,
   "Resize physical volume(s)",
   0,
   "pvresize\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|-?|--help]\n"
   "\t[--setphysicalvolumesize PhysicalVolumeSize[bBsSkKmMgGtTpPeE]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tPhysicalVolume [PhysicalVolume...]\n",

   physicalvolumesize_ARG, test_ARG)

xx(pvck,
   "Check the consistency of physical volume(s)",
   0,
   "pvck "
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[--labelsector sector]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tPhysicalVolume [PhysicalVolume...]\n",

   labelsector_ARG)

xx(pvcreate,
   "Initialize physical volume(s) for use by LVM",
   0,
   "pvcreate\n"
   "\t[--norestorefile]\n"
   "\t[--restorefile file]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f[f]|--force [--force]]\n"
   "\t[-h|-?|--help]\n"
   "\t[--labelsector sector]\n"
   "\t[-M|--metadatatype 1|2]\n"
   "\t[--pvmetadatacopies #copies]\n"
   "\t[--bootloaderareasize BootLoaderAreaSize[bBsSkKmMgGtTpPeE]]\n"
   "\t[--metadatasize MetadataSize[bBsSkKmMgGtTpPeE]]\n"
   "\t[--dataalignment Alignment[bBsSkKmMgGtTpPeE]]\n"
   "\t[--dataalignmentoffset AlignmentOffset[bBsSkKmMgGtTpPeE]]\n"
   "\t[--setphysicalvolumesize PhysicalVolumeSize[bBsSkKmMgGtTpPeE]\n"
   "\t[-t|--test]\n"
   "\t[-u|--uuid uuid]\n"
   "\t[-v|--verbose]\n"
   "\t[-y|--yes]\n"
   "\t[-Z|--zero {y|n}]\n"
   "\t[--version]\n"
   "\tPhysicalVolume [PhysicalVolume...]\n",

   dataalignment_ARG, dataalignmentoffset_ARG, bootloaderareasize_ARG,
   force_ARG, test_ARG, labelsector_ARG, metadatatype_ARG,
   metadatacopies_ARG, metadatasize_ARG, metadataignore_ARG,
   norestorefile_ARG, physicalvolumesize_ARG, pvmetadatacopies_ARG,
   restorefile_ARG, uuidstr_ARG, zero_ARG)

xx(pvdata,
   "Display the on-disk metadata for physical volume(s)",
   0,
   "pvdata\n"
   "\t[-a|--all]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-E|--physicalextent]\n"
   "\t[-h|-?|--help]\n"
   "\t[-L|--logicalvolume]\n"
   "\t[-P[P]|--physicalvolume [--physicalvolume]]\n"
   "\t[-U|--uuidlist]\n"
   "\t[-v[v]|--verbose [--verbose]]\n"
   "\t[-V|--volumegroup]\n"
   "\t[--version]\n"
   "\tPhysicalVolume [PhysicalVolume...]\n",

   all_ARG,  logicalextent_ARG, physicalextent_ARG,
   physicalvolume_ARG, uuidlist_ARG, volumegroup_ARG)

xx(pvdisplay,
   "Display various attributes of physical volume(s)",
   CACHE_VGMETADATA | PERMITTED_READ_ONLY | ENABLE_ALL_DEVS | ENABLE_FOREIGN_VGS,
   "pvdisplay\n"
   "\t[-c|--colon]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[-m|--maps]\n"
   "\t[--nosuffix]\n"
   "\t[--readonly]\n"
   "\t[-S|--select Selection]\n"
   "\t[-s|--short]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[PhysicalVolumePath [PhysicalVolumePath...]]\n"
   "\n"
   "pvdisplay --columns|-C\n"
   "\t[--aligned]\n"
   "\t[-a|--all]\n"
   "\t[--binary]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--noheadings]\n"
   "\t[--nosuffix]\n"
   "\t[-o|--options [+]Field[,Field]]\n"
   "\t[-O|--sort [+|-]key1[,[+|-]key2[,...]]]\n"
   "\t[-S|--select Selection]\n"
   "\t[--readonly]\n"
   "\t[--separator Separator]\n"
   "\t[--unbuffered]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[PhysicalVolumePath [PhysicalVolumePath...]]\n",

   aligned_ARG, all_ARG, binary_ARG, colon_ARG, columns_ARG, foreign_ARG,
   ignorelockingfailure_ARG, ignoreskippedcluster_ARG, maps_ARG,
   noheadings_ARG, nosuffix_ARG, options_ARG, readonly_ARG,
   select_ARG, separator_ARG, short_ARG, sort_ARG, unbuffered_ARG,
   units_ARG)

xx(pvmove,
   "Move extents from one physical volume to another",
   ALL_VGS_IS_DEFAULT,	/* For polldaemon to find pvmoves in-progress using process_each_vg. */
   "pvmove\n"
   "\t[--abort]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[--atomic]\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[-b|--background]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n "
   "\t[-h|-?|--help]\n"
   "\t[-i|--interval seconds]\n"
   "\t[--noudevsync]\n"
   "\t[-t|--test]\n "
   "\t[-v|--verbose]\n "
   "\t[--version]\n"
   "\t[{-n|--name} LogicalVolume]\n"
/* "\t[{-n|--name} LogicalVolume[:LogicalExtent[-LogicalExtent]...]]\n" */
   "\tSourcePhysicalVolume[:PhysicalExtent[-PhysicalExtent]...]}\n"
   "\t[DestinationPhysicalVolume[:PhysicalExtent[-PhysicalExtent]...]...]\n",

   abort_ARG, alloc_ARG, atomic_ARG, autobackup_ARG, background_ARG,
   interval_ARG, name_ARG, noudevsync_ARG, test_ARG)

xx(lvpoll,
   "Continue already initiated poll operation on a logical volume",
   0,
   "\t[--abort]\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n "
   "\t[-h|-?|--help]\n"
   "\t[--handlemissingpvs]\n"
   "\t[-i|--interval seconds]\n"
   "\t[--polloperation]\n"
   "\t[-t|--test]\n "
   "\t[-v|--verbose]\n "
   "\t[--version]\n",

   abort_ARG, autobackup_ARG, handlemissingpvs_ARG, interval_ARG, polloperation_ARG,
   test_ARG)

xx(pvremove,
   "Remove LVM label(s) from physical volume(s)",
   0,
   "pvremove\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f[f]|--force [--force]]\n"
   "\t[-h|-?|--help]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[-y|--yes]\n"
   "\tPhysicalVolume [PhysicalVolume...]\n",

   force_ARG, test_ARG)

xx(pvs,
   "Display information about physical volumes",
   CACHE_VGMETADATA | PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_ALL_DEVS | ENABLE_FOREIGN_VGS,
   "pvs\n"
   "\t[-a|--all]\n"
   "\t[--aligned]\n"
   "\t[--binary]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|-?|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--nameprefixes]\n"
   "\t[--noheadings]\n"
   "\t[--nosuffix]\n"
   "\t[-o|--options [+]Field[,Field]]\n"
   "\t[-O|--sort [+|-]key1[,[+|-]key2[,...]]]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[--rows]\n"
   "\t[--segments]\n"
   "\t[-S|--select Selection]\n"
   "\t[--separator Separator]\n"
   "\t[--trustcache]\n"
   "\t[--unbuffered]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[--unquoted]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[PhysicalVolume [PhysicalVolume...]]\n",

   aligned_ARG, all_ARG, binary_ARG, foreign_ARG, ignorelockingfailure_ARG,
   ignoreskippedcluster_ARG, nameprefixes_ARG, noheadings_ARG, nolocking_ARG,
   nosuffix_ARG, options_ARG, partial_ARG, readonly_ARG, rows_ARG,
   segments_ARG, select_ARG, separator_ARG, sort_ARG, trustcache_ARG,
   unbuffered_ARG, units_ARG, unquoted_ARG)

xx(pvscan,
   "List all physical volumes",
   PERMITTED_READ_ONLY | ENABLE_FOREIGN_VGS,
   "pvscan\n"
   "\t[-b|--background]\n"
   "\t[--cache [-a|--activate ay] [ DevicePath | -j|--major major --minor minor]...]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t{-e|--exported | -n|--novolumegroup}\n"
   "\t[-h|-?|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[-s|--short]\n"
   "\t[-u|--uuid]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n",

   activate_ARG, available_ARG, backgroundfork_ARG, cache_long_ARG,
   exported_ARG, ignorelockingfailure_ARG, major_ARG, minor_ARG,
   novolumegroup_ARG, partial_ARG, readonly_ARG, short_ARG, uuid_ARG)

xx(segtypes,
   "List available segment types",
   PERMITTED_READ_ONLY,
   "segtypes\n")

xx(systemid,
   "Display the system ID, if any, currently set on this host",
   PERMITTED_READ_ONLY,
   "systemid\n")

xx(tags,
   "List tags defined on this host",
   PERMITTED_READ_ONLY,
   "tags\n")

xx(vgcfgbackup,
   "Backup volume group configuration(s)",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_FOREIGN_VGS,
   "vgcfgbackup\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--file filename]\n"
   "\t[--foreign]\n"
   "\t[-h|-?|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[VolumeGroupName...]\n",

   file_ARG, foreign_ARG, ignorelockingfailure_ARG, partial_ARG, readonly_ARG)

xx(vgcfgrestore,
   "Restore volume group configuration",
   0,
   "vgcfgrestore\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--file filename]\n"
   "\t[--force]\n"
   "\t[-l[l]|--list [--list]]\n"
   "\t[-M|--metadatatype 1|2]\n"
   "\t[-h|--help]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tVolumeGroupName",

   file_ARG, force_long_ARG, list_ARG, metadatatype_ARG, test_ARG)

xx(vgchange,
   "Change volume group attributes",
   CACHE_VGMETADATA | PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT,
   "vgchange\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[-P|--partial]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--detachprofile]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoremonitoring]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[-K|--ignoreactivationskip]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[--monitor {y|n}]\n"
   "\t[--[vg]metadatacopies #copies]\n"
   "\t[--poll {y|n}]\n"
   "\t[--noudevsync]\n"
   "\t[--refresh]\n"
   "\t[-S|--select Selection]\n"
   "\t[--sysinit]\n"
   "\t[--systemid SystemID]\n"
   "\t[-t|--test]\n"
   "\t[-u|--uuid]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t{-a|--activate [a|e|l]{y|n}  |\n"
   "\t[--activationmode {complete|degraded|partial}]\n"
   "\t -c|--clustered {y|n} |\n"
   "\t -x|--resizeable {y|n} |\n"
   "\t -l|--logicalvolume MaxLogicalVolumes |\n"
   "\t -p|--maxphysicalvolumes MaxPhysicalVolumes |\n"
   "\t -s|--physicalextentsize PhysicalExtentSize[bBsSkKmMgGtTpPeE] |\n"
   "\t --addtag Tag |\n"
   "\t --deltag Tag}\n"
   "\t[VolumeGroupName...]\n",

   activationmode_ARG, addtag_ARG, alloc_ARG, allocation_ARG, autobackup_ARG,
   activate_ARG, available_ARG, clustered_ARG, deltag_ARG, detachprofile_ARG,
   ignoreactivationskip_ARG, ignorelockingfailure_ARG, ignoremonitoring_ARG,
   ignoreskippedcluster_ARG, logicalvolume_ARG, maxphysicalvolumes_ARG,
   metadataprofile_ARG, monitor_ARG, noudevsync_ARG, metadatacopies_ARG,
   vgmetadatacopies_ARG, partial_ARG, physicalextentsize_ARG, poll_ARG,
   refresh_ARG, resizeable_ARG, resizable_ARG, select_ARG, sysinit_ARG,
   systemid_ARG, test_ARG, uuid_ARG)

xx(vgck,
   "Check the consistency of volume group(s)",
   ALL_VGS_IS_DEFAULT,
   "vgck "
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[VolumeGroupName...]\n")

xx(vgconvert,
   "Change volume group metadata format",
   0,
   "vgconvert\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[--labelsector sector]\n"
   "\t[-M|--metadatatype 1|2]\n"
   "\t[--pvmetadatacopies #copies]\n"
   "\t[--metadatasize MetadataSize[bBsSkKmMgGtTpPeE]]\n"
   "\t[--bootloaderareasize BootLoaderAreaSize[bBsSkKmMgGtTpPeE]]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tVolumeGroupName [VolumeGroupName...]\n",

   force_ARG, test_ARG, labelsector_ARG, bootloaderareasize_ARG,
   metadatatype_ARG, metadatacopies_ARG, pvmetadatacopies_ARG,
   metadatasize_ARG)

xx(vgcreate,
   "Create a volume group",
   0,
   "vgcreate\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[--addtag Tag]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[-c|--clustered {y|n}]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-l|--maxlogicalvolumes MaxLogicalVolumes]\n"
   "\t[--metadataprofile ProfileName]\n"
   "\t[-M|--metadatatype 1|2]\n"
   "\t[--[vg]metadatacopies #copies]\n"
   "\t[-p|--maxphysicalvolumes MaxPhysicalVolumes]\n"
   "\t[-s|--physicalextentsize PhysicalExtentSize[bBsSkKmMgGtTpPeE]]\n"
   "\t[--systemid SystemID]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[-y|--yes]\n"
   "\t[ PHYSICAL DEVICE OPTIONS ]\n"
   "\tVolumeGroupName PhysicalDevicePath [PhysicalDevicePath...]\n",

   addtag_ARG, alloc_ARG, autobackup_ARG, clustered_ARG, maxlogicalvolumes_ARG,
   maxphysicalvolumes_ARG, metadataprofile_ARG, metadatatype_ARG,
   physicalextentsize_ARG, test_ARG, force_ARG, zero_ARG, labelsector_ARG,
   metadatasize_ARG, pvmetadatacopies_ARG, metadatacopies_ARG,
   vgmetadatacopies_ARG, dataalignment_ARG, dataalignmentoffset_ARG,
   systemid_ARG)

xx(vgdisplay,
   "Display volume group information",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_FOREIGN_VGS,
   "vgdisplay\n"
   "\t[-A|--activevolumegroups]\n"
   "\t[-c|--colon | -s|--short | -v|--verbose]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--nosuffix]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[-S|--select Selection]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[--version]\n"
   "\t[VolumeGroupName [VolumeGroupName...]]\n"
   "\n"
   "vgdisplay --columns|-C\n"
   "\t[--aligned]\n"
   "\t[--binary]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--noheadings]\n"
   "\t[--nosuffix]\n"
   "\t[-o|--options [+]Field[,Field]]\n"
   "\t[-O|--sort [+|-]key1[,[+|-]key2[,...]]]\n"
   "\t[-P|--partial]\n"
   "\t[-S|--select Selection]\n"
   "\t[--readonly]\n"
   "\t[--separator Separator]\n"
   "\t[--unbuffered]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[--verbose]\n"
   "\t[--version]\n"
   "\t[VolumeGroupName [VolumeGroupName...]]\n",

   activevolumegroups_ARG, aligned_ARG, binary_ARG, colon_ARG, columns_ARG,
   foreign_ARG, ignorelockingfailure_ARG, ignoreskippedcluster_ARG,
   noheadings_ARG, nosuffix_ARG, options_ARG, partial_ARG, readonly_ARG,
   select_ARG, short_ARG, separator_ARG, sort_ARG, unbuffered_ARG, units_ARG)

xx(vgexport,
   "Unregister volume group(s) from the system",
   ALL_VGS_IS_DEFAULT,
   "vgexport\n"
   "\t[-a|--all]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-S|--select Selection]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tVolumeGroupName [VolumeGroupName...]\n",

   all_ARG, select_ARG, test_ARG)

xx(vgextend,
   "Add physical volumes to a volume group",
   0,
   "vgextend\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--restoremissing]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[-y|--yes]\n"
   "\t[ PHYSICAL DEVICE OPTIONS ]\n"
   "\tVolumeGroupName PhysicalDevicePath [PhysicalDevicePath...]\n",

   autobackup_ARG, test_ARG,
   force_ARG, zero_ARG, labelsector_ARG, metadatatype_ARG,
   metadatasize_ARG, pvmetadatacopies_ARG, metadatacopies_ARG,
   metadataignore_ARG, dataalignment_ARG, dataalignmentoffset_ARG,
   restoremissing_ARG)

xx(vgimport,
   "Register exported volume group with system",
   ALL_VGS_IS_DEFAULT,
   "vgimport\n"
   "\t[-a|--all]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[-S|--select Selection]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tVolumeGroupName...\n",

   all_ARG, force_ARG, select_ARG, test_ARG)

xx(vgmerge,
   "Merge volume groups",
   0,
   "vgmerge\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-l|--list]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tDestinationVolumeGroupName SourceVolumeGroupName\n",

   autobackup_ARG, list_ARG, test_ARG)

xx(vgmknodes,
   "Create the special files for volume group devices in /dev",
   ALL_VGS_IS_DEFAULT,
   "vgmknodes\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--refresh]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[VolumeGroupName...]\n",

   ignorelockingfailure_ARG, refresh_ARG)

xx(vgreduce,
   "Remove physical volume(s) from a volume group",
   0,
   "vgreduce\n"
   "\t[-a|--all]\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[--mirrorsonly]\n"
   "\t[--removemissing]\n"
   "\t[-f|--force]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tVolumeGroupName\n"
   "\t[PhysicalVolumePath...]\n",

   all_ARG, autobackup_ARG, force_ARG, mirrorsonly_ARG, removemissing_ARG,
   test_ARG)

xx(vgremove,
   "Remove volume group(s)",
   ALL_VGS_IS_DEFAULT, /* all VGs only with select */
   "vgremove\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-f|--force]\n"
   "\t[-h|--help]\n"
   "\t[--noudevsync]\n"
   "\t[-S|--select Selection]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tVolumeGroupName [VolumeGroupName...]\n",

   force_ARG, noudevsync_ARG, select_ARG, test_ARG)

xx(vgrename,
   "Rename a volume group",
   0,
   "vgrename\n"
   "\t[-A|--autobackup y|n]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tOldVolumeGroupPath NewVolumeGroupPath |\n"
   "\tOldVolumeGroupName NewVolumeGroupName\n",

   autobackup_ARG, force_ARG, test_ARG)

xx(vgs,
   "Display information about volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_FOREIGN_VGS,
   "vgs\n"
   "\t[--aligned]\n"
   "\t[--binary]\n"
   "\t[-a|--all]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[--foreign]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--ignoreskippedcluster]\n"
   "\t[--nameprefixes]\n"
   "\t[--noheadings]\n"
   "\t[--nosuffix]\n"
   "\t[-o|--options [+]Field[,Field]]\n"
   "\t[-O|--sort [+|-]key1[,[+|-]key2[,...]]]\n"
   "\t[-P|--partial]\n"
   "\t[--readonly]\n"
   "\t[--rows]\n"
   "\t[-S|--select Selection]\n"
   "\t[--separator Separator]\n"
   "\t[--trustcache]\n"
   "\t[--unbuffered]\n"
   "\t[--units hHbBsSkKmMgGtTpPeE]\n"
   "\t[--unquoted]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\t[VolumeGroupName [VolumeGroupName...]]\n",

   aligned_ARG, all_ARG, binary_ARG, foreign_ARG, ignorelockingfailure_ARG,
   ignoreskippedcluster_ARG, nameprefixes_ARG, noheadings_ARG,
   nolocking_ARG, nosuffix_ARG, options_ARG, partial_ARG,
   readonly_ARG, rows_ARG, select_ARG, separator_ARG, sort_ARG,
   trustcache_ARG, unbuffered_ARG, units_ARG, unquoted_ARG)

xx(vgscan,
   "Search for all volume groups",
   PERMITTED_READ_ONLY | ALL_VGS_IS_DEFAULT | ENABLE_FOREIGN_VGS,
   "vgscan "
   "\t[--cache]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[--ignorelockingfailure]\n"
   "\t[--mknodes]\n"
   "\t[-P|--partial]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n",

   cache_long_ARG, ignorelockingfailure_ARG, mknodes_ARG, partial_ARG)

xx(vgsplit,
   "Move physical volumes into a new or existing volume group",
   0,
   "vgsplit\n"
   "\t[-A|--autobackup {y|n}]\n"
   "\t[--alloc AllocationPolicy]\n"
   "\t[-c|--clustered {y|n}]\n"
   "\t[--commandprofile ProfileName]\n"
   "\t[-d|--debug]\n"
   "\t[-h|--help]\n"
   "\t[-l|--maxlogicalvolumes MaxLogicalVolumes]\n"
   "\t[-M|--metadatatype 1|2]\n"
   "\t[--[vg]metadatacopies #copies]\n"
   "\t[-n|--name LogicalVolumeName]\n"
   "\t[-p|--maxphysicalvolumes MaxPhysicalVolumes]\n"
   "\t[-t|--test]\n"
   "\t[-v|--verbose]\n"
   "\t[--version]\n"
   "\tSourceVolumeGroupName DestinationVolumeGroupName\n"
   "\t[PhysicalVolumePath...]\n",

   alloc_ARG, autobackup_ARG, clustered_ARG,
   maxlogicalvolumes_ARG, maxphysicalvolumes_ARG,
   metadatatype_ARG, vgmetadatacopies_ARG, name_ARG, test_ARG)

xx(version,
   "Display software and driver version information",
   PERMITTED_READ_ONLY,
   "version\n")
