/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "filter-composite.h"

#include <stdarg.h>

static int _and_p(struct dev_filter *f, struct device *dev)
{
	struct dev_filter **filters = (struct dev_filter **) f->private;

	while (*filters) {
		if (!(*filters)->passes_filter(*filters, dev))
			return 0;
		filters++;
	}

	log_debug("Using %s", dev_name(dev));

	return 1;
}

static void _destroy(struct dev_filter *f)
{
	struct dev_filter **filters = (struct dev_filter **) f->private;

	while (*filters) {
		(*filters)->destroy(*filters);
		filters++;
	}

	dbg_free(f->private);
	dbg_free(f);
}

struct dev_filter *composite_filter_create(int n, struct dev_filter **filters)
{
	struct dev_filter **filters_copy, *cf;

	if (!filters) {
		stack;
		return NULL;
	}

	if (!(filters_copy = dbg_malloc(sizeof(*filters) * (n + 1)))) {
		log_error("composite filters allocation failed");
		return NULL;
	}

	memcpy(filters_copy, filters, sizeof(*filters) * n);
	filters_copy[n] = NULL;

	if (!(cf = dbg_malloc(sizeof(*cf)))) {
		log_error("compsoite filters allocation failed");
		dbg_free(filters_copy);
		return NULL;
	}

	cf->passes_filter = _and_p;
	cf->destroy = _destroy;
	cf->private = filters_copy;

	return cf;
}
