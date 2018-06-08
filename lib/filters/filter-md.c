/*
 * Copyright (C) 2004 Luca Berra
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
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
#include "lib/misc/lib.h"
#include "lib/filters/filter.h"

#ifdef __linux__

#define MSG_SKIPPING "%s: Skipping md component device"

/*
 * The purpose of these functions is to ignore md component devices,
 * e.g. if /dev/md0 is a raid1 composed of /dev/loop0 and /dev/loop1,
 * lvm wants to deal with md0 and ignore loop0 and loop1.  md0 should
 * pass the filter, and loop0,loop1 should not pass the filter so lvm
 * will ignore them.
 *
 * (This is assuming lvm.conf md_component_detection=1.)
 *
 * If lvm does *not* ignore the components, then lvm will read lvm
 * labels from the md dev and from the component devs, and will see
 * them all as duplicates of each other.  LVM duplicate resolution
 * will then kick in and keep the md dev around to use and ignore
 * the components.
 *
 * It is better to exclude the components as early as possible during
 * lvm processing, ideally before lvm even looks for labels on the
 * components, so that duplicate resolution can be avoided.  There are
 * a number of ways that md components can be excluded earlier than
 * the duplicate resolution phase:
 *
 * - When external_device_info_source="udev", lvm discovers a device is
 *   an md component by asking udev during the initial filtering phase.
 *   However, lvm's default is to not use udev for this.  The
 *   alternative is "native" detection in which lvm tries to detect
 *   md components itself.
 *
 * - When using native detection, lvm's md filter looks for the md
 *   superblock at the start of devices.  It will see the md superblock
 *   on the components, exclude them in the md filter, and avoid
 *   handling them later in duplicate resolution.
 *
 * - When using native detection, lvm's md filter will not detect
 *   components when the md device has an older superblock version that
 *   places the superblock at the end of the device.  This case will
 *   fall back to duplicate resolution to exclude components.
 *
 * A variation of the description above occurs for lvm commands that
 * intend to create new PVs on devices (pvcreate, vgcreate, vgextend).
 * For these commands, the native md filter also reads the end of all
 * devices to check for the odd md superblocks.
 *
 * (The reason that external_device_info_source is not set to udev by
 * default is that there have be issues with udev not being promptly
 * or reliably updated about md state changes, causing the udev info
 * that lvm uses to be occasionally wrong.)
 */

/*
 * Returns 0 if:
 * the device is an md component and it should be ignored.
 *
 * Returns 1 if:
 * the device is not md component and should not be ignored.
 *
 * The actual md device will pass this filter and should be used,
 * it is the md component devices that we are trying to exclude
 * that will not pass.
 */

static int _passes_md_filter(struct device *dev, int full)
{
	int ret;

	/*
	 * When md_component_dectection=0, don't even try to skip md
	 * components.
	 */
	if (!md_filtering())
		return 1;

	ret = dev_is_md(dev, NULL, full);

	if (ret == -EAGAIN) {
		/* let pass, call again after scan */
		dev->flags |= DEV_FILTER_AFTER_SCAN;
		log_debug_devs("filter md deferred %s", dev_name(dev));
		return 1;
	}

	if (ret == 0)
		return 1;

	if (ret == 1) {
		if (dev->ext.src == DEV_EXT_NONE)
			log_debug_devs(MSG_SKIPPING, dev_name(dev));
		else
			log_debug_devs(MSG_SKIPPING " [%s:%p]", dev_name(dev),
					dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	if (ret < 0) {
		log_debug_devs("%s: Skipping: error in md component detection",
			       dev_name(dev));
		return 0;
	}

	return 1;
}

static int _passes_md_filter_lite(struct dev_filter *f __attribute__((unused)),
				  struct device *dev)
{
	return _passes_md_filter(dev, 0);
}

static int _passes_md_filter_full(struct dev_filter *f __attribute__((unused)),
				  struct device *dev)
{
	return _passes_md_filter(dev, 1);
}

static void _destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying md filter while in use %u times.", f->use_count);

	free(f);
}

struct dev_filter *md_filter_create(struct cmd_context *cmd, struct dev_types *dt)
{
	struct dev_filter *f;

	if (!(f = zalloc(sizeof(*f)))) {
		log_error("md filter allocation failed");
		return NULL;
	}

	/*
	 * FIXME: for commands that want a full md check (pvcreate, vgcreate,
	 * vgextend), we do an extra read at the end of every device that the
	 * filter looks at.  This isn't necessary; we only need to do the full
	 * md check on the PVs that these commands are trying to use.
	 */

	if (cmd->use_full_md_check)
		f->passes_filter = _passes_md_filter_full;
	else
		f->passes_filter = _passes_md_filter_lite;

	f->destroy = _destroy;
	f->use_count = 0;
	f->private = dt;

	log_debug_devs("MD filter initialised.");

	return f;
}

#else

struct dev_filter *md_filter_create(struct dev_types *dt)
{
	return NULL;
}

#endif
