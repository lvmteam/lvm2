/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dev-cache.h"
#include "log.h"
#include "pool.h"
#include "hash.h"
#include "list.h"
#include "dbg_malloc.h"

#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/param.h>
#include <dirent.h>

/*
 * FIXME: really need to seperate names from the devices since
 * multiple names can point to the same device.
 */

struct dev_iter {
	struct hash_node *current;
	struct dev_filter *filter;
};

struct dir_list {
	struct list_head list;
	char dir[0];
};

static struct {
	struct pool *mem;
	struct hash_table *devices;

	int has_scanned;
	struct list_head dirs;

} _cache;


#define _alloc(x) pool_alloc(_cache.mem, (x))
#define _free(x) pool_free(_cache.mem, (x))

static int _dir_scan(const char *dir);

/*
 * return a new path for the destination of the path.
 */
static char *_follow_link(const char *path, struct stat *info)
{
	char buffer[PATH_MAX + 1];
	int n;
	n = readlink(path, buffer, sizeof(buffer) - 1);

	if (n <= 0)
		return NULL;

	buffer[n] = '\0';

	if (stat(buffer, info) < 0) {
		log_sys_very_verbose("stat", buffer);
		return NULL;
	}

	return pool_strdup(_cache.mem, buffer);
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

static struct device *_create_dev(const char *path, struct stat *info)
{
	struct device *dev;
	char *name = pool_strdup(_cache.mem, path);

	if (!name) {
		stack;
		return NULL;
	}

	_collapse_slashes(name);

	if (!(dev = _alloc(sizeof(*dev)))) {
		stack;
		goto bad;
	}

	dev->name = name;
	dev->dev = info->st_rdev;
	return dev;

 bad:
	_free(name);
	return NULL;
}

static int _insert(const char *path, int recurse)
{
	struct stat info;
	struct device *dev;

	//log_very_verbose("dev-cache adding %s", path);

	if (stat(path, &info) < 0) {
		log_sys_very_verbose("stat", path);
		return 0;
	}

	if (S_ISDIR(info.st_mode)) {
		if (recurse)
			return _dir_scan(path);

		return 0;
	}

	if (S_ISLNK(info.st_mode)) {
		//log_debug("%s is a symbolic link, following", path);
		if (!(path = _follow_link(path, &info))) {
			stack;
			return 0;
		}
	}

	if (!S_ISBLK(info.st_mode)) {
		//log_debug("%s is not a block device", path);
		return 0;
	}

	if (!(dev = _create_dev(path, &info))) {
		stack;
		return 0;
	}

	hash_insert(_cache.devices, path, dev);
	return 1;
}

static char *_join(const char *dir, const char *name)
{
	int len = strlen(dir) + strlen(name) + 2;
	char *r = dbg_malloc(len);
	if (r)
		snprintf(r, len, "%s/%s", dir, name);

	return r;
}

static int _dir_scan(const char *dir)
{
	int n, dirent_count;
	struct dirent **dirent;
	char *path;

	dirent_count = scandir(dir, &dirent, NULL, alphasort);
	if (dirent_count > 0) {
		for (n = 0; n < dirent_count; n++) {
			if (dirent[n]->d_name[0] == '.') {
				free(dirent[n]);
				continue;
			}

			if ((path = _join(dir, dirent[n]->d_name)))
				_insert(path, 1);

			dbg_free(path);
			free(dirent[n]);
		}
		free(dirent);
	}

	return 1;
}

static void _full_scan(void)
{
	struct list_head *tmp;

	if (_cache.has_scanned)
		return;

	list_for_each(tmp, &_cache.dirs) {
		struct dir_list *dl = list_entry(tmp, struct dir_list, list);
		_dir_scan(dl->dir);
	}

	_cache.has_scanned = 1;
}

int dev_cache_init(void)
{
	if (!(_cache.mem = pool_create(10 * 1024))) {
		stack;
		return 0;
	}

	if (!(_cache.devices = hash_create(128))) {
		stack;
		pool_destroy(_cache.mem);
		_cache.mem = 0;
		return 0;
	}

	INIT_LIST_HEAD(&_cache.dirs);

	return 1;
}

void dev_cache_exit(void)
{
	pool_destroy(_cache.mem);
	hash_destroy(_cache.devices);
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
	list_add(&dl->list, &_cache.dirs);
	return 1;
}

struct device *dev_cache_get(const char *name, struct dev_filter *f)
{
	struct device *d = (struct device *) hash_lookup(_cache.devices, name);

	if (!d) {
		_insert(name, 0);
		d = (struct device *) hash_lookup(_cache.devices, name);
	}

	return (d && (!f || f->passes_filter(f, d))) ? d : NULL;
}

struct dev_iter *dev_iter_create(struct dev_filter *f)
{
	struct dev_iter *di = dbg_malloc(sizeof(*di));

	if (!di)
		return NULL;

	_full_scan();
	di->current = hash_get_first(_cache.devices);
	di->filter = f;

	return di;
}

void dev_iter_destroy(struct dev_iter *iter)
{
	dbg_free(iter);
}

static inline struct device *_iter_next(struct dev_iter *iter)
{
	struct device *d = hash_get_data(_cache.devices, iter->current);
	iter->current = hash_get_next(_cache.devices, iter->current);
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



