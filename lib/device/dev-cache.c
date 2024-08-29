/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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
#include "base/data-struct/radix-tree.h"
#include "lib/misc/lib.h"
#include "lib/device/dev-type.h"
#include "lib/device/device_id.h"
#include "lib/label/label.h"
#include "lib/config/config.h"
#include "lib/commands/toolcontext.h"
#include "device_mapper/misc/dm-ioctl.h"
#include "lib/activate/activate.h"
#include "lib/misc/lvm-string.h"
#include "lib/mm/xlate.h"

#ifdef UDEV_SYNC_SUPPORT
#include <libudev.h>
#endif
#include <unistd.h>
#include <dirent.h>
#include <locale.h>
#include <time.h>
/* coverity[unnecessary_header] needed for MuslC */
#include <sys/file.h>

struct dev_iter {
	union radix_value *values;
	struct dev_filter *filter;
	unsigned nr_values;
	unsigned pos;
};

struct dir_list {
	struct dm_list list;
	char dir[0];
};

static struct {
	struct dm_pool *mem;
	struct radix_tree *names;
	struct dm_hash_table *vgid_index;
	struct dm_hash_table *lvid_index;
	struct dm_list *dm_devs; /* dm_active_device structs with dm UUIDs from DM_DEVICE_LIST (when available) */
	struct radix_tree *dm_uuids; /* references dm_devs entries */
	struct radix_tree *dm_devnos; /* references dm_devs entries */
	struct radix_tree *sysfs_only_devices; /* see comments in _get_device_for_sysfs_dev_name_using_devno */
	struct radix_tree *devices;
	struct dm_regex *preferred_names_matcher;
	const char *dev_dir;
	int use_dm_devs_cache;

	size_t dev_dir_len;
	int has_scanned;
	dev_t st_dev;
	struct dm_list dirs;
	struct dm_list files;

} _cache;

#define _zalloc(x) dm_pool_zalloc(_cache.mem, (x))
#define _free(x) dm_pool_free(_cache.mem, (x))
#define _strdup(x) dm_pool_strdup(_cache.mem, (x))

static int _insert(const char *path, const struct stat *info,
		   int rec, int check_with_udev_db);

/* Setup non-zero members of passed zeroed 'struct device' */
void dev_init(struct device *dev)
{
	dev->fd = -1;
	dev->bcache_fd = -1;
	dev->bcache_di = -1;
	dev->read_ahead = -1;
	dev->part = -1;

	dev->ext.enabled = 0;
	dev->ext.src = DEV_EXT_NONE;

	dm_list_init(&dev->aliases);
	dm_list_init(&dev->ids);
	dm_list_init(&dev->wwids);
}

static struct device *_dev_create(dev_t d)
{
	struct device *dev;

	if (!(dev = _zalloc(sizeof(*dev)))) {
		log_error("struct device allocation failed");
		return NULL;
	}

	dev_init(dev);
	dev->dev = d;

	return dev;
}

void dev_set_preferred_name(struct dm_str_list *sl, struct device *dev)
{
	/*
	 * Don't interfere with ordering specified in config file.
	 */
	if (_cache.preferred_names_matcher)
		return;

	log_debug_devs("%s: New preferred name", sl->str);
	dm_list_del(&sl->list);
	dm_list_add_h(&dev->aliases, &sl->list);
}

/*
 * Check whether path0 or path1 contains the subpath. The path that
 * *does not* contain the subpath wins (return 0 or 1). If both paths
 * contain the subpath, return -1. If none of them contains the subpath,
 * return -2.
 */
static int _builtin_preference(const char *path0, const char *path1,
			       size_t skip_prefix_count, const char *subpath)
{
	size_t subpath_len;
	int r0, r1;

	subpath_len = strlen(subpath);

	r0 = !strncmp(path0 + skip_prefix_count, subpath, subpath_len);
	r1 = !strncmp(path1 + skip_prefix_count, subpath, subpath_len);

	if (!r0 && r1)
		/* path0 does not have the subpath - it wins */
		return 0;
	else if (r0 && !r1)
		/* path1 does not have the subpath - it wins */
		return 1;
	else if (r0 && r1)
		/* both of them have the subpath */
		return -1;

	/* no path has the subpath */
	return -2;
}

static int _apply_builtin_path_preference_rules(const char *path0, const char *path1)
{
	size_t devdir_len = _cache.dev_dir_len;
	int r;


	if (!strncmp(path0, _cache.dev_dir, devdir_len) &&
	    !strncmp(path1, _cache.dev_dir, devdir_len)) {
		/*
		 * We're trying to achieve the ordering:
		 *	/dev/block/ < /dev/dm-* < /dev/disk/ < /dev/mapper/ < anything else
		 */

		/* Prefer any other path over /dev/block/ path. */
		if ((r = _builtin_preference(path0, path1, devdir_len, "block/")) >= -1)
			return r;

		/* Prefer any other path over /dev/dm-* path. */
		if ((r = _builtin_preference(path0, path1, devdir_len, "dm-")) >= -1)
			return r;

		/* Prefer any other path over /dev/disk/ path. */
		if ((r = _builtin_preference(path0, path1, devdir_len, "disk/")) >= -1)
			return r;

		/* Prefer any other path over /dev/mapper/ path. */
		if ((r = _builtin_preference(path0, path1, 0, dm_dir())) >= -1)
			return r;
	}

	return -1;
}

/* Return 1 if we prefer path1 else return 0 */
static int _compare_paths(const char *path0, const char *path1)
{
	int slash0 = 0, slash1 = 0;
	int m0, m1;
	const char *p;
	char p0[PATH_MAX], p1[PATH_MAX];
	char *s0, *s1;
	struct stat stat0, stat1;
	int r;

	/*
	 * FIXME Better to compare patterns one-at-a-time against all names.
	 */
	if (_cache.preferred_names_matcher) {
		m0 = dm_regex_match(_cache.preferred_names_matcher, path0);
		m1 = dm_regex_match(_cache.preferred_names_matcher, path1);

		if (m0 != m1) {
			if (m0 < 0)
				return 1;
			if (m1 < 0)
				return 0;
			if (m0 < m1)
				return 1;
			if (m1 < m0)
				return 0;
		}
	}

	/* Apply built-in preference rules first. */
	if ((r = _apply_builtin_path_preference_rules(path0, path1)) >= 0)
		return r;

	/* Return the path with fewer slashes */
	for (p = path0; p++; p = (const char *) strchr(p, '/'))
		slash0++;

	for (p = path1; p++; p = (const char *) strchr(p, '/'))
		slash1++;

	if (slash0 < slash1)
		return 0;
	if (slash1 < slash0)
		return 1;

	dm_strncpy(p0, path0, sizeof(p0));
	dm_strncpy(p1, path1, sizeof(p1));
	s0 = p0 + 1;
	s1 = p1 + 1;

	/*
	 * If we reach here, both paths are the same length.
	 * Now skip past identical path components.
	 */
	while (*s0 && *s0 == *s1)
		s0++, s1++;

	/* We prefer symlinks - they exist for a reason!
	 * So we prefer a shorter path before the first symlink in the name.
	 * FIXME Configuration option to invert this? */
	while (s0 && s1) {
		if ((s0 = strchr(s0, '/')))
			*s0 = '\0';

		if ((s1 = strchr(s1, '/')))
			*s1 = '\0';

		if (lstat(p0, &stat0)) {
			log_sys_very_verbose("lstat", p0);
			return 1;
		}
		if (lstat(p1, &stat1)) {
			log_sys_very_verbose("lstat", p1);
			return 0;
		}
		if (S_ISLNK(stat0.st_mode) && !S_ISLNK(stat1.st_mode))
			return 0;
		if (!S_ISLNK(stat0.st_mode) && S_ISLNK(stat1.st_mode))
			return 1;
		if (s0)
			*s0++ = '/';
		if (s1)
			*s1++ = '/';
	}

	/* ASCII comparison */
	if (strcmp(path0, path1) < 0)
		return 0;

	return 1;
}

enum add_hash {
	NO_HASH,
	HASH,
	REHASH
};

static int _add_alias(struct device *dev, const char *path, enum add_hash hash)
{
	struct dm_str_list *sl;
	struct dm_str_list *strl;
	const char *oldpath;
	int prefer_old = 1;
	size_t path_len = strlen(path);

	if (hash == REHASH)
		radix_tree_remove(_cache.names, path, path_len);

	/* Is name already there? */
	dm_list_iterate_items(strl, &dev->aliases)
		if (!strcmp(strl->str, path)) {
			path = strl->str;
			goto out;
		}

	if (!(path = _strdup(path)) ||
	    !(sl = _zalloc(sizeof(*sl)))) {
		log_error("Failed to add alias to dev cache.");
		return 0;
	}

	if (!strncmp(path, "/dev/nvme", 9))
		dev->flags |= DEV_IS_NVME;

	sl->str = path;

	if (!dm_list_empty(&dev->aliases)) {
		oldpath = dm_list_item(dev->aliases.n, struct dm_str_list)->str;
		prefer_old = _compare_paths(path, oldpath);
	}

	if (prefer_old)
		dm_list_add(&dev->aliases, &sl->list);
	else
		dm_list_add_h(&dev->aliases, &sl->list);
out:
	if ((hash != NO_HASH) &&
	    !radix_tree_insert_ptr(_cache.names, path, path_len, dev)) {
		log_error("Couldn't add name to hash in dev cache.");
		return 0;
	}

	return 1;
}

