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

xx(available_ARG, 'a', "available", yes_no_arg)
xx(all_ARG, 'a', "all", yes_no_arg)
xx(autobackup_ARG, 'A', "autobackup", yes_no_arg)
xx(activevolumegroups_ARG, 'A', "activevolumegroups", NULL)
xx(blockdevice_ARG, 'b', "blockdevice", NULL)
xx(chunksize_ARG, 'c', "chunksize", size_arg)
xx(colon_ARG, 'c', "colon", NULL)
xx(contiguous_ARG, 'C', "contiguous", yes_no_arg)
xx(debug_ARG, 'd', "debug", NULL)
xx(disk_ARG, 'D', "disk", NULL)
xx(exported_ARG, 'e', "exported", NULL)
xx(physicalextent_ARG, 'E', "physicalextent", NULL)
xx(file_ARG, 'f', "file", NULL)
xx(force_ARG, 'f', "force", NULL)
xx(full_ARG, 'f', "full", NULL)
xx(help_ARG, 'h', "help", NULL)
xx(stripesize_ARG, 'I', "stripesize", size_arg)
xx(stripes_ARG, 'i', "stripes", int_arg)
xx(iop_version_ARG, 'i', "iop_version", NULL)
xx(logicalvolume_ARG, 'l', "logicalvolume", int_arg)
xx(maxlogicalvolumes_ARG, 'l', "maxlogicalvolumes", int_arg)
xx(extents_ARG, 'l', "extents", int_arg)
xx(lvmpartition_ARG, 'l', "lvmpartition", NULL)
xx(list_ARG, 'l', "list", NULL)
xx(size_ARG, 'L', "size", size_arg)
xx(logicalextent_ARG, 'L', "logicalextent", int_arg)
xx(name_ARG, 'n', "name", string_arg)
xx(oldpath_ARG, 'n', "oldpath", NULL)
xx(nofsck_ARG, 'n', "nofsck", NULL)
xx(novolumegroup_ARG, 'n', "novolumegroup", NULL)
xx(permission_ARG, 'p', "permission", permission_arg)
xx(maxphysicalvolumes_ARG, 'p', "maxphysicalvolumes", int_arg)
xx(physicalvolume_ARG, 'P', "physicalvolume", NULL)
xx(readahead_ARG, 'r', "readahead", int_arg)
xx(reset_ARG, 'R', "reset", NULL)
xx(physicalextentsize_ARG, 's', "physicalextentsize", size_arg)
xx(stdin_ARG, 's', "stdin", NULL)
xx(snapshot_ARG, 's', "snapshot", NULL)
xx(short_ARG, 's', "short", NULL)
xx(test_ARG, 't', "test", NULL)
xx(uuid_ARG, 'u', "uuid", NULL)
xx(uuidlist_ARG, 'U', "uuidlist", NULL)
xx(verbose_ARG, 'v', "verbose", NULL)
xx(volumegroup_ARG, 'V', "volumegroup", NULL)
xx(version_ARG, '\0', "version", NULL)
xx(allocation_ARG, 'x', "allocation", yes_no_arg)
xx(yes_ARG, 'y', "yes", NULL)
xx(zero_ARG, 'Z', "zero", yes_no_arg)
xx(suspend_ARG, 'z', "suspend", NULL)

/* this should always be last */
xx(ARG_COUNT, '-', "", NULL)

