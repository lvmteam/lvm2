/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2007 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "tools.h"
#include "lib/format_text/format-text.h"

/*
 * TODO: option to dump all copies of metadata that are found
 *
 * TODO: option to intelligently search for mda locations on
 * disk in case the pv_header and/or mda_header are damaged.
 */

static int _dump_metadata(struct cmd_context *cmd, int argc, char **argv, int full_area)
{
	struct dm_list devs;
	struct device_list *devl;
	struct device *dev;
	const char *pv_name;
	const char *vgname;
	const char *vgid;
	struct lvmcache_info *info;
	struct metadata_area *mda;
	const char *tofile = NULL;
	int mda_num = 1;
	int ret;

	dm_list_init(&devs);

	if (arg_is_set(cmd, file_ARG)) {
		if (!(tofile = arg_str_value(cmd, file_ARG, NULL)))
			return ECMD_FAILED;
	}

	/* 1: dump metadata from first mda, 2: dump metadata from second mda */
	if (arg_is_set(cmd, pvmetadatacopies_ARG))
		mda_num = arg_int_value(cmd, pvmetadatacopies_ARG, 1);

	pv_name = argv[0];

	if (!(dev = dev_cache_get(cmd, pv_name, cmd->filter))) {
		log_error("No device found for %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
		return ECMD_FAILED;
	}

	if (!(devl = zalloc(sizeof(*devl))))
		return ECMD_FAILED;

	devl->dev = dev;
	dm_list_add(&devs, &devl->list);

	label_scan_setup_bcache();
	label_scan_devs(cmd, cmd->filter, &devs);

	if (!dev->pvid[0]) {
		log_error("No PV ID found for %s", dev_name(dev));
		return ECMD_FAILED;
	}

	if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
		log_error("No VG info found for %s", dev_name(dev));
		return ECMD_FAILED;
	}

	if (!(vgname = lvmcache_vgname_from_info(info))) {
		log_error("No VG name found for %s", dev_name(dev));
		return ECMD_FAILED;
	}

        if (!(vgid = lvmcache_vgid_from_vgname(cmd, vgname))) {
		log_error("No VG ID found for %s", dev_name(dev));
		return ECMD_FAILED;
        }

	if (!(mda = lvmcache_get_mda(cmd, vgname, dev, mda_num))) {
		log_error("No mda %d found for %s", mda_num, dev_name(dev));
		return ECMD_FAILED;
	}

	if (full_area)
		ret = dump_metadata_area(cmd, vgname, vgid, dev, mda, tofile);
	else
		ret = dump_metadata_text(cmd, vgname, vgid, dev, mda, tofile);

	if (!ret)
		return ECMD_FAILED;
	return ECMD_PROCESSED;
}

int pvck(struct cmd_context *cmd, int argc, char **argv)
{
	struct dm_list devs;
	struct device_list *devl;
	struct device *dev;
	const char *dump;
	const char *pv_name;
	uint64_t labelsector;
	int i;
	int ret_max = ECMD_PROCESSED;

	if (arg_is_set(cmd, dump_ARG)) {
		dump = arg_str_value(cmd, dump_ARG, NULL);

		if (!strcmp(dump, "metadata"))
			return _dump_metadata(cmd, argc, argv, 0);

		if (!strcmp(dump, "metadata_area"))
			return _dump_metadata(cmd, argc, argv, 1);

		log_error("Unknown dump value.");
		return ECMD_FAILED;
	}

	labelsector = arg_uint64_value(cmd, labelsector_ARG, UINT64_C(0));

	dm_list_init(&devs);

	for (i = 0; i < argc; i++) {
		dm_unescape_colons_and_at_signs(argv[i], NULL, NULL);

		pv_name = argv[i];

		dev = dev_cache_get(cmd, pv_name, cmd->filter);

		if (!dev) {
			log_error("Device %s %s.", pv_name, dev_cache_filtered_reason(pv_name));
			continue;
		}

		if (!(devl = zalloc(sizeof(*devl))))
			continue;

		devl->dev = dev;
		dm_list_add(&devs, &devl->list);
	}

	label_scan_setup_bcache();
	label_scan_devs(cmd, cmd->filter, &devs);

	dm_list_iterate_items(devl, &devs) {
		/*
		 * The scan above will populate lvmcache with any info from the
		 * standard locations at the start of the device.  Now populate
		 * lvmcache with any info from non-standard offsets.
		 *
		 * FIXME: is it possible for a real lvm label sector to be
		 * anywhere other than the first four sectors of the disk?
		 * If not, drop the code in label_read_sector/find_lvm_header
		 * that supports searching at any sector.
		 */
		if (labelsector) {
			if (!label_read_sector(devl->dev, labelsector)) {
				stack;
				ret_max = ECMD_FAILED;
				continue;
			}
		}

		if (!pv_analyze(cmd, devl->dev, labelsector)) {
			stack;
			ret_max = ECMD_FAILED;
		}
	}

	return ret_max;
}