int get_sysfs_binary(const char *path, char *buf, size_t buf_size, int *retlen)
{
	int ret;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	ret = read(fd, buf, buf_size);

	if (close(fd))
		log_sys_debug("close", path);

	if (ret <= 0)
		return 0;
	*retlen = ret;
	return 1;
}

/* coverity[+tainted_string_sanitize_content:arg-0] */
static int _sanitize_buffer(const char *buf)
{
	size_t i;

	if (!strcmp(buf, ".."))
		return 0;

	for (i = 0; buf[i]; ++i)
		if ((buf[i] < ' ') ||
		    (buf[i] == '/') ||
		    (buf[i] == ':') ||
		    (buf[i] == '\\'))
			return 0;

	return 1;
}

int get_sysfs_value(const char *path, char *buf, size_t buf_size, int error_if_no_value)
{
	FILE *fp;
	size_t len;
	int r = 0;

	if (!(fp = fopen(path, "r"))) {
		if (error_if_no_value)
			log_sys_debug("fopen", path);
		return 0;
	}

	if (!fgets(buf, buf_size, fp)) {
		if (error_if_no_value)
			log_sys_debug("fgets", path);
		goto out;
	}

	if ((len = strlen(buf)) && buf[len - 1] == '\n')
		buf[--len] = '\0';

	if (!len) {
		if (error_if_no_value)
			log_error("_get_sysfs_value: %s: no value", path);
	} else
		r = 1;
out:
	if (fclose(fp))
		log_sys_debug("fclose", path);

	return r;
}

/* Change bit ordering for devno to generate more compact bTree */
static inline uint32_t _shuffle_devno(dev_t d)
{
	return cpu_to_be32(d);
	//return (d & 0xff) << 24 | (d & 0xff00) << 8 | (d & 0xff0000) >> 8 | (d & 0xff000000) >> 24;
	//return (d & 0xff000000) >> 24 | (d & 0xffff00) | ((d & 0xff) << 24);
	//return (uint32_t) d;
}

static struct dm_list *_get_or_add_list_by_index_key(struct dm_hash_table *idx, const char *key)
{
	struct dm_list *list;

	if ((list = dm_hash_lookup(idx, key)))
		return list;

	if (!(list = _zalloc(sizeof(*list)))) {
		log_error("%s: failed to allocate device list for device cache index.", key);
		return NULL;
	}

	dm_list_init(list);

	if (!dm_hash_insert(idx, key, list)) {
		log_error("%s: failed to insert device list to device cache index.", key);
		return NULL;
	}

	return list;
}

static bool _dev_cache_insert_devno(struct radix_tree *rt, dev_t devno, void *dev)
{
	union radix_value v = { .ptr = dev };
	uint32_t key = _shuffle_devno(devno);

	return radix_tree_insert(rt, &key, sizeof(key), v);
}

static struct device *_dev_cache_get_dev_by_devno(struct radix_tree *rt, dev_t devno)
{
	uint32_t key = _shuffle_devno(devno);

	return radix_tree_lookup_ptr(rt, &key, sizeof(key));
}

static struct device *_insert_sysfs_dev(dev_t devno, const char *devname)
{
	static struct device _fake_dev = { .flags = DEV_USED_FOR_LV };
	struct stat stat0;
	char path[PATH_MAX];
	struct device *dev;

	if (dm_snprintf(path, sizeof(path), "%s%s", _cache.dev_dir, devname) < 0) {
		log_error("_insert_sysfs_dev: %s: dm_snprintf failed", devname);
		return NULL;
	}

	if (lstat(path, &stat0) < 0) {
		/* When device node does not exist return fake entry.
		 * This may happen when i.e. lvm2 device dir != /dev */
		log_debug("%s: Not available device node", path);
		return &_fake_dev;
	}

	if (!(dev = _dev_create(devno)))
		return_NULL;

	if (!_add_alias(dev, path, NO_HASH)) {
		_free(dev);
		return_NULL;
	}

	if (!_dev_cache_insert_devno(_cache.sysfs_only_devices, devno, dev)) {
		log_error("Couldn't add device to binary tree of sysfs-only devices in dev cache.");
		_free(dev);
		return NULL;
	}

	return dev;
}

static struct device *_get_device_for_sysfs_dev_name_using_devno(const char *devname)
{
	char path[PATH_MAX];
	char buf[PATH_MAX];
	int major, minor;
	dev_t devno;
	struct device *dev;

	if (dm_snprintf(path, sizeof(path), "%sblock/%s/dev", dm_sysfs_dir(), devname) < 0) {
		log_error("_get_device_for_sysfs_dev_name_using_devno: %s: dm_snprintf failed", devname);
		return NULL;
	}

	if (!get_sysfs_value(path, buf, sizeof(buf), 1))
		return_NULL;

	if (sscanf(buf, "%d:%d", &major, &minor) != 2) {
		log_error("_get_device_for_sysfs_dev_name_using_devno: %s: failed to get major and minor number", devname);
		return NULL;
	}

	devno = MKDEV(major, minor);
	if (!(dev = _dev_cache_get_dev_by_devno(_cache.devices, devno))) {
		/*
		 * If we get here, it means the device is referenced in sysfs, but it's not yet in /dev.
		 * This may happen in some rare cases right after LVs get created - we sync with udev
		 * (or alternatively we create /dev content ourselves) while VG lock is held. However,
		 * dev scan is done without VG lock so devices may already be in sysfs, but /dev may
		 * not be updated yet if we call LVM command right after LV creation. This is not a
		 * problem with devtmpfs as there's at least kernel name for device in /dev as soon
		 * as the sysfs item exists, but we still support environments without devtmpfs or
		 * where different directory for dev nodes is used (e.g. our test suite). So track
		 * such devices in _cache.sysfs_only_devices hash for the vgid/lvid check to work still.
		 */
		if (!(dev = _dev_cache_get_dev_by_devno(_cache.sysfs_only_devices, devno)) &&
		    !(dev = _insert_sysfs_dev(devno, devname)))
			return_NULL;
	}

	return dev;
}

#define NOT_LVM_UUID "-"

static int _get_vgid_and_lvid_for_dev(struct cmd_context *cmd, struct device *dev)
{
	const size_t lvm_prefix_len = sizeof(UUID_PREFIX) - 1;
	const size_t lvm_uuid_len = sizeof(UUID_PREFIX) - 1 + 2 * ID_LEN;
	char uuid[DM_UUID_LEN];
	size_t uuid_len;

	if (!dev_dm_uuid(cmd, dev, uuid, sizeof(uuid)))
		return_0;

	uuid_len = strlen(uuid);

	/*
	 * UUID for LV is either "LVM-<vg_uuid><lv_uuid>" or "LVM-<vg_uuid><lv_uuid>-<suffix>",
	 * where vg_uuid and lv_uuid has length of ID_LEN and suffix len is not restricted
	 * (only restricted by whole DM UUID max len).
	 */
	if (((uuid_len == lvm_uuid_len) ||
	     ((uuid_len > lvm_uuid_len) && (uuid[lvm_uuid_len] == '-'))) &&
	    !strncmp(uuid, UUID_PREFIX, lvm_prefix_len)) {
		/* Separate VGID and LVID part from DM UUID. */
		if (!(dev->vgid = dm_pool_strndup(_cache.mem, uuid + lvm_prefix_len, ID_LEN)) ||
		    !(dev->lvid = dm_pool_strndup(_cache.mem, uuid + lvm_prefix_len + ID_LEN, ID_LEN)))
			return_0;
	} else
		dev->vgid = dev->lvid = NOT_LVM_UUID;

	return 1;
}

