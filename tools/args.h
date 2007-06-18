/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Put all long args that don't have a
 * corresponding short option first ...
 */
/* *INDENT-OFF* */
arg(version_ARG, '\0', "version", NULL)
arg(quiet_ARG, '\0', "quiet", NULL)
arg(physicalvolumesize_ARG, '\0', "setphysicalvolumesize", size_mb_arg)
arg(ignorelockingfailure_ARG, '\0', "ignorelockingfailure", NULL)
arg(nolocking_ARG, '\0', "nolocking", NULL)
arg(metadatacopies_ARG, '\0', "metadatacopies", int_arg)
arg(metadatasize_ARG, '\0', "metadatasize", size_mb_arg)
arg(restorefile_ARG, '\0', "restorefile", string_arg)
arg(labelsector_ARG, '\0', "labelsector", int_arg)
arg(driverloaded_ARG, '\0', "driverloaded", yes_no_arg)
arg(aligned_ARG, '\0', "aligned", NULL)
arg(unbuffered_ARG, '\0', "unbuffered", NULL)
arg(noheadings_ARG, '\0', "noheadings", NULL)
arg(segments_ARG, '\0', "segments", NULL)
arg(units_ARG, '\0', "units", string_arg)
arg(nosuffix_ARG, '\0', "nosuffix", NULL)
arg(removemissing_ARG, '\0', "removemissing", NULL)
arg(abort_ARG, '\0', "abort", NULL)
arg(addtag_ARG, '\0', "addtag", tag_arg)
arg(deltag_ARG, '\0', "deltag", tag_arg)
arg(refresh_ARG, '\0', "refresh", NULL)
arg(mknodes_ARG, '\0', "mknodes", NULL)
arg(minor_ARG, '\0', "minor", minor_arg)
arg(type_ARG, '\0', "type", segtype_arg)
arg(alloc_ARG, '\0', "alloc", alloc_arg)
arg(separator_ARG, '\0', "separator", string_arg)
arg(mirrorsonly_ARG, '\0', "mirrorsonly", NULL)
arg(nosync_ARG, '\0', "nosync", NULL)
arg(resync_ARG, '\0', "resync", NULL)
arg(corelog_ARG, '\0', "corelog", NULL)
arg(monitor_ARG, '\0', "monitor", yes_no_arg)
arg(config_ARG, '\0', "config", string_arg)
arg(trustcache_ARG, '\0', "trustcache", NULL)
arg(ignoremonitoring_ARG, '\0', "ignoremonitoring", NULL)

/* Allow some variations */
arg(resizable_ARG, '\0', "resizable", yes_no_arg)
arg(allocation_ARG, '\0', "allocation", yes_no_arg)

/*
 * ... and now the short args.
 */
arg(available_ARG, 'a', "available", yes_no_excl_arg)
arg(all_ARG, 'a', "all", NULL)
arg(autobackup_ARG, 'A', "autobackup", yes_no_arg)
arg(activevolumegroups_ARG, 'A', "activevolumegroups", NULL)
arg(background_ARG, 'b', "background", NULL)
arg(blockdevice_ARG, 'b', "blockdevice", NULL)
arg(chunksize_ARG, 'c', "chunksize", size_kb_arg)
arg(clustered_ARG, 'c', "clustered", yes_no_arg)
arg(colon_ARG, 'c', "colon", NULL)
arg(columns_ARG, 'C', "columns", NULL)
arg(contiguous_ARG, 'C', "contiguous", yes_no_arg)
arg(debug_ARG, 'd', "debug", NULL)
arg(disk_ARG, 'D', "disk", NULL)
arg(exported_ARG, 'e', "exported", NULL)
arg(physicalextent_ARG, 'E', "physicalextent", NULL)
arg(file_ARG, 'f', "file", string_arg)
arg(force_ARG, 'f', "force", NULL)
arg(full_ARG, 'f', "full", NULL)
arg(help_ARG, 'h', "help", NULL)
arg(help2_ARG, '?', "", NULL)
arg(stripesize_ARG, 'I', "stripesize", size_kb_arg)
arg(stripes_ARG, 'i', "stripes", int_arg)
arg(interval_ARG, 'i', "interval", int_arg)
arg(iop_version_ARG, 'i', "iop_version", NULL)
arg(logicalvolume_ARG, 'l', "logicalvolume", int_arg)
arg(maxlogicalvolumes_ARG, 'l', "maxlogicalvolumes", int_arg)
arg(extents_ARG, 'l', "extents", int_arg_with_sign_and_percent)
arg(lvmpartition_ARG, 'l', "lvmpartition", NULL)
arg(list_ARG, 'l', "list", NULL)
arg(size_ARG, 'L', "size", size_mb_arg)
arg(logicalextent_ARG, 'L', "logicalextent", int_arg_with_sign)
arg(persistent_ARG, 'M', "persistent", yes_no_arg)
arg(major_ARG, 'j', "major", major_arg)
arg(mirrors_ARG, 'm', "mirrors", int_arg_with_sign)
arg(metadatatype_ARG, 'M', "metadatatype", metadatatype_arg)
arg(maps_ARG, 'm', "maps", NULL)
arg(name_ARG, 'n', "name", string_arg)
arg(oldpath_ARG, 'n', "oldpath", NULL)
arg(nofsck_ARG, 'n', "nofsck", NULL)
arg(novolumegroup_ARG, 'n', "novolumegroup", NULL)
arg(options_ARG, 'o', "options", string_arg)
arg(sort_ARG, 'O', "sort", string_arg)
arg(permission_ARG, 'p', "permission", permission_arg)
arg(maxphysicalvolumes_ARG, 'p', "maxphysicalvolumes", int_arg)
arg(partial_ARG, 'P', "partial", NULL)
arg(physicalvolume_ARG, 'P', "physicalvolume", NULL)
arg(readahead_ARG, 'r', "readahead", int_arg)
arg(resizefs_ARG, 'r', "resizefs", NULL)
arg(reset_ARG, 'R', "reset", NULL)
arg(regionsize_ARG, 'R', "regionsize", size_mb_arg)
arg(physicalextentsize_ARG, 's', "physicalextentsize", size_mb_arg)
arg(stdin_ARG, 's', "stdin", NULL)
arg(snapshot_ARG, 's', "snapshot", NULL)
arg(short_ARG, 's', "short", NULL)
arg(test_ARG, 't', "test", NULL)
arg(uuid_ARG, 'u', "uuid", NULL)
arg(uuidstr_ARG, 'u', "uuid", string_arg)
arg(uuidlist_ARG, 'U', "uuidlist", NULL)
arg(verbose_ARG, 'v', "verbose", NULL)
arg(volumegroup_ARG, 'V', "volumegroup", NULL)
arg(allocatable_ARG, 'x', "allocatable", yes_no_arg)
arg(resizeable_ARG, 'x', "resizeable", yes_no_arg)
arg(yes_ARG, 'y', "yes", NULL)
arg(zero_ARG, 'Z', "zero", yes_no_arg)

/* this should always be last */
arg(ARG_COUNT, '-', "", NULL)
/* *INDENT-ON* */
