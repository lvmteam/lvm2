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

#include "lib.h"
#include "dev-cache.h"
#include "pool.h"
#include "hash.h"
#include "list.h"
#include "lvm-types.h"
#include "btree.h"
#include "filter.h"
#include "filter-persistent.h"

#include <unistd.h>
#include <sys/param.h>
#include <dirent.h>

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

struct device *dev_create_file(const char *filename, struct device *dev,
			       struct str_list *alias)
{
	int allocate = !dev;

	if (allocate && !(dev = dbg_malloc(sizeof(*dev)))) {
		log_error("struct device allocation failed");
		return NULL;
	}
	if (allocate && !(alias = dbg_malloc(sizeof(*alias)))) {
		log_error("struct str_list allocation failed");
		dbg_free(dev);
		return NULL;
	}
	if (!(alias->str = dbg_strdup(filename))) {
		log_error("filename strdup failed");
		if (allocate) {
			dbg_free(dev);
			dbg_free(alias);
		}
		return NULL;
	}
	dev->flags = DEV_REGULAR;
	if (allocate)
		dev->flags |= DEV_ALLOCED;
	list_init(&dev->aliases);
	list_add(&dev->aliases, &alias->list);
	dev->end = UINT64_C(0);
	dev->dev = 0;
	dev->fd = -1;
	dev->open_count = 0;
	dev->block_size = -1;
	memset(dev->pvid, 0, sizeof(dev->pvid));
	list_init(&dev->open_list);

	return dev;
}

static struct device *_dev_create(dev_t d)
{
	struct device *dev;

	if (!(dev = _alloc(sizeof(*dev)))) {
		log_error("struct device allocation failed");
		return NULL;
	}
	dev->flags = 0;
	list_init(&dev->aliases);
	dev->dev = d;
	dev->fd = -1;
	dev->open_count = 0;
	dev->block_size = -1;
	dev->end = UINT64_C(0);
	memset(dev->pvid, 0, sizeof(dev->pvid));
	list_init(&dev->open_list);

	return dev;
}

/* Return 1 if we prefer path1 else return 0 */
static int _compare_paths(const char *path0, const char *path1)
{
	int slash0 = 0, slash1 = 0;
	const char *p;
	char p0[PATH_MAX], p1[PATH_MAX];
	char *s0, *s1;
	struct stat stat0, stat1;

	/* Return the path with fewer slashes */
	for (p = path0; p++; p = (const char *) strchr(p, '/'))
		slash0++;

	for (p = path1; p++; p = (const char *) strchr(p, '/'))
		slash1++;

	if (slash0 < slash1)
		return 0;
	if (slash1 < slash0)
		return 1;

	strncpy(p0, path0, PATH_MAX);
	strncpy(p1, path1, PATH_MAX);
	s0 = &p0[0] + 1;
	s1 = &p1[0] + 1;

	/* We prefer symlinks - they exist for a reason!
	 * So we prefer a shorter path before the first symlink in the name.
	 * FIXME Configuration option to invert this? */
	while (s0) {
		s0 = strchr(s0, '/');
		s1 = strchr(s1, '/');
		if (s0) {
			*s0 = '\0';
			*s1 = '\0';
		}
		if (lstat(p0, &stat0)) {
			log_sys_error("lstat", p0);
			return 1;
		}
		if (lstat(p1, &stat1)) {
			log_sys_error("lstat", p1);
			return 0;
		}
		if (S_ISLNK(stat0.st_mode) && !S_ISLNK(stat1.st_mode))
			return 0;
		if (!S_ISLNK(stat0.st_mode) && S_ISLNK(stat1.st_mode))
			return 1;
		if (s0) {
			*s0++ = '/';
			*s1++ = '/';
		}
	}

	/* ASCII comparison */
	if (strcmp(path0, path1) < 0)
		return 0;
	else
		return 1;
}