static int _index_dev_by_vgid_and_lvid(struct cmd_context *cmd, struct device *dev)
{
	const char *devname = dev_name(dev);
	char devpath[PATH_MAX];
	char path[PATH_MAX];
	DIR *d;
	struct dirent *dirent;
	struct device *holder_dev;
	struct dm_list *vgid_list, *lvid_list;
	struct device_list *dl_vgid, *dl_lvid;
	int r = 0;

	if (dev->flags & DEV_USED_FOR_LV)
		/* already indexed */
		return 1;

	/* Get holders for device. */
	if (dm_snprintf(path, sizeof(path), "%sdev/block/%u:%u/holders/",
			dm_sysfs_dir(), MAJOR(dev->dev), MINOR(dev->dev)) < 0) {
		log_error("%s: dm_snprintf failed for path to holders directory.", devname);
		return 0;
	}

	if (!(d = opendir(path))) {
		if (errno == ENOENT) {
			log_debug("%s: path does not exist, skipping", path);
			return 1;
		}
		log_sys_error("opendir", path);
		return 0;
	}

	/* Iterate over device's holders and look for LVs. */
	while ((dirent = readdir(d))) {
		if (!strcmp(".", dirent->d_name) ||
		    !strcmp("..", dirent->d_name))
			continue;

		if (dm_snprintf(devpath, sizeof(devpath), "%s%s", _cache.dev_dir, dirent->d_name) == -1) {
			log_error("%s: dm_snprintf failed for holder %s device path.", devname, dirent->d_name);
			goto out;
		}

		if (!(holder_dev = dev_cache_get_dev_by_name(devpath))) {
			/*
			 * Cope with situation where canonical /<dev_dir>/<dirent->d_name>
			 * does not exist, but some other node name or symlink exists in
			 * non-standard environments - someone renaming the nodes or using
			 * mknod with different dev names than actual kernel names.
			 * This looks up struct device by major:minor pair which we get
			 * by looking at /sys/block/<dirent->d_name>/dev sysfs attribute.
			 */
			if (!(holder_dev = _get_device_for_sysfs_dev_name_using_devno(dirent->d_name))) {
				log_error("%s: failed to find associated device structure for holder %s.", devname, devpath);
				goto out;
			}
		}

		/* We're only interested in a holder which is a DM device. */
		if (!dm_is_dm_major(MAJOR(holder_dev->dev)))
			continue;

		/*
		 * And if it's a DM device, we're only interested in a holder which is an LVM device.
		 * Get the VG UUID and LV UUID if we don't have that already.
		 */
		if (!holder_dev->vgid && !_get_vgid_and_lvid_for_dev(cmd, holder_dev))
			goto_out;

		if (*holder_dev->vgid == *NOT_LVM_UUID)
			continue;

		/*
		 * Do not add internal LV devices to index.
		 * If a device is internal, the holder has the same VG UUID as the device.
		 */
		if (dm_is_dm_major(MAJOR(dev->dev))) {
			if (!dev->vgid && !_get_vgid_and_lvid_for_dev(cmd, dev))
				goto_out;

			if (*dev->vgid != *NOT_LVM_UUID && !strcmp(holder_dev->vgid, dev->vgid))
				continue;
		}

		if (!(vgid_list = _get_or_add_list_by_index_key(_cache.vgid_index, holder_dev->vgid)) ||
		    !(lvid_list = _get_or_add_list_by_index_key(_cache.lvid_index, holder_dev->lvid)))
			goto_out;

		/* Create dev list items for the holder device. */
		if (!(dl_vgid = _zalloc(sizeof(*dl_vgid))) ||
		    !(dl_lvid = _zalloc(sizeof(*dl_lvid)))) {
			log_error("%s: failed to allocate dev list item.", devname);
			goto out;
		}

		dl_vgid->dev = dl_lvid->dev = dev;

		/* Add dev list item to VGID device list if it's not there already. */
		if (!(dev->flags & DEV_USED_FOR_LV))
			dm_list_add(vgid_list, &dl_vgid->list);

		/* Add dev list item to LVID device list. */
		dm_list_add(lvid_list, &dl_lvid->list);

		/* Mark device as used == also indexed in dev cache by VGID and LVID. */
		dev->flags |= DEV_USED_FOR_LV;
	}

	r = 1;
out:
	if (closedir(d))
		log_sys_debug("closedir", path);

	return r;
}

struct dm_list *dev_cache_get_dev_list_for_vgid(const char *vgid)
{
	return dm_hash_lookup(_cache.vgid_index, vgid);
}

struct dm_list *dev_cache_get_dev_list_for_lvid(const char *lvid)
{
	return dm_hash_lookup(_cache.lvid_index, lvid);
}

/*
 * Scanning code calls this when it fails to open a device using
 * this path.  The path is dropped from dev-cache.  In the next
 * dev_cache_scan it may be added again, but it could be for a
 * different device.
 */

void dev_cache_failed_path(struct device *dev, const char *path)
{
	struct dm_str_list *strl;

	radix_tree_remove(_cache.names, path, strlen(path));

	dm_list_iterate_items(strl, &dev->aliases) {
		if (!strcmp(strl->str, path)) {
			dm_list_del(&strl->list);
			break;
		}
	}
}

/*
 * Either creates a new dev, or adds an alias to
 * an existing dev.
 */
static int _insert_dev(const char *path, dev_t d)
{
	struct device *dev;
	struct device *dev_by_devt;
	struct device *dev_by_path;

	dev_by_devt = _dev_cache_get_dev_by_devno(_cache.devices, d);
	dev_by_path = dev_cache_get_dev_by_name(path);
	dev = dev_by_devt;

	/*
	 * Existing device, existing path points to the same device.
	 */
	if (dev_by_devt && dev_by_path && (dev_by_devt == dev_by_path)) {
		log_debug_devs("Found dev %u:%u %s - exists. %.8s",
			       MAJOR(d), MINOR(d), path, dev->pvid);
		return 1;
	}

	/*
	 * No device or path found, add devt to cache.devices, add name to cache.names.
	 */
	if (!dev_by_devt && !dev_by_path) {
		log_debug_devs("Found dev %u:%u %s - new.", MAJOR(d), MINOR(d), path);
		if (!(dev = _dev_cache_get_dev_by_devno(_cache.sysfs_only_devices, d)))
			/* create new device */
			if (!(dev = _dev_create(d)))
				return_0;

		if (!(_dev_cache_insert_devno(_cache.devices, d, dev))) {
			log_error("Couldn't insert device into binary tree.");
			_free(dev);
			return 0;
		}

		if (!_add_alias(dev, path, HASH))
			return_0;

		return 1;
	}

	/*
	 * Existing device, path is new, add path as a new alias for the device.
	 */
	if (dev_by_devt && !dev_by_path) {
		log_debug_devs("Found dev %u:%u %s - new alias.", MAJOR(d), MINOR(d), path);

		if (!_add_alias(dev, path, HASH))
			return_0;

		return 1;
	}

	/*
	 * No existing device, but path exists and previously pointed
	 * to a different device.
	 */
	if (!dev_by_devt && dev_by_path) {
		log_debug_devs("Found dev %u:%u %s - new device, path was previously %u:%u.",
			       MAJOR(d), MINOR(d), path,
			       MAJOR(dev_by_path->dev), MINOR(dev_by_path->dev));

		if (!(dev = _dev_cache_get_dev_by_devno(_cache.sysfs_only_devices, d))) {
			/* create new device */
			if (!(dev = _dev_create(d)))
				return_0;
		}

		if (!(_dev_cache_insert_devno(_cache.devices, d, dev))) {
			log_error("Couldn't insert device into binary tree.");
			_free(dev);
			return 0;
		}

		if (!_add_alias(dev, path, REHASH))
			return_0;

		return 1;
	}

	/*
	 * Existing device, and path exists and previously pointed to
	 * a different device.
	 */
	if (dev_by_devt && dev_by_path) {
		log_debug_devs("Found dev %u:%u %s - existing device, path was previously %u:%u.",
			       MAJOR(d), MINOR(d), path,
			       MAJOR(dev_by_path->dev), MINOR(dev_by_path->dev));

		if (!_add_alias(dev, path, REHASH))
			return_0;

		return 1;
	}

	log_error("Found dev %u:%u %s - failed to use.", MAJOR(d), MINOR(d), path);
	return 0;
}

/*
 * Get rid of extra slashes in the path string.
 */
static size_t _collapse_slashes(char *str)
{
	char *start = str;
	char *ptr;
	int was_slash = 0;

	for (ptr = str; *ptr; ptr++) {
		if (*ptr == '/') {
			if (was_slash)
				continue;

			was_slash = 1;
		} else
			was_slash = 0;
		*str++ = *ptr;
	}

	*str = 0;

	return (str - start);
}

static int _insert_dir(const char *dir)
{
	/* alphabetically! sorted list used by bsearch of
	 * /dev subdirectories that should not contain
	 * any block device, so no reason to scan them. */
	static const char _no_scan[][12] = {
		"bsg/",
		"bus/",
		"char/",
		"cpu/",
		"dma_heap/",
		"dri/",
		"input/",
		"snd/",
		"usb/",
	};
	int n, dirent_count, r = 1;
	struct dirent **dirent = NULL;
	char path[PATH_MAX];
	size_t len;

	if (!_dm_strncpy(path, dir, sizeof(path))) {
		log_debug_devs("Dir path %s is too long", path);
		return 0;
	}

	len = _collapse_slashes(path);
	if (len && path[len - 1] != '/')
		path[len++] = '/';

	if ((len < (5 + sizeof(_no_scan[0]))) && (strncmp("/dev/", path, 5) == 0) && (len > 5)) {
		path[len] = 0;
		if (bsearch(path + 5, _no_scan, DM_ARRAY_SIZE(_no_scan), sizeof(_no_scan[0]),
			    (int (*)(const void*, const void*))strcmp)) {
			/* Skip insertion of directories that can't have block devices */
			log_debug("Skipping \"%s\" (no block devices).", path);
			return 1;
		}
	}

	dirent_count = scandir(dir, &dirent, NULL, alphasort);
	if (dirent_count > 0) {
		for (n = 0; n < dirent_count; n++) {
			if (dirent[n]->d_name[0] == '.')
				continue;

			if (!_dm_strncpy(path + len, dirent[n]->d_name, sizeof(path) - len)) {
				log_debug_devs("Path %s/%s is too long.", dir, dirent[n]->d_name);
				r = 0;
				continue;
			}

			r &= _insert(path, NULL, 1, 0);
		}

		for (n = 0; n < dirent_count; n++)
			free(dirent[n]);
		free(dirent);
	}

	return r;
}

static int _dev_cache_iterate_devs_for_index(struct cmd_context *cmd)
{
	struct dev_iter *iter;
	struct device *dev = NULL;
	int r = 1;

	if (!(iter = dev_iter_create(NULL, 0)))
		return_0;

	while ((dev = dev_iter_get(NULL, iter)))
		if (!_index_dev_by_vgid_and_lvid(cmd, dev))
			r = 0;

	return r;
}

