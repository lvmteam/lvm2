/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "config.h"
#include "dev-cache.h"
#include "hash.h"
#include "filter-persistent.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

struct pfilter {
	char *file;
	struct hash_table *devices;
	struct dev_filter *real;
};

/*
 * entries in the table can be in one of these
 * states.
 */
#define PF_BAD_DEVICE ((void *) 1)
#define PF_GOOD_DEVICE ((void *) 2)

static int _init_hash(struct pfilter *pf)
{
	if (pf->devices)
		hash_destroy(pf->devices);

	if (!(pf->devices = hash_create(128))) {
		stack;
		return 0;
	}

	return 1;
}

int persistent_filter_wipe(struct dev_filter *f)
{
	struct pfilter *pf = (struct pfilter *) f->private;

	hash_wipe(pf->devices);
	/* Trigger complete device scan */
	dev_cache_scan(1);

	return 1;
}

static int _read_array(struct pfilter *pf, struct config_tree *cf,
		       const char *path, void *data)
{
	struct config_node *cn;
	struct config_value *cv;

	if (!(cn = find_config_node(cf->root, path, '/'))) {
		log_very_verbose("Couldn't find %s array in '%s'",
				 path, pf->file);
		return 0;
	}

	/*
	 * iterate through the array, adding
	 * devices as we go.
	 */
	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != CFG_STRING) {
			log_verbose("Devices array contains a value "
				    "which is not a string ... ignoring");
			continue;
		}

		if (!hash_insert(pf->devices, cv->v.str, data))
			log_verbose("Couldn't add '%s' to filter ... ignoring",
				    cv->v.str);
		/* Populate dev_cache ourselves */
		dev_cache_get(cv->v.str, NULL);
	}
	return 1;
}

int persistent_filter_load(struct dev_filter *f)
{
	struct pfilter *pf = (struct pfilter *) f->private;

	int r = 0;
	struct config_tree *cf;

	if (!(cf = create_config_tree())) {
		stack;
		return 0;
	}

	if (!read_config_file(cf, pf->file)) {
		stack;
		goto out;
	}

	_read_array(pf, cf, "persistent_filter_cache/valid_devices",
		    PF_GOOD_DEVICE);
	/* We don't gain anything by holding invalid devices */
	/* _read_array(pf, cf, "persistent_filter_cache/invalid_devices",
	   PF_BAD_DEVICE); */

	/* Did we find anything? */
	if (hash_get_num_entries(pf->devices)) {
		/* We populated dev_cache ourselves */
		dev_cache_scan(0);
		r = 1;
	}

	log_very_verbose("Loaded persistent filter cache from %s", pf->file);

      out:
	destroy_config_tree(cf);
	return r;
}

static void _write_array(struct pfilter *pf, FILE *fp, const char *path,
			 void *data)
{
	void *d;
	int first = 1;
	struct hash_node *n;

	for (n = hash_get_first(pf->devices); n;
	     n = hash_get_next(pf->devices, n)) {
		d = hash_get_data(pf->devices, n);

		if (d != data)
			continue;

		if (!first)
			fprintf(fp, ",\n");
		else {
			fprintf(fp, "\t%s=[\n", path);
			first = 0;
		}

		fprintf(fp, "\t\t\"%s\"", hash_get_key(pf->devices, n));
	}

	if (!first)
		fprintf(fp, "\n\t]\n");

	return;
}

int persistent_filter_dump(struct dev_filter *f)
{
	struct pfilter *pf = (struct pfilter *) f->private;

	FILE *fp;

	if (!hash_get_num_entries(pf->devices)) {
		log_very_verbose("Internal persistent device cache empty "
				 "- not writing to %s", pf->file);
		return 0;
	}
	if (!dev_cache_has_scanned()) {
		log_very_verbose("Device cache incomplete - not writing "
				 "to %s", pf->file);
		return 0;
	}

	log_very_verbose("Dumping persistent device cache to %s", pf->file);

	fp = fopen(pf->file, "w");
	if (!fp) {
		if (errno != EROFS)
			log_sys_error("fopen", pf->file);
		return 0;
	}

	fprintf(fp, "# This file is automatically maintained by lvm.\n\n");
	fprintf(fp, "persistent_filter_cache {\n");

	_write_array(pf, fp, "valid_devices", PF_GOOD_DEVICE);
	/* We don't gain anything by remembering invalid devices */
	/* _write_array(pf, fp, "invalid_devices", PF_BAD_DEVICE); */

	fprintf(fp, "}\n");
	fclose(fp);
	return 1;
}

static int _lookup_p(struct dev_filter *f, struct device *dev)
{
	struct pfilter *pf = (struct pfilter *) f->private;
	void *l = hash_lookup(pf->devices, dev_name(dev));
	struct str_list *sl;
	struct list *ah;

	if (!l) {
		l = pf->real->passes_filter(pf->real, dev) ?
		    PF_GOOD_DEVICE : PF_BAD_DEVICE;

		list_iterate(ah, &dev->aliases) {
			sl = list_item(ah, struct str_list);
			hash_insert(pf->devices, sl->str, l);
		}
	}

	if (l == PF_BAD_DEVICE) {
		log_debug("%s: Skipping (cached)", dev_name(dev));
		return 0;
	} else
		return 1;
}

static void _destroy(struct dev_filter *f)
{
	struct pfilter *pf = (struct pfilter *) f->private;

	hash_destroy(pf->devices);
	dbg_free(pf->file);
	pf->real->destroy(pf->real);
	dbg_free(pf);
	dbg_free(f);
}

struct dev_filter *persistent_filter_create(struct dev_filter *real,
					    const char *file)
{
	struct pfilter *pf;
	struct dev_filter *f = NULL;

	if (!(pf = dbg_malloc(sizeof(*pf)))) {
		stack;
		return NULL;
	}
	memset(pf, 0, sizeof(*pf));

	if (!(pf->file = dbg_malloc(strlen(file) + 1))) {
		stack;
		goto bad;
	}
	strcpy(pf->file, file);
	pf->real = real;

	if (!(_init_hash(pf))) {
		log_error("Couldn't create hash table for persistent filter.");
		goto bad;
	}

	if (!(f = dbg_malloc(sizeof(*f)))) {
		stack;
		goto bad;
	}

	f->passes_filter = _lookup_p;
	f->destroy = _destroy;
	f->private = pf;

	return f;

      bad:
	dbg_free(pf->file);
	if (pf->devices)
		hash_destroy(pf->devices);
	dbg_free(pf);
	dbg_free(f);
	return NULL;
}
