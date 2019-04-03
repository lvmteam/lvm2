/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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
#include "lib/activate/activate.h"
#ifdef UDEV_SYNC_SUPPORT
#include <libudev.h>
#include "lib/device/dev-ext-udev-constants.h"
#endif

#ifdef __linux__

#include <dirent.h>

#define MPATH_PREFIX "mpath-"


struct mpath_priv {
	struct dm_pool *mem;
	struct dev_filter f;
	struct dev_types *dt;
	struct dm_hash_table *hash;
};

static const char *_get_sysfs_name(struct device *dev)
{
	const char *name;

	if (!(name = strrchr(dev_name(dev), '/'))) {
		log_error("Cannot find '/' in device name.");
		return NULL;
	}
	name++;

	if (!*name) {
		log_error("Device name is not valid.");
		return NULL;
	}

	return name;
}

static const char *_get_sysfs_name_by_devt(const char *sysfs_dir, dev_t devno,
					  char *buf, size_t buf_size)
{
	const char *name;
	char path[PATH_MAX];
	int size;

	if (dm_snprintf(path, sizeof(path), "%s/dev/block/%d:%d", sysfs_dir,
			(int) MAJOR(devno), (int) MINOR(devno)) < 0) {
		log_error("Sysfs path string is too long.");
		return NULL;
	}

	if ((size = readlink(path, buf, buf_size - 1)) < 0) {
		log_sys_error("readlink", path);
		return NULL;
	}
	buf[size] = '\0';

	if (!(name = strrchr(buf, '/'))) {
		log_error("Cannot find device name in sysfs path.");
		return NULL;
	}
	name++;

	return name;
}

static int _get_sysfs_string(const char *path, char *buffer, int max_size)
{
	FILE *fp;
	int r = 0;

	if (!(fp = fopen(path, "r"))) {
		log_sys_error("fopen", path);
		return 0;
	}

	if (!fgets(buffer, max_size, fp))
		log_sys_error("fgets", path);
	else
		r = 1;

	if (fclose(fp))
		log_sys_error("fclose", path);

	return r;
}

static int _get_sysfs_get_major_minor(const char *sysfs_dir, const char *kname, int *major, int *minor)
{
	char path[PATH_MAX], buffer[64];

	if (dm_snprintf(path, sizeof(path), "%s/block/%s/dev", sysfs_dir, kname) < 0) {
		log_error("Sysfs path string is too long.");
		return 0;
	}

	if (!_get_sysfs_string(path, buffer, sizeof(buffer)))
		return_0;

	if (sscanf(buffer, "%d:%d", major, minor) != 2) {
		log_error("Failed to parse major minor from %s", buffer);
		return 0;
	}

	return 1;
}

static int _get_parent_mpath(const char *dir, char *name, int max_size)
{
	struct dirent *d;
	DIR *dr;
	int r = 0;

	if (!(dr = opendir(dir))) {
		log_sys_error("opendir", dir);
		return 0;
	}

	*name = '\0';
	while ((d = readdir(dr))) {
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		/* There should be only one holder if it is multipath */
		if (*name) {
			r = 0;
			break;
		}

		strncpy(name, d->d_name, max_size);
		r = 1;
	}

	if (closedir(dr))
		log_sys_error("closedir", dir);

	return r;
}

#ifdef UDEV_SYNC_SUPPORT
static int _udev_dev_is_mpath(struct device *dev)
{
	const char *value;
	struct dev_ext *ext;

	if (!(ext = dev_ext_get(dev)))
		return_0;

	value = udev_device_get_property_value((struct udev_device *)ext->handle, DEV_EXT_UDEV_BLKID_TYPE);
	if (value && !strcmp(value, DEV_EXT_UDEV_BLKID_TYPE_MPATH))
		return 1;

	value = udev_device_get_property_value((struct udev_device *)ext->handle, DEV_EXT_UDEV_MPATH_DEVICE_PATH);
	if (value && !strcmp(value, "1"))
		return 1;

	return 0;
}
#else
static int _udev_dev_is_mpath(struct device *dev)
{
	return 0;
}
#endif