static int _dev_cache_iterate_sysfs_for_index(struct cmd_context *cmd, const char *path)
{
	char devname[PATH_MAX];
	DIR *d;
	struct dirent *dirent;
	int major, minor;
	dev_t devno;
	struct device *dev;
	int partial_failure = 0;
	int r = 0;

	if (!(d = opendir(path))) {
		log_sys_error("opendir", path);
		return 0;
	}

	while ((dirent = readdir(d))) {
		if (!strcmp(".", dirent->d_name) ||
		    !strcmp("..", dirent->d_name))
			continue;

		if (sscanf(dirent->d_name, "%d:%d", &major, &minor) != 2) {
			log_error("_dev_cache_iterate_sysfs_for_index: %s: failed "
				  "to get major and minor number", dirent->d_name);
			partial_failure = 1;
			continue;
		}

		devno = MKDEV(major, minor);
		if (!(dev = _dev_cache_get_dev_by_devno(_cache.devices, devno)) &&
		    !(dev = _dev_cache_get_dev_by_devno(_cache.sysfs_only_devices, devno))) {
			if (!dm_device_get_name(major, minor, 1, devname, sizeof(devname)) ||
			    !(dev = _insert_sysfs_dev(devno, devname))) {
				partial_failure = 1;
				continue;
			}
		}

		if (!_index_dev_by_vgid_and_lvid(cmd, dev))
			partial_failure = 1;
	}

	r = !partial_failure;

	if (closedir(d))
		log_sys_debug("closedir", path);

	return r;
}

static int _dev_cache_index_devs(struct cmd_context *cmd)
{
	static int _sysfs_has_dev_block = -1;
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%sdev/block", dm_sysfs_dir()) < 0) {
		log_error("dev_cache_index_devs: dm_snprintf failed.");
		return 0;
	}

	/* Skip indexing if /sys/dev/block is not available.*/
	if (_sysfs_has_dev_block == -1) {
		struct stat info;
		if (stat(path, &info) == 0)
			_sysfs_has_dev_block = 1;
		else {
			if (errno == ENOENT) {
				_sysfs_has_dev_block = 0;
				return 1;
			}

			log_sys_debug("stat", path);
			return 0;
		}
	} else if (!_sysfs_has_dev_block)
		return 1;

	if (obtain_device_list_from_udev() &&
	    udev_get_library_context())
		return _dev_cache_iterate_devs_for_index(cmd);  /* with udev */

	return _dev_cache_iterate_sysfs_for_index(cmd, path);
}

#ifdef UDEV_SYNC_SUPPORT

static int _device_in_udev_db(const dev_t d)
{
	struct udev *udev;
	struct udev_device *udev_device;

	if (!(udev = udev_get_library_context()))
		return_0;

	if ((udev_device = udev_device_new_from_devnum(udev, 'b', d))) {
		udev_device_unref(udev_device);
		return 1;
	}

	return 0;
}

static int _insert_udev_dir(struct udev *udev, const char *dir)
{
	struct udev_enumerate *udev_enum = NULL;
	struct udev_list_entry *device_entry, *symlink_entry;
	const char *entry_name, *node_name, *symlink_name;
	struct udev_device *device;
	int r = 1;

	if (!(udev_enum = udev_enumerate_new(udev))) {
		log_error("Failed to udev_enumerate_new.");
		return 0;
	}

	if (udev_enumerate_add_match_subsystem(udev_enum, "block")) {
		log_error("Failed to udev_enumerate_add_match_subsystem.");
		goto out;
	}

	if (udev_enumerate_scan_devices(udev_enum)) {
		log_error("Failed to udev_enumerate_scan_devices.");
		goto out;
	}

	/*
	 * Report any missing information as "log_very_verbose" only, do not
	 * report it as a "warning" or "error" - the record could be removed
	 * by the time we ask for more info (node name, symlink name...).
	 * Whatever removes *any* block device in the system (even unrelated
	 * to our operation), we would have a warning/error on output then.
	 * That could be misleading. If there's really any problem with missing
	 * information from udev db, we can still have a look at the verbose log.
	 */
	udev_list_entry_foreach(device_entry, udev_enumerate_get_list_entry(udev_enum)) {
		entry_name = udev_list_entry_get_name(device_entry);

		if (!(device = udev_device_new_from_syspath(udev, entry_name))) {
			log_very_verbose("udev failed to return a device for entry %s.",
					 entry_name);
			continue;
		}

		if (!(node_name = udev_device_get_devnode(device)))
			log_very_verbose("udev failed to return a device node for entry %s.",
					 entry_name);
		else
			r &= _insert(node_name, NULL, 0, 0);

		udev_list_entry_foreach(symlink_entry, udev_device_get_devlinks_list_entry(device)) {
			if (!(symlink_name = udev_list_entry_get_name(symlink_entry)))
				log_very_verbose("udev failed to return a symlink name for entry %s.",
						 entry_name);
			else
				r &= _insert(symlink_name, NULL, 0, 0);
		}

		udev_device_unref(device);
	}

out:
	udev_enumerate_unref(udev_enum);

	return r;
}

static void _insert_dirs(struct dm_list *dirs)
{
	struct dir_list *dl;
	struct udev *udev = NULL;
	int with_udev;
	struct stat tinfo;

	with_udev = obtain_device_list_from_udev() &&
		    (udev = udev_get_library_context());

	dm_list_iterate_items(dl, &_cache.dirs) {
		if (stat(dl->dir, &tinfo) < 0) {
			log_warn("WARNING: Cannot use dir %s, %s.",
				 dl->dir, strerror(errno));
			continue;
		}
		_cache.st_dev = tinfo.st_dev;
		if (with_udev) {
			if (!_insert_udev_dir(udev, dl->dir))
				log_debug_devs("%s: Failed to insert devices from "
					       "udev-managed directory to device "
					       "cache fully", dl->dir);
		}
		else if (!_insert_dir(dl->dir))
			log_debug_devs("%s: Failed to insert devices to "
				       "device cache fully", dl->dir);
	}
}

#else	/* UDEV_SYNC_SUPPORT */

static int _device_in_udev_db(const dev_t d)
{
	return 0;
}

static void _insert_dirs(struct dm_list *dirs)
{
	struct dir_list *dl;
	struct stat tinfo;

	dm_list_iterate_items(dl, &_cache.dirs) {
		if (stat(dl->dir, &tinfo) < 0) {
			log_warn("WARNING: Cannot use dir %s, %s.",
				 dl->dir, strerror(errno));
			continue;
		}
		_cache.st_dev = tinfo.st_dev;
		_insert_dir(dl->dir);
	}
}

#endif	/* UDEV_SYNC_SUPPORT */

static int _insert(const char *path, const struct stat *info,
		   int rec, int check_with_udev_db)
{
	struct stat tinfo;

	if (!info) {
		if (stat(path, &tinfo) < 0) {
			log_sys_very_verbose("stat", path);
			return 0;
		}
		info = &tinfo;
	}

	if (check_with_udev_db && !_device_in_udev_db(info->st_rdev)) {
		log_very_verbose("%s: Not in udev db", path);
		return 0;
	}

	if (S_ISDIR(info->st_mode)) {	/* add a directory */
		/* check it's not a symbolic link */
		if (lstat(path, &tinfo) < 0) {
			log_sys_very_verbose("lstat", path);
			return 0;
		}

		if (S_ISLNK(tinfo.st_mode)) {
			log_debug_devs("%s: Symbolic link to directory", path);
			return 1;
		}

		if (info->st_dev != _cache.st_dev) {
			log_debug_devs("%s: Different filesystem in directory", path);
			return 1;
		}

		if (rec && !_insert_dir(path))
			return 0;
	} else {		/* add a device */
		if (!S_ISBLK(info->st_mode))
			return 1;

		if (!_insert_dev(path, info->st_rdev))
			return 0;
	}

	return 1;
}

static void _drop_all_aliases(struct device *dev)
{
	struct dm_str_list *strl, *strl2;

	dm_list_iterate_items_safe(strl, strl2, &dev->aliases) {
		log_debug("Drop alias for %u:%u %s.", MAJOR(dev->dev), MINOR(dev->dev), strl->str);
		radix_tree_remove(_cache.names, strl->str, strlen(strl->str));
		dm_list_del(&strl->list);
	}
}

void dev_cache_scan(struct cmd_context *cmd)
{
	log_debug_devs("Creating list of system devices.");

	_cache.has_scanned = 1;

	setlocale(LC_COLLATE, "C"); /* Avoid sorting by locales */
	_insert_dirs(&_cache.dirs);
	setlocale(LC_COLLATE, "");

	if (cmd->check_devs_used)
		(void) _dev_cache_index_devs(cmd);
}

int dev_cache_has_scanned(void)
{
	return _cache.has_scanned;
}

static int _init_preferred_names(struct cmd_context *cmd)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *v;
	struct dm_pool *scratch = NULL;
	const char **regex;
	unsigned count = 0;
	int i, r = 0;

	_cache.preferred_names_matcher = NULL;

	if (!(cn = find_config_tree_array(cmd, devices_preferred_names_CFG, NULL)) ||
	    cn->v->type == DM_CFG_EMPTY_ARRAY) {
		log_very_verbose("devices/preferred_names %s: "
				 "using built-in preferences",
				 cn && cn->v->type == DM_CFG_EMPTY_ARRAY ? "is empty"
									 : "not found in config");
		return 1;
	}

	for (v = cn->v; v; v = v->next) {
		if (v->type != DM_CFG_STRING) {
			log_error("preferred_names patterns must be enclosed in quotes");
			return 0;
		}

		count++;
	}

	if (!(scratch = dm_pool_create("preferred device name matcher", 1024)))
		return_0;

	if (!(regex = dm_pool_alloc(scratch, sizeof(*regex) * count))) {
		log_error("Failed to allocate preferred device name "
			  "pattern list.");
		goto out;
	}

	for (v = cn->v, i = count - 1; v; v = v->next, i--) {
		if (!(regex[i] = dm_pool_strdup(scratch, v->v.str))) {
			log_error("Failed to allocate a preferred device name "
				  "pattern.");
			goto out;
		}
	}

	if (!(_cache.preferred_names_matcher =
		dm_regex_create(_cache.mem, regex, count))) {
		log_error("Preferred device name pattern matcher creation failed.");
		goto out;
	}

	r = 1;

