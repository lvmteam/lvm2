/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "dev-cache.h"
#include "pool.h"
#include "hash.h"
#include "list.h"
#include "lvm-types.h"
#include "btree.h"

#include <sys/stat.h>
#include <unistd.h>
#include <sys/param.h>
#include <dirent.h>
#include <linux/kdev_t.h>

/*
 * FIXME: really need to seperate names from the devices since
 * multiple names can point to the same device.
 */

struct dev_iter {
	struct btree_iter *current;
	struct dev_filter *filter;
};

struct dir_list {
	struct list list;
	char dir[0];
};

static struct {
	struct pool *mem;
	struct hash_table *names;
	struct btree *devices;

	int has_scanned;
	struct list dirs;

} _cache;

#define _alloc(x) pool_alloc(_cache.mem, (x))
#define _free(x) pool_free(_cache.mem, (x))

static int _insert(const char *path, int rec);

static struct device *_create_dev(dev_t d)
{
	struct device *dev;

	if (!(dev = _alloc(sizeof(*dev)))) {
		stack;
		return NULL;
	}

	list_init(&dev->aliases);
	dev->dev = d;
	dev->fd = -1;
	dev->flags = 0;
	memset(dev->pvid, 0, sizeof(dev->pvid));
	return dev;
}

static int _add_alias(struct device *dev, const char *path)
{
	struct str_list *sl = _alloc(sizeof(*sl));

	if (!sl) {
		stack;
		return 0;
	}

	if (!(sl->str = pool_strdup(_cache.mem, path))) {
		stack;
		return 0;
	}

	list_add(&dev->aliases, &sl->list);
	return 1;
}

/*
 * Either creates a new dev, or adds an alias to
 * an existing dev.
 */
static int _insert_dev(const char *path, dev_t d)
{
	struct device *dev;

	/* is this device already registered ? */
	if (!(dev = (struct device *) btree_lookup(_cache.devices,
						   (uint32_t) d))) {
		/* create new device */
		if (!(dev = _create_dev(d))) {
			stack;
			return 0;
		}

		if (!(btree_insert(_cache.devices, (uint32_t) d, dev))) {
			log_err("Couldn't insert device into binary tree.");
			_free(dev);
			return 0;
		}
	}

	if (!_add_alias(dev, path)) {
		log_err("Couldn't add alias to dev cache.");
		return 0;
	}

	if (!hash_insert(_cache.names, path, dev)) {
		log_err("Couldn't add name to hash in dev cache.");
		return 0;
	}

	return 1;
}

static char *_join(const char *dir, const char *name)
{
	size_t len = strlen(dir) + strlen(name) + 2;
	char *r = dbg_malloc(len);
	if (r)
		snprintf(r, len, "%s/%s", dir, name);

	return r;
}

/*
 * Get rid of extra slashes in the path string.
 */
static void _collapse_slashes(char *str)
{
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

	*str = *ptr;
}

static int _insert_dir(const char *dir)
{
	int n, dirent_count, r = 1;
	struct dirent **dirent;
	char *path;

	dirent_count = scandir(dir, &dirent, NULL, alphasort);
	if (dirent_count > 0) {
		for (n = 0; n < dirent_count; n++) {
			if (dirent[n]->d_name[0] == '.') {
				free(dirent[n]);
				continue;
			}

			if (!(path = _join(dir, dirent[n]->d_name))) {
				stack;
				return 0;
			}

			_collapse_slashes(path);
			r &= _insert(path, 1);
			dbg_free(path);

			free(dirent[n]);
		}
		free(dirent);
	}

	return r;
}

static int _insert(const char *path, int rec)
{
	struct stat info;
	int r = 0;

	if (stat(path, &info) < 0) {
		log_sys_very_verbose("stat", path);
		return 0;
	}

	if (S_ISDIR(info.st_mode)) {	/* add a directory */
		if (rec)
			r = _insert_dir(path);

	} else {		/* add a device */
		if (!S_ISBLK(info.st_mode)) {
			log_debug("%s: Not a block device", path);
			return 0;
		}

		if (!_insert_dev(path, info.st_rdev)) {
			stack;
			return 0;
		}

		r = 1;
	}

	return r;
}

static void _full_scan(void)
{
	struct list *dh;

	if (_cache.has_scanned)
		return;

	list_iterate(dh, &_cache.dirs) {
		struct dir_list *dl = list_item(dh, struct dir_list);
		_insert_dir(dl->dir);
	};

	_cache.has_scanned = 1;
}

