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
#include "lib/filters/filter.h"
#include "lib/config/config.h"

struct pfilter {
	struct radix_tree *devices;
	struct dev_filter *real;
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
 */

static int _good_device;
static int _bad_device;

/*
 * The hash table holds one of these two states
 * against each entry.
 */
#define PF_BAD_DEVICE ((void *) &_good_device)
#define PF_GOOD_DEVICE ((void *) &_bad_device)

static int _init_hash(struct pfilter *pf)
{
	if (pf->devices)
		radix_tree_destroy(pf->devices);

	if (!(pf->devices = radix_tree_create(NULL, NULL)))
		return_0;

	return 1;
}

static void _persistent_filter_wipe(struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name)
{
	struct pfilter *pf = (struct pfilter *) f->private;
	struct dm_str_list *sl;

	if (!dev) {
		_init_hash(pf);
	} else {
		dm_list_iterate_items(sl, &dev->aliases)
			radix_tree_remove(pf->devices, sl->str, strlen(sl->str));
	}
}

static int _lookup_p(struct cmd_context *cmd, struct dev_filter *f, struct device *dev, const char *use_filter_name)
{
	struct pfilter *pf = (struct pfilter *) f->private;
	void *l;
	struct dm_str_list *sl;
	int pass = 1;
	const char *devname = dev_name(dev);

	if (use_filter_name && strcmp(f->name, use_filter_name))
		return pf->real->passes_filter(cmd, pf->real, dev, use_filter_name);

	if (dm_list_empty(&dev->aliases)) {
		log_debug_devs("%u:%u: filter cache skipping (no name).",
				MAJOR(dev->dev), MINOR(dev->dev));
		return 0;
	}

	l = radix_tree_lookup_ptr(pf->devices, devname, strlen(devname));

	/* Cached bad, skip dev */
	if (l == PF_BAD_DEVICE) {
		log_debug_devs("%s: filter cache skipping (cached bad).", devname);
		return 0;
	}

	/* Cached good, use dev */
	if (l == PF_GOOD_DEVICE) {
		log_debug_devs("%s: filter cache using (cached good).", devname);
		return 1;
	}

	/* Uncached, check filters and cache the result */
	if (!l) {
		pass = pf->real->passes_filter(cmd, pf->real, dev, use_filter_name);

		if (!pass) {
			/*
			 * A device that does not pass one filter is excluded
			 * even if the result of another filter is deferred,
			 * because the deferred result won't change the exclude.
			 */
			l = PF_BAD_DEVICE;
		} else if (pass == 1) {
			l = PF_GOOD_DEVICE;
		} else {
			log_error("Ignore invalid filter result %d %s.", pass, devname);
			pass = 1;
			/* don't cache invalid result */
			goto out;
		}

		if (!dev->filtered_flags) /* skipping reason already logged by filter */
			log_debug_devs("filter caching %s %s.", pass ? "good" : "bad", devname);

		dm_list_iterate_items(sl, &dev->aliases)
			if (!radix_tree_insert_ptr(pf->devices, sl->str, strlen(sl->str), l)) {
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

	radix_tree_destroy(pf->devices);
	pf->real->destroy(pf->real);
	free(pf);
	free(f);
}

struct dev_filter *persistent_filter_create(struct dev_types *dt, struct dev_filter *real)
{
	struct pfilter *pf;
	struct dev_filter *f = NULL;

	if (!(pf = zalloc(sizeof(*pf)))) {
		log_error("Allocation of persistent filter failed.");
		return NULL;
	}

	pf->dt = dt;

	pf->real = real;

	if (!(_init_hash(pf))) {
		log_error("Couldn't create hash table for persistent filter.");
		goto bad;
	}

	if (!(f = zalloc(sizeof(*f)))) {
		log_error("Allocation of device filter for persistent filter failed.");
		goto bad;
	}

	f->passes_filter = _lookup_p;
	f->destroy = _persistent_destroy;
	f->use_count = 0;
	f->private = pf;
	f->wipe = _persistent_filter_wipe;
	f->name = "persistent";

	log_debug_devs("Persistent filter initialised.");

	return f;

      bad:
	if (pf->devices)
		radix_tree_destroy(pf->devices);
	free(pf);
	free(f);
	return NULL;
}