out:
	dm_pool_destroy(scratch);

	return r;
}

int dm_devs_cache_use(void)
{
	return _cache.use_dm_devs_cache;
}

void dm_devs_cache_destroy(void)
{
	_cache.use_dm_devs_cache = 0;

	if (_cache.dm_devnos) {
		radix_tree_destroy(_cache.dm_devnos);
		_cache.dm_devnos = NULL;
	}

	if (_cache.dm_uuids) {
		radix_tree_destroy(_cache.dm_uuids);
		_cache.dm_uuids = NULL;
	}

	dm_device_list_destroy(&_cache.dm_devs);
}

int dm_devs_cache_update(void)
{
	struct dm_active_device *dm_dev;
	unsigned devs_features;
	uint32_t d;

	dm_devs_cache_destroy();

	if (!get_dm_active_devices(NULL, &_cache.dm_devs, &devs_features))
		return 1;

	if (!(devs_features & DM_DEVICE_LIST_HAS_UUID)) {
		/* Cache unusable with older kernels without UUIDs in LIST */
		dm_device_list_destroy(&_cache.dm_devs);
		return 1;
	}

	/* _cache.dm_devs entries are referenced by radix trees */

	/* TODO: if _cache.dm_devs list is small, then skip the
	   overhead of radix trees and just do list searches on dm_devs */

	if (!(_cache.dm_devnos = radix_tree_create(NULL, NULL)) ||
	    !(_cache.dm_uuids = radix_tree_create(NULL, NULL))) {
		return_0; // FIXME
	}

	/* Insert every active DM device into radix trees */
	dm_list_iterate_items(dm_dev, _cache.dm_devs) {
		d = _shuffle_devno(dm_dev->devno);

		if (!radix_tree_insert_ptr(_cache.dm_devnos, &d, sizeof(d), dm_dev))
			return_0;

		if (dm_dev->uuid[0] &&
		    !radix_tree_insert_ptr(_cache.dm_uuids, dm_dev->uuid, strlen(dm_dev->uuid), dm_dev))
			return_0;
	}

	//radix_tree_dump(_cache.dm_devnos, stdout);
	//radix_tree_dump(_cache.dm_uuids, stdout);

	_cache.use_dm_devs_cache = 1;
	return 1;
}

void dm_devs_cache_label_invalidate(struct cmd_context *cmd)
{
	struct dm_active_device *dm_dev;
	struct device *dev;

	dm_list_iterate_items(dm_dev, _cache.dm_devs) {
		if (dm_dev->uuid &&
		   strncmp(dm_dev->uuid, UUID_PREFIX, sizeof(UUID_PREFIX) - 1) == 0) {
			if ((dev = dev_cache_get_by_devt(cmd, dm_dev->devno)))
				label_scan_invalidate(dev);
		}
	}
}

/* Find active DM device in devs array for given major:minor */
const struct dm_active_device *
dm_devs_cache_get_by_devno(struct cmd_context *cmd, dev_t devno)
{
	uint32_t d = _shuffle_devno(devno);

	if (!_cache.dm_devnos)
		return NULL;

	return radix_tree_lookup_ptr(_cache.dm_devnos, &d, sizeof(d));
}

/* Find active DM device in devs array for given DM UUID */
const struct dm_active_device *
dm_devs_cache_get_by_uuid(struct cmd_context *cmd, const char *dm_uuid)
{
	if (!_cache.dm_uuids)
		return NULL;

	return radix_tree_lookup_ptr(_cache.dm_uuids, dm_uuid, strlen(dm_uuid));
}

int dev_cache_init(struct cmd_context *cmd)
{
	_cache.names = NULL;

	if (!(_cache.mem = dm_pool_create("dev_cache", 10 * 1024)))
		return_0;

	if (!(_cache.names = radix_tree_create(NULL, NULL)) ||
	    !(_cache.vgid_index = dm_hash_create(30)) ||
	    !(_cache.lvid_index = dm_hash_create(29))) {
		dm_pool_destroy(_cache.mem);
		_cache.mem = 0;
		return_0;
	}

	if (!(_cache.devices = radix_tree_create(NULL, NULL))) {
		log_error("Couldn't create binary tree for dev-cache.");
		goto bad;
	}

	if (!(_cache.sysfs_only_devices = radix_tree_create(NULL, NULL))) {
		log_error("Couldn't create binary tree for sysfs-only devices in dev cache.");
		goto bad;
	}

	if (!(_cache.dev_dir = _strdup(cmd->dev_dir))) {
		log_error("strdup dev_dir failed.");
		goto bad;
	}

	_cache.dev_dir_len = strlen(_cache.dev_dir);
	dm_list_init(&_cache.dirs);

	if (!_init_preferred_names(cmd))
		goto_bad;

	return 1;

      bad:
	dev_cache_exit();
	return 0;
}

struct dev_visitor {
	struct radix_tree_iterator it;
	int close_immediate;
	int free;
	unsigned num_open;
};
/*
 * Returns number of devices still open.
 */
static bool _visit_check_for_open_devices(struct radix_tree_iterator *it,
					  const void *key, size_t keylen,
					  union radix_value v)
{
	struct dev_visitor *vt = container_of(it, struct dev_visitor, it);
	struct device *dev = v.ptr;

	if (dev->fd >= 0) {
		log_error("Device '%s' has been left open (%d remaining references).",
			  dev_name(dev), dev->open_count);
		vt->num_open++;
		if (vt->close_immediate && !dev_close_immediate(dev))
			stack;
	}

	if (vt->free) {
		free_dids(&dev->ids);
		free_wwids(&dev->wwids);
	}

	return true;
}

/*
 * Returns number of devices left open.
 */
int dev_cache_check_for_open_devices(void)
{
	struct dev_visitor vt = {
		.it.visit = _visit_check_for_open_devices,
	};

	radix_tree_iterate(_cache.names, NULL, 0, &vt.it);

	return vt.num_open;
}

int dev_cache_exit(void)
{
	struct dev_visitor vt = {
		.it.visit = _visit_check_for_open_devices,
		.close_immediate = 1, /* close open devices */
		.free = 1,            /* free dids, wwids */
	};

	if (_cache.names) {
		/* check for open devices */
		radix_tree_iterate(_cache.names, NULL, 0, &vt.it);

		if (vt.num_open)
			log_error(INTERNAL_ERROR "%d device(s) were left open and have been closed.",
				  vt.num_open);
	}

	dm_devs_cache_destroy();

	if (_cache.mem)
		dm_pool_destroy(_cache.mem);

	if (_cache.names)
		radix_tree_destroy(_cache.names);

	if (_cache.vgid_index)
		dm_hash_destroy(_cache.vgid_index);

	if (_cache.lvid_index)
		dm_hash_destroy(_cache.lvid_index);

	if (_cache.devices)
	       radix_tree_destroy(_cache.devices);

	if (_cache.sysfs_only_devices)
	       radix_tree_destroy(_cache.sysfs_only_devices);

	memset(&_cache, 0, sizeof(_cache));

	return (!vt.num_open);
}

int dev_cache_add_dir(const char *path)
{
	struct dir_list *dl;
	struct stat st;
	size_t len;

	if (stat(path, &st)) {
		log_warn("Ignoring %s: %s.", path, strerror(errno));
		/* But don't fail */
		return 1;
	}

	if (!S_ISDIR(st.st_mode)) {
		log_warn("Ignoring %s: Not a directory.", path);
		return 1;
	}

	len = strlen(path);
	if (!(dl = _zalloc(sizeof(*dl) + len + 1))) {
		log_error("dir_list allocation failed");
		return 0;
	}

	memcpy(dl->dir, path, len + 1);
	dm_list_add(&_cache.dirs, &dl->list);
	return 1;
}

struct device *dev_cache_get_dev_by_name(const char *name)
{
	return radix_tree_lookup_ptr(_cache.names, name, strlen(name));
}

static void _remove_alias(struct device *dev, const char *name)
{
	struct dm_str_list *strl;

	dm_list_iterate_items(strl, &dev->aliases) {
		if (!strcmp(strl->str, name)) {
			dm_list_del(&strl->list);
			return;
		}
	}
}

/*
 * Check that paths for this dev still refer to the same dev_t.  This is known
 * to drop invalid paths in the case where lvm deactivates an LV, which causes
 * that LV path to go away, but that LV path is not removed from dev-cache (it
 * probably should be).  Later a new path to a different LV is added to
 * dev-cache, where the new LV has the same major:minor as the previously
 * deactivated LV.  The new LV will find the existing struct dev, and that
 * struct dev will have dev->aliases entries that refer to the name of the old
 * deactivated LV.  Those old paths are all invalid and are dropped here.
 */

void dev_cache_verify_aliases(struct device *dev)
{
	struct dm_str_list *strl, *strl2;
	struct stat st;

	dm_list_iterate_items_safe(strl, strl2, &dev->aliases) {
		if (stat(strl->str, &st) || (st.st_rdev != dev->dev)) {
			log_debug("Drop alias for %u:%u invalid path %s %u:%u.",
				  MAJOR(dev->dev), MINOR(dev->dev), strl->str,
				  MAJOR(st.st_rdev), MINOR(st.st_rdev));
			radix_tree_remove(_cache.names, strl->str, strlen(strl->str));
			dm_list_del(&strl->list);
		}
	}
}

