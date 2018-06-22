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

#include "lib.h"
#include "filter.h"
#include "config.h"
#include "lvm-file.h"

struct pfilter {
	char *file;
	struct dm_hash_table *devices;
	struct dev_filter *real;
	struct timespec ctime;
	struct dev_types *dt;
};

/*
 * The persistent filter is filter layer that sits above the other filters and
 * caches the final result of those other filters.  When a device is first
 * checked against filters, it will not be in this cache, so this filter will
 * pass the device down to the other filters to check it.  The other filters
 * will run and either include the device (good/pass) or exclude the device
 * (bad/fail).  That good or bad result propagates up through this filter which
 * saves the result.  The next time some code checks the filters against the
 * device, this persistent/cache filter is checked first.  This filter finds
 * the previous result in its cache and returns it without reevaluating the
 * other real filters.
 *
 * FIXME: a cache like this should not be needed.  The fact it's needed is a
 * symptom of code that should be fixed to not reevaluate filters multiple
 * times.  A device should be checked against the filter once, and then not
 * need to be checked again.  With scanning now controlled, we could probably
 * do this.
 *
 * FIXME: "persistent" isn't a great name for this caching filter.  This filter
 * at one time saved its cache results to a file, which is how it got the name.
 * That .cache file does not work well, causes problems, and is no longer used
 * by default.  The old code for it should be removed.
 */

static char* _good_device = "good";
static char* _bad_device = "bad";

/*
 * The hash table holds one of these two states
 * against each entry.
 */
#define PF_BAD_DEVICE ((void *) &_good_device)
#define PF_GOOD_DEVICE ((void *) &_bad_device)

static int _init_hash(struct pfilter *pf)
{
	if (pf->devices)
		dm_hash_destroy(pf->devices);

	if (!(pf->devices = dm_hash_create(128)))
		return_0;

	return 1;
}

static void _persistent_filter_wipe(struct dev_filter *f)
{
	struct pfilter *pf = (struct pfilter *) f->private;

	dm_hash_wipe(pf->devices);
}

static int _read_array(struct pfilter *pf, struct dm_config_tree *cft,
		       const char *path, void *data)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;

	if (!(cn = dm_config_find_node(cft->root, path))) {
		log_very_verbose("Couldn't find %s array in '%s'",
				 path, pf->file);
		return 0;
	}

	/*
	 * iterate through the array, adding
	 * devices as we go.
	 */
	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != DM_CFG_STRING) {
			log_verbose("Devices array contains a value "
				    "which is not a string ... ignoring");
			continue;
		}

		if (!dm_hash_insert(pf->devices, cv->v.str, data))
			log_verbose("Couldn't add '%s' to filter ... ignoring",
				    cv->v.str);
		/* Populate dev_cache ourselves */
		dev_cache_get(cv->v.str, NULL);
	}
	return 1;
}

int persistent_filter_load(struct dev_filter *f, struct dm_config_tree **cft_out)
{
	struct pfilter *pf = (struct pfilter *) f->private;
	struct dm_config_tree *cft;
	struct stat info;
	int r = 0;

	if (obtain_device_list_from_udev()) {
		if (!stat(pf->file, &info)) {
			log_very_verbose("Obtaining device list from udev. "
					 "Removing obsolete %s.",
					 pf->file);
			if (unlink(pf->file) < 0 && errno != EROFS)
				log_sys_error("unlink", pf->file);
		}
		return 1;
	}

	if (!stat(pf->file, &info))
		lvm_stat_ctim(&pf->ctime, &info);
	else {
		log_very_verbose("%s: stat failed: %s", pf->file,
				 strerror(errno));
		return_0;
	}

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, pf->file, 1)))
		return_0;

	if (!config_file_read(cft))
		goto_out;

	log_debug_devs("Loading persistent filter cache from %s", pf->file);
	_read_array(pf, cft, "persistent_filter_cache/valid_devices",
		    PF_GOOD_DEVICE);
	/* We don't gain anything by holding invalid devices */
	/* _read_array(pf, cft, "persistent_filter_cache/invalid_devices",
	   PF_BAD_DEVICE); */

	log_very_verbose("Loaded persistent filter cache from %s", pf->file);

      out:
	if (r && cft_out)
		*cft_out = cft;
	else
		config_destroy(cft);
	return r;
}