static int _native_dev_is_mpath(struct dev_filter *f, struct device *dev)
{
	struct mpath_priv *mp = (struct mpath_priv *) f->private;
	struct dev_types *dt = mp->dt;
	const char *part_name, *name;
	struct stat info;
	char path[PATH_MAX], parent_name[PATH_MAX];
	const char *sysfs_dir = dm_sysfs_dir();
	int major = MAJOR(dev->dev);
	int minor = MINOR(dev->dev);
	dev_t primary_dev;
	long look;

	/* Limit this filter only to SCSI devices */
	if (!major_is_scsi_device(dt, MAJOR(dev->dev)))
		return 0;

	switch (dev_get_primary_dev(dt, dev, &primary_dev)) {
	case 2: /* The dev is partition. */
		part_name = dev_name(dev); /* name of original dev for log_debug msg */
		if (!(name = _get_sysfs_name_by_devt(sysfs_dir, primary_dev, parent_name, sizeof(parent_name))))
			return_0;
		log_debug_devs("%s: Device is a partition, using primary "
			       "device %s for mpath component detection",
			       part_name, name);
		break;
	case 1: /* The dev is already a primary dev. Just continue with the dev. */
		if (!(name = _get_sysfs_name(dev)))
			return_0;
		break;
	default: /* 0, error. */
		log_warn("Failed to get primary device for %d:%d.", major, minor);
		return 0;
	}

	if (dm_snprintf(path, sizeof(path), "%s/block/%s/holders", sysfs_dir, name) < 0) {
		log_warn("Sysfs path to check mpath is too long.");
		return 0;
	}

	/* also will filter out partitions */
	if (stat(path, &info))
		return 0;

	if (!S_ISDIR(info.st_mode)) {
		log_warn("Path %s is not a directory.", path);
		return 0;
	}

	if (!_get_parent_mpath(path, parent_name, sizeof(parent_name)))
		return 0;

	if (!_get_sysfs_get_major_minor(sysfs_dir, parent_name, &major, &minor))
		return_0;

	if (major != dt->device_mapper_major)
		return 0;

	/* Avoid repeated detection of multipath device and use first checked result */
	look = (long) dm_hash_lookup_binary(mp->hash, &minor, sizeof(minor));
	if (look > 0) {
		log_debug_devs("%s(%u:%u): already checked as %sbeing mpath.",
			       parent_name, major, minor, (look > 1) ? "" : "not ");
		return (look > 1) ? 1 : 0;
	}

	if (lvm_dm_prefix_check(major, minor, MPATH_PREFIX)) {
		(void) dm_hash_insert_binary(mp->hash, &minor, sizeof(minor), (void*)2);
		return 1;
	}

	(void) dm_hash_insert_binary(mp->hash, &minor, sizeof(minor), (void*)1);

	return 0;
}

static int _dev_is_mpath(struct dev_filter *f, struct device *dev)
{
	if (dev->ext.src == DEV_EXT_NONE)
		return _native_dev_is_mpath(f, dev);

	if (dev->ext.src == DEV_EXT_UDEV)
		return _udev_dev_is_mpath(dev);

	log_error(INTERNAL_ERROR "Missing hook for mpath recognition "
		  "using external device info source %s", dev_ext_name(dev));

	return 0;
}

#define MSG_SKIPPING "%s: Skipping mpath component device"

static int _ignore_mpath(struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name)
{
	if (_dev_is_mpath(f, dev) == 1) {
		if (dev->ext.src == DEV_EXT_NONE)
			log_debug_devs(MSG_SKIPPING, dev_name(dev));
		else
			log_debug_devs(MSG_SKIPPING " [%s:%p]", dev_name(dev),
					dev_ext_name(dev), dev->ext.handle);
		return 0;
	}

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	struct mpath_priv *mp = (struct mpath_priv*) f->private;

	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying mpath filter while in use %u times.", f->use_count);

	dm_hash_destroy(mp->hash);
	dm_pool_destroy(mp->mem);
}

struct dev_filter *mpath_filter_create(struct dev_types *dt)
{
	const char *sysfs_dir = dm_sysfs_dir();
	struct dm_pool *mem;
	struct mpath_priv *mp;
	struct dm_hash_table *hash;

	if (!*sysfs_dir) {
		log_verbose("No proc filesystem found: skipping multipath filter");
		return NULL;
	}

	if (!(hash = dm_hash_create(128))) {
		log_error("mpath hash table creation failed.");
		return NULL;
	}

	if (!(mem = dm_pool_create("mpath", 256))) {
		log_error("mpath pool creation failed.");
		dm_hash_destroy(hash);
		return NULL;
	}

	if (!(mp = dm_pool_zalloc(mem, sizeof(*mp)))) {
		log_error("mpath filter allocation failed.");
		goto bad;
	}

	if (!(mp = dm_pool_zalloc(mem, sizeof(*mp)))) {
		log_error("mpath filter allocation failed.");
		goto bad;
	}

	mp->f.passes_filter = _ignore_mpath;
	mp->f.destroy = _destroy;
	mp->f.use_count = 0;
	mp->f.private = mp;
	mp->f.name = "mpath";

	mp->mem = mem;
	mp->dt = dt;
	mp->hash = hash;

	log_debug_devs("mpath filter initialised.");

	return &mp->f;
bad:
	dm_pool_destroy(mem);
	dm_hash_destroy(hash);
	return NULL;
}

#else

struct dev_filter *mpath_filter_create(struct dev_types *dt)
{
	return NULL;
}

#endif