static struct device *_dev_cache_get(struct cmd_context *cmd, const char *name, struct dev_filter *f, int existing)
{
	struct device *dev = dev_cache_get_dev_by_name(name);
	struct stat st;
	int ret;

	/*
	 * DEV_REGULAR means that is "dev" is actually a file, not a device.
	 * FIXME: I don't think dev-cache is used for files any more and this
	 * can be dropped?
	 */
	if (dev && (dev->flags & DEV_REGULAR))
		return dev;

	if (dev && dm_list_empty(&dev->aliases)) {
		/* shouldn't happen */
		log_warn("Ignoring dev with no valid paths for %s.", name);
		return NULL;
	}

	/*
	 * The requested path is invalid, remove any dev-cache info for it.
	 */
	if (stat(name, &st)) {
		if (dev) {
			log_debug("Device path %s is invalid for %u:%u %s.",
				  name, MAJOR(dev->dev), MINOR(dev->dev), dev_name(dev));

			radix_tree_remove(_cache.names, name, strlen(name));

			_remove_alias(dev, name);

			/* Remove any other names in dev->aliases that are incorrect. */
			dev_cache_verify_aliases(dev);
		}
		return NULL;
	}

	if (dev && dm_list_empty(&dev->aliases)) {
		/* shouldn't happen */
		log_warn("Ignoring dev with no valid paths for %s.", name);
		return NULL;
	}

	if (!S_ISBLK(st.st_mode)) {
		log_debug("Not a block device %s.", name);
		return NULL;
	}

	/*
	 * dev-cache has incorrect info for the requested path.
	 * Remove incorrect info and then add new dev-cache entry.
	 */
	if (dev && (st.st_rdev != dev->dev)) {
		struct device *dev_by_devt = _dev_cache_get_dev_by_devno(_cache.devices, st.st_rdev);

		/*
		 * lvm commands create this condition when they
		 * activate/deactivate LVs combined with creating new LVs.
		 * The command does not purge dev structs when deactivating
		 * an LV (which it probably should do), but the better
		 * approach would be not using dev-cache at all for LVs.
		 */

		log_debug("Dropping aliases for device entry %u:%u %s for new device %u:%u %s.",
			  MAJOR(dev->dev), MINOR(dev->dev), dev_name(dev),
			  MAJOR(st.st_rdev), MINOR(st.st_rdev), name);

		_drop_all_aliases(dev);

		if (dev_by_devt) {
			log_debug("Dropping aliases for device entry %u:%u %s for new device %u:%u %s.",
				   MAJOR(dev_by_devt->dev), MINOR(dev_by_devt->dev), dev_name(dev_by_devt),
				   MAJOR(st.st_rdev), MINOR(st.st_rdev), name);

			_drop_all_aliases(dev_by_devt);
		}

#if 0
		/*
		 * I think only lvm's own dm devs should be added here, so use
		 * a warning to look for any other unknown cases.
		 */
		if (MAJOR(st.st_rdev) != cmd->dev_types->device_mapper_major) {
			log_warn("WARNING: new device appeared %u:%u %s",
				  MAJOR(st.st_rdev), (MINOR(st.st_rdev)), name);
		}
#endif

		if (!_insert_dev(name, st.st_rdev))
			return_NULL;

		/* Get the struct dev that was just added. */
		dev = dev_cache_get_dev_by_name(name);

		if (!dev) {
			log_error("Failed to get device %s", name);
			return NULL;
		}

		goto out;
	}

	if (dev && dm_list_empty(&dev->aliases)) {
		/* shouldn't happen */
		log_warn("Ignoring dev with no valid paths for %s.", name);
		return NULL;
	}

	if (!dev && existing)
		return_NULL;

	/*
	 * This case should never be hit for a PV. It should only
	 * happen when the command is opening a new LV it has created.
	 * Add an arg to all callers indicating when the arg should be
	 * new (for an LV) and not existing.
	 * FIXME: fix this further by not using dev-cache struct devs
	 * at all for new dm devs (LVs) that lvm uses.  Make the
	 * dev-cache contain only devs for PVs.
	 * Places to fix that use a dev for LVs include:
	 * . lv_resize opening lv to discard
	 * . wipe_lv opening lv to zero it
	 * . _extend_sanlock_lv opening lv to extend it
	 * . _write_log_header opening lv to write header
	 * Also, io to LVs should not go through bcache.
	 * bcache should contain only labels and metadata
	 * scanned from PVs.
	 */
	if (!dev) {
		/*
		 * This case should only be used for new devices created by this
		 * command (opening LVs it's created), so if a dev exists for the
		 * dev_t referenced by the name, then drop all aliases for before
		 * _insert_dev adds the new name.  lvm commands actually hit this
		 * fairly often when it uses some LV, deactivates the LV, then
		 * creates some new LV which ends up with the same major:minor.
		 * Without dropping the aliases, it's plausible that lvm commands
		 * could end up using the wrong dm device.
		 */
		struct device *dev_by_devt = _dev_cache_get_dev_by_devno(_cache.devices, st.st_rdev);

		if (dev_by_devt) {
			log_debug("Dropping aliases for %u:%u before adding new path %s.",
				  MAJOR(st.st_rdev), MINOR(st.st_rdev), name);
			_drop_all_aliases(dev_by_devt);
		}

#if 0
		/*
		 * I think only lvm's own dm devs should be added here, so use
		 * a warning to look for any other unknown cases.
		 */
		if (MAJOR(st.st_rdev) != cmd->dev_types->device_mapper_major) {
			log_warn("WARNING: new device appeared %u:%u %s.",
				  MAJOR(st.st_rdev), MINOR(st.st_rdev), name);
		}
#endif

		if (!_insert_dev(name, st.st_rdev))
			return_NULL;

		/* Get the struct dev that was just added. */
		dev = dev_cache_get_dev_by_name(name);

		if (!dev) {
			log_error("Failed to get device %s", name);
			return NULL;
		}
	}

 out:
	/*
	 * The caller passed a filter if they only want the dev if it
	 * passes filters.
	 */

	if (!f)
		return dev;

	ret = f->passes_filter(cmd, f, dev, NULL);
	if (!ret) {
		log_debug_devs("dev_cache_get filter excludes %s", dev_name(dev));
		return NULL;
	}

	return dev;
}

struct device *dev_cache_get_existing(struct cmd_context *cmd, const char *name, struct dev_filter *f)
{
	return _dev_cache_get(cmd, name, f, 1);
}

struct device *dev_cache_get(struct cmd_context *cmd, const char *name, struct dev_filter *f)
{
	return _dev_cache_get(cmd, name, f, 0);
}

struct device *dev_cache_get_by_devt(struct cmd_context *cmd, dev_t devt)
{
	struct device *dev = _dev_cache_get_dev_by_devno(_cache.devices, devt);

	if (dev)
		return dev;
	log_debug_devs("No devno %u:%u in dev cache.", MAJOR(devt), MINOR(devt));
	return NULL;
}

struct device *dev_cache_get_by_pvid(struct cmd_context *cmd, const char *pvid)
{
	struct dev_iter *iter;
	struct device *dev;

	if (!(iter = dev_iter_create(NULL, 0)))
		return_NULL;

	while ((dev = dev_iter_get(NULL, iter)))
		if (!memcmp(dev->pvid, pvid, ID_LEN))
			break;

	dev_iter_destroy(iter);

	return dev;
}

struct dev_iter *dev_iter_create(struct dev_filter *f, int unused)
{
	struct dev_iter *di = malloc(sizeof(*di));

	if (!di) {
		log_error("dev_iter allocation failed.");
		return NULL;
	}

	if (!radix_tree_values(_cache.devices, NULL, 0,
			       &di->values, &di->nr_values)) {
		log_error("dev_iter values allocation failed.");
		free(di);
		return NULL;
	}

	di->pos = 0;
	di->filter = f;
	if (di->filter)
		di->filter->use_count++;

	return di;
}

void dev_iter_destroy(struct dev_iter *iter)
{
	if (iter->filter)
		iter->filter->use_count--;
	free(iter->values);
	free(iter);
}

struct device *dev_iter_get(struct cmd_context *cmd, struct dev_iter *iter)
{
	struct dev_filter *f;
	struct device *d;
	int ret;

	while (iter->pos < iter->nr_values) {
		d = iter->values[iter->pos++].ptr;
		ret = 1;

		f = iter->filter;

		if (f && cmd && !(d->flags & DEV_REGULAR))
			ret = f->passes_filter(cmd, f, d, NULL);

		if (!f || (d->flags & DEV_REGULAR) || ret)
			return d;
	}

	return NULL;
}

int dev_fd(struct device *dev)
{
	return dev->fd;
}

const char *dev_name(const struct device *dev)
{
	if (dev && dev->aliases.n && !dm_list_empty(&dev->aliases))
		return dm_list_item(dev->aliases.n, struct dm_str_list)->str;
	else
		return unknown_device_name();
}

bool dev_cache_has_md_with_end_superblock(struct dev_types *dt)
{
	struct device *dev;
	struct dev_iter *iter;
	bool ret = false;

	if (!(iter = dev_iter_create(NULL, 0)))
		return_false;

	while ((dev = dev_iter_get(NULL, iter)))
		if ((ret = dev_is_md_with_end_superblock(dt, dev)))
			break;

	dev_iter_destroy(iter);

	return ret;
}