int dev_cache_has_scanned(void)
{
	return _cache.has_scanned;
}

void dev_cache_scan(int do_scan)
{
	if (!do_scan)
		_cache.has_scanned = 1;
	else {
		_cache.has_scanned = 0;
		_full_scan();
	}
}

int dev_cache_init(void)
{
	_cache.names = NULL;

	if (!(_cache.mem = pool_create(10 * 1024))) {
		stack;
		return 0;
	}

	if (!(_cache.names = hash_create(128))) {
		stack;
		pool_destroy(_cache.mem);
		_cache.mem = 0;
		return 0;
	}

	if (!(_cache.devices = btree_create(_cache.mem))) {
		log_err("Couldn't create binary tree for dev-cache.");
		goto bad;
	}

	list_init(&_cache.dirs);

	return 1;

      bad:
	dev_cache_exit();
	return 0;
}

static void _check_closed(struct device *dev)
{
	if (dev->fd >= 0)
		log_err("Device '%s' has been left open.", dev_name(dev));
}

static inline void _check_for_open_devices(void)
{
	hash_iter(_cache.names, (iterate_fn) _check_closed);
}

void dev_cache_exit(void)
{
	_check_for_open_devices();

	pool_destroy(_cache.mem);
	if (_cache.names)
		hash_destroy(_cache.names);
}

int dev_cache_add_dir(const char *path)
{
	struct dir_list *dl;
	struct stat st;

	if (stat(path, &st)) {
		log_error("Ignoring %s: %s", path, strerror(errno));
		/* But don't fail */
		return 1;
	}

	if (!S_ISDIR(st.st_mode)) {
		log_error("Ignoring %s: Not a directory", path);
		return 1;
	}

	if (!(dl = _alloc(sizeof(*dl) + strlen(path) + 1)))
		return 0;

	strcpy(dl->dir, path);
	list_add(&_cache.dirs, &dl->list);
	return 1;
}

/* Check cached device name is still valid before returning it */
/* This should be a rare occurrence */
/* FIXME Make rest of code pass/cache struct device instead of dev_name */
const char *dev_name_confirmed(struct device *dev)
{
	struct stat buf;
	char *name;
	int r;

	while ((r = stat(name = list_item(dev->aliases.n,
					  struct str_list)->str, &buf)) ||
	       (buf.st_rdev != dev->dev)) {
		if (r < 0)
			log_sys_error("stat", name);
		log_error("Path %s no longer valid for device(%d,%d)",
			  name, (int) MAJOR(dev->dev), (int) MINOR(dev->dev));

		/* Remove the incorrect hash entry */
		hash_remove(_cache.names, name);

		/* Leave list alone if there isn't an alternative name */
		/* so dev_name will always find something to return. */
		/* Otherwise add the name to the correct device. */
		if (list_size(&dev->aliases) > 1) {
			list_del(dev->aliases.n);
			if (!r)
				_insert(name, 0);
			continue;
		}

		log_error("Aborting - please provide new pathname for what "
			  "used to be %s", name);
		return NULL;
	}

	return dev_name(dev);
}

struct device *dev_cache_get(const char *name, struct dev_filter *f)
{
	struct stat buf;
	struct device *d = (struct device *) hash_lookup(_cache.names, name);

	/* If the entry's wrong, remove it */
	if (d && (stat(name, &buf) || (buf.st_rdev != d->dev))) {
		hash_remove(_cache.names, name);
		d = NULL;
	}

	if (!d) {
		_insert(name, 0);
		d = (struct device *) hash_lookup(_cache.names, name);
	}

	return (d && (!f || f->passes_filter(f, d))) ? d : NULL;
}

struct dev_iter *dev_iter_create(struct dev_filter *f)
{
	struct dev_iter *di = dbg_malloc(sizeof(*di));

	if (!di)
		return NULL;

	_full_scan();
	di->current = btree_first(_cache.devices);
	di->filter = f;

	return di;
}

void dev_iter_destroy(struct dev_iter *iter)
{
	dbg_free(iter);
}

static inline struct device *_iter_next(struct dev_iter *iter)
{
	struct device *d = btree_get_data(iter->current);
	iter->current = btree_next(iter->current);
	return d;
}

struct device *dev_iter_get(struct dev_iter *iter)
{
	while (iter->current) {
		struct device *d = _iter_next(iter);
		if (!iter->filter ||
		    iter->filter->passes_filter(iter->filter, d))
			return d;
	}

	return NULL;
}