static void _write_array(struct pfilter *pf, FILE *fp, const char *path,
			 void *data)
{
	void *d;
	int first = 1;
	char buf[2 * PATH_MAX];
	struct dm_hash_node *n;

	for (n = dm_hash_get_first(pf->devices); n;
	     n = dm_hash_get_next(pf->devices, n)) {
		d = dm_hash_get_data(pf->devices, n);

		if (d != data)
			continue;

		if (!first)
			fprintf(fp, ",\n");
		else {
			fprintf(fp, "\t%s=[\n", path);
			first = 0;
		}

		dm_escape_double_quotes(buf, dm_hash_get_key(pf->devices, n));
		fprintf(fp, "\t\t\"%s\"", buf);
	}

	if (!first)
		fprintf(fp, "\n\t]\n");
}

static int _persistent_filter_dump(struct dev_filter *f, int merge_existing)
{
	struct pfilter *pf;
	char *tmp_file;
	struct stat info, info2;
	struct timespec ts;
	struct dm_config_tree *cft = NULL;
	FILE *fp;
	int lockfd;
	int r = 0;

	if (obtain_device_list_from_udev())
		return 1;

	if (!f)
		return_0;
	pf = (struct pfilter *) f->private;

	if (!dm_hash_get_num_entries(pf->devices)) {
		log_very_verbose("Internal persistent device cache empty "
				 "- not writing to %s", pf->file);
		return 1;
	}
	if (!dev_cache_has_scanned()) {
		log_very_verbose("Device cache incomplete - not writing "
				 "to %s", pf->file);
		return 0;
	}

	log_very_verbose("Dumping persistent device cache to %s", pf->file);

	while (1) {
		if ((lockfd = fcntl_lock_file(pf->file, F_WRLCK, 0)) < 0)
			return_0;

		/*
		 * Ensure we locked the file we expected
		 */
		if (fstat(lockfd, &info)) {
			log_sys_error("fstat", pf->file);
			goto out;
		}
		if (stat(pf->file, &info2)) {
			log_sys_error("stat", pf->file);
			goto out;
		}

		if (is_same_inode(info, info2))
			break;
	
		fcntl_unlock_file(lockfd);
	}

	/*
	 * If file contents changed since we loaded it, merge new contents
	 */
	lvm_stat_ctim(&ts, &info);
	if (merge_existing && timespeccmp(&ts, &pf->ctime, !=))
		/* Keep cft open to avoid losing lock */
		persistent_filter_load(f, &cft);

	tmp_file = alloca(strlen(pf->file) + 5);
	sprintf(tmp_file, "%s.tmp", pf->file);

	if (!(fp = fopen(tmp_file, "w"))) {
		/* EACCES has been reported over NFS */
		if (errno != EROFS && errno != EACCES)
			log_sys_error("fopen", tmp_file);
		goto out;
	}

	fprintf(fp, "# This file is automatically maintained by lvm.\n\n");
	fprintf(fp, "persistent_filter_cache {\n");

	_write_array(pf, fp, "valid_devices", PF_GOOD_DEVICE);
	/* We don't gain anything by remembering invalid devices */
	/* _write_array(pf, fp, "invalid_devices", PF_BAD_DEVICE); */

	fprintf(fp, "}\n");
	if (lvm_fclose(fp, tmp_file))
		goto_out;

	if (rename(tmp_file, pf->file))
		log_error("%s: rename to %s failed: %s", tmp_file, pf->file,
			  strerror(errno));

	r = 1;

out:
	fcntl_unlock_file(lockfd);

	if (cft)
		config_destroy(cft);

	return r;
}