static int _setup_devices_list(struct cmd_context *cmd)
{
	struct dm_str_list *strl;
	struct dev_use *du;

	/*
	 * For each --devices arg, add a du to cmd->use_devices.
	 * The du has devname is the devices arg value.
	 */

	dm_list_iterate_items(strl, &cmd->deviceslist) {
		if (!(du = dm_pool_zalloc(cmd->mem, sizeof(struct dev_use))))
			return_0;

		if (!(du->devname = dm_pool_strdup(cmd->mem, strl->str)))
			return_0;

		dm_list_add(&cmd->use_devices, &du->list);
	}

	return 1;
}

/*
 * LVs that use dmeventd need to be accessible using system.devices.
 *
 * dmeventd will use dmeventd.devices if it exists, otherwise will use
 * system.devices.  If a VG is covered by dmeventd.devices it should also
 * be covered by system.devices.  The automated vgchange --monitor and
 * vgchange -aay commands do not currently consider dmeventd.devices.
 * dmeventd.devices can be useful to prevent commands run by dmeventd
 * from reading VGs that do not need the service.
 *
 * dmeventd gets events for any applicable dm device in the kernel,
 * and will attempt to run lvm commands for it.  These commands
 * will only use dmeventd.devices or system.devices, so if an LV
 * that dmeventd is watching is not covered by either of these
 * devices files, the dmeventd-generated command will fail when
 * it doesn't find the VG.
 */
 
static int _setup_devices_file_dmeventd(struct cmd_context *cmd)
{
	char path[PATH_MAX];
	struct stat st;

	if (!find_config_tree_bool(cmd, devices_use_devicesfile_CFG, NULL)) {
		cmd->enable_devices_file = 0;
		return 1;
	}

	if (dm_snprintf(path, sizeof(path), "%s/devices/dmeventd.devices", cmd->system_dir) < 0)
		return_0;
	if (stat(path, &st))
		return 0;

	cmd->enable_devices_file = 1;
	dm_strncpy(cmd->devices_file_path, path, sizeof(cmd->devices_file_path));
	return 1;
}

/*
 * When lvm.conf use_devicesfile=0, then an existing system.devices
 * is renamed to system.devices-unused.<date>.<time>.  Because,
 * if lvm.conf is later changed to use_devicesfile=1, then the
 * old system.devices file would immediately be used again, and
 * the old file may not longer be correct due to changes to the
 * system while it was disabled.
 */
static void devices_file_rename_unused(struct cmd_context *cmd)
{
	char path[PATH_MAX];
	char path2[PATH_MAX];
	char datetime_str[48] = {0};
	const char *filename;
	time_t t;
	struct tm *tm;
	struct stat st;

	filename = find_config_tree_str(cmd, devices_devicesfile_CFG, NULL);

	if (!filename || !strlen(filename))
		return;

	if (dm_snprintf(path, sizeof(path), "%s/devices/%s", cmd->system_dir, filename) < 0)
		return;

	if (stat(path, &st))
		return;

	t = time(NULL);
	if (!(tm = localtime(&t)))
		return;

	if (!strftime(datetime_str, sizeof(datetime_str), "%Y%m%d.%H%M%S", tm))
		return;

	if (dm_snprintf(path2, sizeof(path2), "%s/devices/%s-unused.%s", cmd->system_dir, filename, datetime_str) < 0)
		return;

	if (rename(path, path2) < 0) {
		stack;
		return;
	}
	log_debug("Devices file moved to %s", path2);
}

int setup_devices_file(struct cmd_context *cmd)
{
	char dirpath[PATH_MAX];
	const char *filename = NULL;
	struct stat st;
	int rv;

	/* Use dmeventd.devices if it exists. */
	if (cmd->run_by_dmeventd && _setup_devices_file_dmeventd(cmd))
		return 1;

	if (cmd->devicesfile) {
		/* --devicesfile <filename> or "" has been set which overrides
		   lvm.conf settings use_devicesfile and devicesfile. */
		if (!strlen(cmd->devicesfile))
			cmd->enable_devices_file = 0;
		else {
			cmd->enable_devices_file = 1;
			filename = cmd->devicesfile;
		}
		/* TODO: print a warning if --devicesfile system.devices
		   while lvm.conf use_devicesfile=0. */
	} else {
		if (!find_config_tree_bool(cmd, devices_use_devicesfile_CFG, NULL))
			cmd->enable_devices_file = 0;
		else {
			cmd->enable_devices_file = 1;
			filename = find_config_tree_str(cmd, devices_devicesfile_CFG, NULL);
			if (!validate_name(filename)) {
				log_error("Invalid devices file name from config setting \"%s\".", filename);
				return 0;
			}
		}
	}

	if (!cmd->enable_devices_file) {
		struct dm_config_tree *cft;
		/* rename unused system.devices to system.devices.unused.<date>.<time> */
		if (!cmd->devicesfile &&
		    (cft = get_config_tree_by_source(cmd, CONFIG_MERGED_FILES)) &&
		    !find_config_bool(cmd, cft, devices_use_devicesfile_CFG))
			devices_file_rename_unused(cmd);
		return 1;
	}

	if (dm_snprintf(dirpath, sizeof(dirpath), "%s/devices", cmd->system_dir) < 0) {
		log_error("Failed to copy devices dir path");
		return 0;
	}

	if (stat(dirpath, &st)) {
		log_debug("Creating %s.", dirpath);
		dm_prepare_selinux_context(dirpath, S_IFDIR);
		rv = mkdir(dirpath, 0755);
		dm_prepare_selinux_context(NULL, 0);

		if ((rv < 0) && stat(dirpath, &st)) {
			log_error("Failed to create %s %d", dirpath, errno);
			return 0;
		}
	}
	
	if (dm_snprintf(cmd->devices_file_path, sizeof(cmd->devices_file_path),
			"%s/devices/%s", cmd->system_dir, filename) < 0) {
		log_error("Failed to copy devices file path");
		return 0;
	}

	return 1;
}

/*
 * Add all system devices to dev-cache, and attempt to
 * match all devices_file entries to dev-cache entries.
 */
int setup_devices(struct cmd_context *cmd)
{
	int file_exists;
	int lock_mode = 0;

	if (cmd->enable_devices_list) {
		if (!_setup_devices_list(cmd))
			return_0;
		goto scan;
	}

	if (!setup_devices_file(cmd))
		return_0;

	if (!cmd->enable_devices_file)
		goto scan;

	file_exists = devices_file_exists(cmd);

	/*
	 * Fail if user specifies a file name that doesn't exist and
	 * the command is not creating a new devices file.
	 */
	if (!file_exists && !cmd->create_edit_devices_file && cmd->devicesfile && strlen(cmd->devicesfile)) {
		log_error("Devices file not found: %s", cmd->devices_file_path);
		return 0;
	}

	/*
	 * Removing the devices file is another way of disabling the use of
	 * a devices file, unless the command creates the devices file.
	 */
	if (!file_exists && !cmd->create_edit_devices_file) {
		log_debug("Devices file not found, ignoring.");
		cmd->enable_devices_file = 0;
		goto scan;
	}

	/*
	 * Don't let pvcreate or vgcreate create a new system devices file
	 * unless it's specified explicitly with --devicesfile.  This avoids
	 * a problem where a system is running with existing PVs, and is
	 * not using a devices file based on the fact that the system
	 * devices file doesn't exist.  If the user simply uses pvcreate
	 * to create a new PV, they almost certainly do not want that to
	 * create a new system devices file containing the new PV and none
	 * of the existing PVs that the system is already using.
	 * However, if they use the vgimportdevices or lvmdevices command
	 * then they are clearly intending to use the devices file, so we
	 * can create it.  Or, if they specify a non-system devices file
	 * with pvcreate/vgcreate, then they clearly want to use a devices
	 * file and we can create it (and creating a non-system devices file 
	 * would not cause existing PVs to disappear from the main system.)
	 *
	 * An exception is if pvcreate/vgcreate get to device_id_write and
	 * did not see any existing VGs during label scan.  In that case
	 * they will create a new system devices file, since there will be
	 * no VGs that the new file would hide.
	 */
	if (cmd->create_edit_devices_file && !cmd->devicesfile && !file_exists &&
	    (!strncmp(cmd->name, "pvcreate", 8) || !strncmp(cmd->name, "vgcreate", 8))) {
		/* The command will decide in device_ids_write whether to create
		   a new system devices file. */
		cmd->enable_devices_file = 0;
		cmd->pending_devices_file = 1;
		goto scan;
	}

	if (!file_exists && cmd->sysinit) {
		cmd->enable_devices_file = 0;
		goto scan;
	}

	if (!file_exists) {
		/*
		 * pvcreate/vgcreate create a new devices file here if it
		 * doesn't exist.  They have create_edit_devices_file=1.
		 * First create/lock-ex the devices file lockfile.
		 * Other commands will not use a devices file if none exists.
		 */
		lock_mode = LOCK_EX;

		if (!lock_devices_file(cmd, lock_mode)) {
			log_error("Failed to lock the devices file to create.");
			return 0;
		}

		/* The file will be created in device_ids_write() */
		if (!devices_file_exists(cmd))
			goto scan;
	} else {
		/*
		 * Commands that intend to edit the devices file have
		 * edit_devices_file or create_edit_devices_file set (create if
		 * they can also create a new devices file) and lock it ex
		 * here prior to reading.  Other commands that intend to just
		 * read the devices file lock sh.
		 */
		lock_mode = (cmd->create_edit_devices_file || cmd->edit_devices_file) ? LOCK_EX : LOCK_SH;

		if (!lock_devices_file(cmd, lock_mode)) {
			log_error("Failed to lock the devices file.");
			return 0;
		}
	}

	/*
	 * Read the list of device ids that lvm can use.
	 * Adds a struct dev_id to cmd->use_devices for each one.
	 */
	if (!device_ids_read(cmd)) {
		log_error("Failed to read the devices file.");
		unlock_devices_file(cmd);
		return 0;
	}

	/*
	 * When the command is editing the devices file, it acquires
	 * the ex lock above, will later call device_ids_write(), and
	 * then unlock the lock after writing the file.
	 * When the command is just reading the devices file, it's
	 * locked sh above just before reading the file, and unlocked
	 * here after reading.
	 */
	if (lock_mode == LOCK_SH)
		unlock_devices_file(cmd);

 scan:
	/*
	 * Add a 'struct device' to dev-cache for each device available on the system.
	 * This will not open or read any devices, but may look at sysfs properties.
	 * This list of devs comes from looking /dev entries, or from asking libudev.
	 */
	dev_cache_scan(cmd);

	/*
	 * Match entries from cmd->use_devices with device structs in dev-cache.
	 */
	device_ids_match(cmd);

	return 1;
}

