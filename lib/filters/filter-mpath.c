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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "filter.h"
#include "filter-mpath.h"
#include "activate.h"

#ifdef linux
#include <dirent.h>

#define MPATH_PREFIX "mpath-"

static const char *get_sysfs_name(struct device *dev)
{
	const char *name;

	name = strrchr(dev_name(dev), '/');
	if (!name)
		return NULL;
	name++;

	if (!*name)
		return NULL;

	return name;
}

static int get_sysfs_string(const char *path, char *buffer, int max_size)
{
	FILE *fp;
	int r = 0;

	if (!(fp = fopen(path, "r")))
		return_0;

	if (!fgets(buffer, max_size, fp))
		stack;
	else
		r = 1;

	if (fclose(fp))
		stack;

	return r;
}

static int get_sysfs_get_major_minor(const char *sysfs_dir, const char *kname, int *major, int *minor)
{
	char path[PATH_MAX], buffer[64];

	if (dm_snprintf(path, sizeof(path), "%s/block/%s/dev", sysfs_dir, kname) < 0)
		return_0;

	if (!get_sysfs_string(path, buffer, sizeof(buffer)))
		return 0;

	if (sscanf(buffer, "%d:%d", major, minor) != 2)
		return 0;

	return 1;
}

static int get_parent_mpath(const char *dir, char *name, int max_size)
{
	struct dirent *d;
	DIR *dr;
	int r = 0;

	if (!(dr = opendir(dir)))
		return_0;

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
		stack;

	return r;
}

static int dev_is_mpath(struct dev_filter *f, struct device *dev)
{
	const char *name;
	char path[PATH_MAX+1];
	char parent_name[PATH_MAX+1];
	struct stat info;
	const char *sysfs_dir = f->private;
	int major, minor, r;

	/* Limit this filter only to SCSI devices */
	if (!major_is_scsi_device(MAJOR(dev->dev)))
		return 0;

	name = get_sysfs_name(dev);
	if (!name)
		return_0;

	r = dm_snprintf(path, PATH_MAX, "%s/block/%s/holders", sysfs_dir, name);
	if (r < 0)
		return_0;

	/* also will filter out partitions */
	if (stat(path, &info) == -1 || !S_ISDIR(info.st_mode))
		return 0;

	if (!get_parent_mpath(path, parent_name, PATH_MAX))
		return 0;

	if (!get_sysfs_get_major_minor(sysfs_dir, parent_name, &major, &minor))
		return 0;

	if (major != dm_major())
		return 0;

	return lvm_dm_prefix_check(major, minor, MPATH_PREFIX);
}

static int _ignore_mpath(struct dev_filter *f, struct device *dev)
{
	if (dev_is_mpath(f, dev) == 1) {
		log_debug("%s: Skipping mpath component device", dev_name(dev));
		return 0;
	}

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying mpath filter while in use %u times.", f->use_count);

	dm_free(f->private);
	dm_free(f);
}

struct dev_filter *mpath_filter_create(const char *sysfs_dir)
{
	struct dev_filter *f;

	if (!*sysfs_dir) {
		log_verbose("No proc filesystem found: skipping multipath filter");
		return NULL;
	}

	if (!(f = dm_malloc(sizeof(*f)))) {
		log_error("mpath filter allocation failed");
		return NULL;
	}

	f->passes_filter = _ignore_mpath;
	f->destroy = _destroy;
	f->use_count = 0;
	f->private = dm_strdup(sysfs_dir);

	return f;
}

#else

struct dev_filter *mpath_filter_create(const char *sysfs_dir __attribute__((unused)))
{
	return NULL;
}

#endif
