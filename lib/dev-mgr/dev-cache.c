/*
 * tools/lib/dev-manager.h
 *
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 */

#include "dev-cache.h"
#include "log.h"
#include "pool.h"
#include "hash.h"

#include <stdlib.h>

#define DEV_PATH_LEN 256

struct d_internal {
	struct device dev;

	int count;

	int rw;
	FILE *fp;
};

struct {
	static pool *mem;
	struct hash_table *devices;
} _cache;

static inline struct d_internal *_alloc_dev()
{
	return pool_alloc(_cache.mem, sizeof(struct d_internal));
}

static struct d_internal *_create_dev(const char *path)
{
	/* FIXME: follow sym links */
	struct stat info;
	char norm[DEV_PATH_LEN];
	_normalise_path(norm, path);

	if (stat(path, &info) < 0) {
		log_sys_error("stat");
		return 0;
	}

	/* FIXME: lost the will to program */
	if () {

	}
}


#define DEV_READ 0
#define DEV_WRITE 1
static inline struct FILE *_open_read(const char *path)
{
	return fopen(path, "r");
}

static inline struct FILE *_open_write(const char *path)
{
	return fopen(path, "rw");
}

static int _check_open(struct d_internal *di, int rw)
{
	/* is it already open ? */
	if (dev->fp)
		return 0;

	if (di->fp) {
		/* do we need to upgrade a read ? */
		if (di->rw == DEV_READ && rw == DEV_WRITE) {
			struct FILE *fp = _open_write(di->dev.path);
			if (!fp)
				return 0;

			fclose(di->fp);
			di->fp  = fp;
		}
	} else if (rw == DEV_READ)
		di->fp = _open_read(di->dev.path);
	else
		di->fp = _open_write(di->dev.path);

	return di->fp ? 1 : 0;
}

int init_dev_cache()
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

static void _close_device(void *d)
{
	struct d_internal *di = (struct d_internal *) d;
	if (di->fp)
		fclose(di->fp);
}

void exit_dev_cache()
{
	pool_destroy(_cache.mem);
	hash_iterate(_cache.devices, _close_devices);
	hash_destroy(_cache.devices);
}

struct device *get_device(const char *name)
{
	struct d_internal *di = hash_lookup(_cache.devices, name);
	if (di)
		di->count++;
	else {
		di = _create_device(name);

		if (di)
			hash_insert(t, name, di);
	}

	return di ? &di->d : 0;
}

void put_device(struct device *d)
{
	struct d_internal *di = (struct d_internal *) d;
	if (--di->count < 0)
		log_error("device reference count < 0");

	else if (di->count == 0)
		_close_device(di);
}

ssize_t dev_read(struct device *dev, size_t offset, size_t len, void *buffer)
{
	struct d_internal *di = (struct d_internal *) dev;

	if (!_check_open(di, DEV_READ)) {
		stack;
		return -EPERM;
	}

	fseek(dev->fp, offset, SEEK_SET);
	return fread(buffer, 1, len, dev->fp);

}

ssize_t dev_write(struct device *dev, size_t offset, size_t len, void *buffer)
{
	struct d_internal *di = (struct d_internal *) dev;

	if (!_check_open(di, DEV_WRITE)) {
		stack;
		return -EPERM;
	}

	fseek(dev->fp, offset, SEEK_SET);
	return fwrite(buffer, 1, len, dev->fp);
}