/*
 * The alternative to setup_devices() when the command is interested
 * in using only one PV.
 *
 * Add one system device to dev-cache, and attempt to
 * match its dev-cache entry to a devices_file entry.
 */
int setup_device(struct cmd_context *cmd, const char *devname)
{
	struct stat buf;
	struct device *dev;

	if (cmd->enable_devices_list) {
		if (!_setup_devices_list(cmd))
			return_0;
		goto scan;
	}

	if (!setup_devices_file(cmd))
		return_0;

	if (!cmd->enable_devices_file)
		goto scan;

	if (!devices_file_exists(cmd)) {
		log_debug("Devices file not found, ignoring.");
		cmd->enable_devices_file = 0;
		goto scan;
	}

	if (!lock_devices_file(cmd, LOCK_SH)) {
		log_error("Failed to lock the devices file to read.");
		return 0;
	}

	if (!device_ids_read(cmd)) {
		log_error("Failed to read the devices file.");
		unlock_devices_file(cmd);
		return 0;
	}

	unlock_devices_file(cmd);

 scan:
	if (stat(devname, &buf) < 0) {
		log_error("Cannot access device %s.", devname);
		return 0;
	}

	if (!S_ISBLK(buf.st_mode)) {
		log_error("Invalid device type %s.", devname);
		return 0;
	}

	if (!_insert_dev(devname, buf.st_rdev))
		return_0;

	if (!(dev = dev_cache_get_dev_by_name(devname)))
		return_0;

	/* Match this device to an entry in devices_file so it will not
	   be rejected by filter-deviceid. */
	if (cmd->enable_devices_file)
		device_ids_match_dev(cmd, dev);

	return 1;
}

/*
 * autoactivation is specialized/optimized to look only at command args,
 * so this just sets up the devices file, then individual devices are
 * added to dev-cache and matched with device_ids.
 */

int setup_devices_for_online_autoactivation(struct cmd_context *cmd)
{
	if (cmd->enable_devices_list) {
		if (!_setup_devices_list(cmd))
			return_0;
		return 1;
	}

	if (!setup_devices_file(cmd))
		return_0;

	if (!cmd->enable_devices_file)
		return 1;

	if (!devices_file_exists(cmd)) {
		log_debug("Devices file not found, ignoring.");
		cmd->enable_devices_file = 0;
		return 1;
	}

	if (!lock_devices_file(cmd, LOCK_SH)) {
		log_error("Failed to lock the devices file to read.");
		return 0;
	}

	if (!device_ids_read(cmd)) {
		log_error("Failed to read the devices file.");
		unlock_devices_file(cmd);
		return 0;
	}

	unlock_devices_file(cmd);
	return 1;
}


/* Get a device name from a devno. */

static char *_get_devname_from_devno(struct cmd_context *cmd, dev_t devno)
{
	static const char _partitions[] = "/proc/partitions";
	char path[PATH_MAX];
	char devname[PATH_MAX] = { 0 };
	char namebuf[NAME_LEN];
	char line[1024];
	unsigned major = MAJOR(devno);
	unsigned minor = MINOR(devno);
	unsigned line_major;
	unsigned line_minor;
	uint64_t line_blocks;
	DIR *dir;
	struct dirent *dirent;
	FILE *fp;

	if (!devno)
		return NULL;

	/*
	 * $ ls /sys/dev/block/8:0/device/block/
	 * sda
	 */
	if (major_is_scsi_device(cmd->dev_types, major)) {
		if (dm_snprintf(path, sizeof(path), "%sdev/block/%d:%d/device/block",
				dm_sysfs_dir(), major, minor) < 0) {
			return NULL;
		}

		if (!(dir = opendir(path)))
			goto try_partition;

		while ((dirent = readdir(dir))) {
			if (dirent->d_name[0] == '.')
				continue;
			if (dm_snprintf(devname, sizeof(devname), "/dev/%s", dirent->d_name) < 0) {
				devname[0] = '\0';
				stack;
			}
			break;
		}
		if (closedir(dir))
			log_sys_debug("closedir", path);

		if (devname[0]) {
			log_debug("Found %s for %d:%d from sys", devname, major, minor);
			return _strdup(devname);
		}
		return NULL;
	}

	/*
	 * $ cat /sys/dev/block/253:3/dm/name
	 * mpatha
	 */
	if (major == cmd->dev_types->device_mapper_major) {
		if (dm_snprintf(path, sizeof(path), "%sdev/block/%d:%d/dm/name",
				dm_sysfs_dir(), major, minor) < 0) {
			return NULL;
		}

		if (!get_sysfs_value(path, namebuf, sizeof(namebuf), 0))
			return NULL;

		if (!_sanitize_buffer(namebuf))
			return NULL;

		if (dm_snprintf(devname, sizeof(devname), "%s/%s", dm_dir(), namebuf) < 0)
			return_NULL;

		log_debug("Found %s for %d:%d from sys dm.", devname, major, minor);

		return _strdup(devname);
	}

	/*
	 * /proc/partitions lists
	 * major minor #blocks name
	 */

try_partition:
	if (!(fp = fopen(_partitions, "r")))
		return NULL;

	while (fgets(line, sizeof(line), fp)) {
		if (sscanf(line, "%u %u %llu %" DM_TO_STRING(NAME_LEN) "s",
			   &line_major, &line_minor, (unsigned long long *)&line_blocks, namebuf) != 4)
			continue;
		if (line_major != major)
			continue;
		if (line_minor != minor)
			continue;

		if (dm_snprintf(devname, sizeof(devname), "/dev/%s", namebuf) < 0) {
			devname[0] = '\0';
			stack;
		}
		break;
	}

	if (fclose(fp))
		log_sys_debug("fclose", _partitions);

	if (devname[0]) {
		log_debug("Found %s for %d:%d from %s", devname, major, minor, _partitions);
		return _strdup(devname);
	}

	/*
	 * If necessary, this could continue searching by stat'ing /dev entries.
	 */

	return NULL;
}

int setup_devname_in_dev_cache(struct cmd_context *cmd, const char *devname)
{
	struct stat buf;

	if (stat(devname, &buf) < 0) {
		log_error("Cannot access device %s.", devname);
		return 0;
	}

	if (!S_ISBLK(buf.st_mode)) {
		log_error("Invalid device type %s.", devname);
		return 0;
	}

	if (!_insert_dev(devname, buf.st_rdev))
		return_0;

	if (!dev_cache_get_dev_by_name(devname))
		return_0;

	return 1;
}

int setup_devno_in_dev_cache(struct cmd_context *cmd, dev_t devno)
{
	const char *devname;

	if (!(devname = _get_devname_from_devno(cmd, devno)))
		return_0;

	return setup_devname_in_dev_cache(cmd, devname);
}

struct device *setup_dev_in_dev_cache(struct cmd_context *cmd, dev_t devno, const char *devname)
{
	struct device *dev;
	struct stat buf;
	unsigned major = MAJOR(devno);
	unsigned minor = MINOR(devno);

	if (devname) {
		if (stat(devname, &buf) < 0) {
			log_error("Cannot access device %s for %u:%u.", devname, major, minor);
			if (!devno)
				return_NULL;
			if (!(devname = _get_devname_from_devno(cmd, devno))) {
				log_error("No device name found from %u:%u.", major, minor);
				return_NULL;
			}
			if (stat(devname, &buf) < 0) {
				log_error("Cannot access device %s from %u:%u.", devname, major, minor);
				return_NULL;
			}
		}
	} else {
		if (!(devname = _get_devname_from_devno(cmd, devno))) {
			log_error("No device name found from %u:%u.", major, minor);
			return_NULL;
		}
		if (stat(devname, &buf) < 0) {
			log_error("Cannot access device %s from %u:%u.", devname, major, minor);
			return_NULL;
		}
	}

	if (!S_ISBLK(buf.st_mode)) {
		log_error("Invalid device type %s.", devname);
		return_NULL;
	}

	if (devno && (buf.st_rdev != devno)) {
		log_warn("Found %s devno %u:%u expected %u:%u.", devname,
			  MAJOR(buf.st_rdev), MINOR(buf.st_rdev), major, minor);
	}

	if (!_insert_dev(devname, buf.st_rdev))
		return_NULL;

	if (!(dev = dev_cache_get_dev_by_name(devname))) {
		log_error("Device lookup failed for %u:%u %s", major, minor, devname);
		return_NULL;
	}

	return dev;
}
