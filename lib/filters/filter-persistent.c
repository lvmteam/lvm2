/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "config.h"
#include "dev-cache.h"
#include "hash.h"
#include "dbg_malloc.h"
#include "log.h"
#include "filter-persistent.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

struct pfilter {
	char *file;
	struct hash_table *devices;
};

/*
 * entries in the table can be in one of these
 * states.
 */
#define PF_UNCHECKED ((void *) 1)
#define PF_CHECKED ((void *) 2)

static int _init_hash(struct pfilter *pf)
{
	if (pf->devices)
		hash_destroy(pf->devices);

	pf->devices = hash_create(128);
	return pf ? 1 : 0;
}

static int _load(struct pfilter *pf)
{
	int r = 0;
	struct config_file *cf;
	struct config_node *cn;
	struct config_value *cv;

	if (!(cf = create_config_file())) {
		stack;
		return 0;
	}

	if (!read_config(cf, pf->file)) {
		stack;
		goto out;
	}

	if (!(cn = find_config_node(cf->root, "/valid_devices", '/'))) {
		log_info("Couldn't find 'valid_devices' array in '%s'",
			 pf->file);
		goto out;
	}

	/*
	 * iterate through the array, adding
	 * devices as we go.
	 */
	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != CFG_STRING) {
			log_info("Valid_devices array contains a value "
				 "which is not a string ... ignoring");
			continue;
		}

		if (!hash_insert(pf->devices, cv->v.str, PF_UNCHECKED))
			log_info("Couldn't add '%s' to filter ... ignoring",
				 cv->v.str);
	}
	r = 1;

 out:
	destroy_config_file(cf);
	return r;
}

static int _dump(struct pfilter *pf)
{
	int first = 1;
	struct hash_node *n;
	FILE *fp = fopen(pf->file, "w");

	log_very_verbose("Dumping persistent device cache to %s", pf->file);

	if (!fp) {
		log_info("Couldn't open '%s' for to hold valid devices.",
			 pf->file);
		return 0;
	}

	fprintf(fp, "# This file is automatically maintained by lvm.\n\n");
	fprintf(fp, "valid_devices=[\n");

	for (n = hash_get_first(pf->devices); n;
	     n = hash_get_next(pf->devices, n)) {
		if (!first)
			fprintf(fp, ",\n");
		else
			first = 0;

		fprintf(fp, "\t\"%s\"", hash_get_key(pf->devices, n));
	}

	fprintf(fp, "\n]\n");
	fclose(fp);
	return 1;
}

static int _check(const char *path)
{
	int fd = open(path, O_RDONLY), r = 0;

	if (fd >= 0)
		r = 1;
	else
		log_debug("Unable to open %s: %s", path, strerror(errno));

	close(fd);
	return r;
}

static int _init_valid_p(struct dev_filter *f, struct device *dev)
{
	struct pfilter *pf = (struct pfilter *) f->private;
	void *l = hash_lookup(pf->devices, dev->name);

	if (l)
		return 1;

	if (_check(dev->name)) {
		hash_insert(pf->devices, dev->name, PF_CHECKED);
		return 1;
	}

	return 0;
}

static int _valid_p(struct dev_filter *f, struct device *dev)
{
	struct pfilter *pf = (struct pfilter *) f->private;
	void *l = hash_lookup(pf->devices, dev->name);

	if (!l)
		return 0;

	if (l == PF_UNCHECKED && !_check(dev->name)) {
		hash_remove(pf->devices, dev->name);
		return 0;
	}

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	struct pfilter *pf = (struct pfilter *) f->private;

	_dump(pf);
	hash_destroy(pf->devices);
	dbg_free(pf->file);
	dbg_free(pf);
	dbg_free(f);
}

struct dev_filter *persistent_filter_create(const char *file, int init)
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

	if (!(_init_hash(pf))) {
		log_err("Couldn't create hash table for persistent filter.");
		goto bad;
	}

	if (!init)
		_load(pf);

	if (!(f = dbg_malloc(sizeof(*f)))) {
		stack;
		goto bad;
	}

	f->passes_filter = init ? _init_valid_p : _valid_p;
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