static int _lookup_p(struct dev_filter *f, struct device *dev)
{
	struct pfilter *pf = (struct pfilter *) f->private;
	void *l;
	struct dm_str_list *sl;
	int pass = 1;

	if (dm_list_empty(&dev->aliases)) {
		log_debug_devs("%d:%d: filter cache skipping (no name)",
				(int)MAJOR(dev->dev), (int)MINOR(dev->dev));
		return 0;
	}

	l = dm_hash_lookup(pf->devices, dev_name(dev));

	/* Cached bad, skip dev */
	if (l == PF_BAD_DEVICE) {
		log_debug_devs("%s: filter cache skipping (cached bad)", dev_name(dev));
		return 0;
	}

	/* Cached good, use dev */
	if (l == PF_GOOD_DEVICE) {
		log_debug_devs("%s: filter cache using (cached good)", dev_name(dev));
		return 1;
	}

	/* Uncached, check filters and cache the result */
	if (!l) {
		dev->flags &= ~DEV_FILTER_AFTER_SCAN;

		pass = pf->real->passes_filter(pf->real, dev);

		if (!pass) {
			/*
			 * A device that does not pass one filter is excluded
			 * even if the result of another filter is deferred,
			 * because the deferred result won't change the exclude.
			 */
			l = PF_BAD_DEVICE;

		} else if ((pass == -EAGAIN) || (dev->flags & DEV_FILTER_AFTER_SCAN)) {
			/*
			 * When the filter result is deferred, we let the device
			 * pass for now, but do not cache the result.  We need to
			 * rerun the filters later.  At that point the final result
			 * will be cached.
			 */
			log_debug_devs("filter cache deferred %s", dev_name(dev));
			dev->flags |= DEV_FILTER_AFTER_SCAN;
			pass = 1;
			goto out;

		} else if (pass) {
			l = PF_GOOD_DEVICE;
		}

		log_debug_devs("filter caching %s %s", pass ? "good" : "bad", dev_name(dev));

		dm_list_iterate_items(sl, &dev->aliases)
			if (!dm_hash_insert(pf->devices, sl->str, l)) {
				log_error("Failed to hash alias to filter.");
				return 0;
			}
	}
 out:
	return pass;
}

static void _persistent_destroy(struct dev_filter *f)
{
	struct pfilter *pf = (struct pfilter *) f->private;

	if (f->use_count)
		log_error(INTERNAL_ERROR "Destroying persistent filter while in use %u times.", f->use_count);

	dm_hash_destroy(pf->devices);
	dm_free(pf->file);
	pf->real->destroy(pf->real);
	dm_free(pf);
	dm_free(f);
}

struct dev_filter *persistent_filter_create(struct dev_types *dt,
					    struct dev_filter *real,
					    const char *file)
{
	struct pfilter *pf;
	struct dev_filter *f = NULL;
	struct stat info;

	if (!(pf = dm_zalloc(sizeof(*pf)))) {
		log_error("Allocation of persistent filter failed.");
		return NULL;
	}

	pf->dt = dt;

	if (!(pf->file = dm_strdup(file))) {
		log_error("Filename duplication for persistent filter failed.");
		goto bad;
	}

	pf->real = real;

	if (!(_init_hash(pf))) {
		log_error("Couldn't create hash table for persistent filter.");
		goto bad;
	}

	if (!(f = dm_zalloc(sizeof(*f)))) {
		log_error("Allocation of device filter for persistent filter failed.");
		goto bad;
	}

	/* Only merge cache file before dumping it if it changed externally. */
	if (!stat(pf->file, &info))
		lvm_stat_ctim(&pf->ctime, &info);

	f->passes_filter = _lookup_p;
	f->destroy = _persistent_destroy;
	f->use_count = 0;
	f->private = pf;
	f->wipe = _persistent_filter_wipe;
	f->dump = _persistent_filter_dump;

	log_debug_devs("Persistent filter initialised.");

	return f;

      bad:
	dm_free(pf->file);
	if (pf->devices)
		dm_hash_destroy(pf->devices);
	dm_free(pf);
	dm_free(f);
	return NULL;
}
