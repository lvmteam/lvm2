/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dev-cache.h"
#include "log.h"
#include "pool.h"
#include "hash.h"

#include <stdlib.h>

/*
 * FIXME: really need to seperate names from the devices since
 * multiple names can point to the same device.
 */

struct dev_iter {
	struct hash_node *current;
	struct dev_filter *filter;
};

struct dir_list {
	struct list_head dir_list;
	char *dir[0];
};

static struct {
	struct pool *mem;
	struct hash_table *devices;

	int has_scanned;
	struct list_head dirs;

} _cache;


#define _alloc(x) pool_alloc(_cache.mem, (x))
#define _free(x) pool_free(_cache.mem, (x))

/*
 * return a new path for the destination of the path.
 */
static char * _follow_link(const char *path, struct stat *info)
{
	char buffer[PATH_MAX + 1], *r;
	int n;
	n = readlink(path, buffer);

	if (n <= 0)
		return NULL;

	buffer[n] = '\0';

	if (stat(buffer, info) < 0) {
		log_sys_err("stat");
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

static struct device *_create_dev(const char *path)
{
	struct stat info;
	struct device *dev;
	char *normed = _collapse_slashes(path);

	if (!normed) {
		stack;
		return NULL;
	}

	if (stat(normed, &info) < 0) {
		log_sys_error("stat");
		return NULL;
	}

	if (S_ISLNK(info.st_mode)) {
		char *new_path;
		log_debug("%s is a symbolic link, following\n", normed);
		normed = _follow_link(normed, &info);
		if (!normed)
			return NULL;
	}

	if (!S_ISBLK(info.st_mode)) {
		log_debug("%s is not a block device\n", normed);
		return NULL;
	}

	if (!(dev = _alloc(sizeof(*dev)))) {
		stack;
		return NULL;
	}

	dev->name = normed;
	dev->dev = info.st_rdev;
	return dev;
}

static struct device *_add(const char *dir, const char *path)
{
	struct device *d;
	int len = strlen(dir) + strlen(path) + 2;
	char *buffer = _alloc(len);

	snprintf(buffer, len, "%s/%s", path);
	d = dev_cache_get(path);

	if (!d)
		_free(buffer);	/* pool_free is safe in this case */

	return d;
}

static int _dir_scan(const char *dir)
{
	int n, dirent_count;
	struct dirent **dirent;

	dirent_count = scandir(dir, &dirent, NULL, alphasort);
	if (dirent_count > 0) {
		for (n = 0; n < dirent_count; n++) {
			_add(dir, dirent[n]->d_name);
			free(dirent[n]);
		}
		free(dirent);
	}

	return 1;
}

static void _full_scan(void)
{
	struct dir_list *dl;

	if (_cache.has_scanned)
		return;

	for (dl = _cache.dirs.next; dl != &_cache.dirs; dl = dl->next)
		_dir_scan(dl.dir);

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

	if (!(dl = _alloc(sizeof(*dl) + strlen(path) + 1)))
		return 0;

	strcpy(dl->dir, path);
	list_add(dl, _cache.directories);
	return 1;
}

struct device *dev_cache_get(const char *name)
{
	struct device *d = hash_lookup(_cache.devices, name);
	if (!d && (d = _create_device(name)))
		hash_insert(t, name, d);

	return d;
}

struct dev_iter *dev_iter_create(struct dev_filter *f)
{
	struct dev_iter *di = dbg_malloc(sizeof(*di));

	if (!di)
		return NULL;

	_full_scan();
	di->current = hash_get_first();
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
	iter->current = hash_next(_cache.devices, iter->current);
	return d;
}

struct device *dev_iter_get(struct dev_iter *iter)
{
	while (iter->current) {
		struct device *d = _iter_next(iter);
		if (!iter->filter ||
		    iter->filter->pass_filter(iter->filter, d))
			return d;
	}

	return NULL;
}