static int _add_alias(struct device *dev, const char *path)
{
	struct str_list *sl = _alloc(sizeof(*sl));
	struct list *ah;
	const char *oldpath;
	int prefer_old = 1;

	if (!sl) {
		stack;
		return 0;
	}

	/* Is name already there? */
	list_iterate(ah, &dev->aliases) {
		if (!strcmp(list_item(ah, struct str_list)->str, path)) {
			log_debug("%s: Already in device cache", path);
			return 1;
		}
	}

	if (!(sl->str = pool_strdup(_cache.mem, path))) {
		stack;
		return 0;
	}

	if (!list_empty(&dev->aliases)) {
		oldpath = list_item(dev->aliases.n, struct str_list)->str;
		prefer_old = _compare_paths(path, oldpath);
		log_debug("%s: Aliased to %s in device cache%s",
			  path, oldpath, prefer_old ? "" : " (preferred name)");

	} else
		log_debug("%s: Added to device cache", path);

	if (prefer_old)
		list_add(&dev->aliases, &sl->list);
	else
		list_add_h(&dev->aliases, &sl->list);

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
		if (!(dev = _dev_create(d))) {
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
		/* check it's not a symbolic link */
		if (lstat(path, &info) < 0) {
			log_sys_very_verbose("lstat", path);
			return 0;
		}

		if (S_ISLNK(info.st_mode)) {
			log_debug("%s: Symbolic link to directory", path);
			return 0;
		}

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

static void _full_scan(int dev_scan)
{
	struct list *dh;

	if (_cache.has_scanned && !dev_scan)
		return;

	list_iterate(dh, &_cache.dirs) {
		struct dir_list *dl = list_item(dh, struct dir_list);
		_insert_dir(dl->dir);
	};

	_cache.has_scanned = 1;
	init_full_scan_done(1);
}

int dev_cache_has_scanned(void)
{
	return _cache.has_scanned;
}

void dev_cache_scan(int do_scan)
{
	if (!do_scan)
		_cache.has_scanned = 1;
	else
		_full_scan(1);
}

int dev_cache_init(void)
{
	_cache.names = NULL;
	_cache.has_scanned = 0;

	if (!(_cache.mem = pool_create("dev_cache", 10 * 1024))) {
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
	if (_cache.names)
		_check_for_open_devices();

	if (_cache.mem) {
		pool_destroy(_cache.mem);
		_cache.mem = NULL;
	}

	if (_cache.names) {
		hash_destroy(_cache.names);
		_cache.names = NULL;
	}

	_cache.devices = NULL;
	_cache.has_scanned = 0;
	list_init(&_cache.dirs);
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

	if (!(dl = _alloc(sizeof(*dl) + strlen(path) + 1))) {
		log_error("dir_list allocation failed");
		return 0;
	}

	strcpy(dl->dir, path);
	list_add(&_cache.dirs, &dl->list);
	return 1;
}

/* Check cached device name is still valid before returning it */
/* This should be a rare occurrence */
/* set quiet if the cache is expected to be out-of-date */
/* FIXME Make rest of code pass/cache struct device instead of dev_name */
const char *dev_name_confirmed(struct device *dev, int quiet)
{
	struct stat buf;
	const char *name;
	int r;

	while ((r = stat(name = list_item(dev->aliases.n,
					  struct str_list)->str, &buf)) ||
	       (buf.st_rdev != dev->dev)) {
		if (r < 0) {
			if (quiet)
				log_sys_debug("stat", name);
			else
				log_sys_error("stat", name);
		}
		if (quiet)
			log_debug("Path %s no longer valid for device(%d,%d)",
				  name, (int) MAJOR(dev->dev),
				  (int) MINOR(dev->dev));
		else
			log_error("Path %s no longer valid for device(%d,%d)",
				  name, (int) MAJOR(dev->dev),
				  (int) MINOR(dev->dev));

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

struct dev_iter *dev_iter_create(struct dev_filter *f, int dev_scan)
{
	struct dev_iter *di = dbg_malloc(sizeof(*di));

	if (!di) {
		log_error("dev_iter allocation failed");
		return NULL;
	}


	if (dev_scan) {
		/* Flag gets reset between each command */
		if (!full_scan_done())
			persistent_filter_wipe(f); /* Calls _full_scan(1) */
	} else
		_full_scan(0);

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
