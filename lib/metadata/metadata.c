/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "log.h"
#include "pool.h"
#include "device.h"
#include "dev-cache.h"
#include "metadata.h"

#include <string.h>

struct physical_volume *pv_create(struct io_space *ios, const char *name)
{
	struct physical_volume *pv = pool_alloc(ios->mem, sizeof(*pv));

	if (!pv) {
		stack;
		return NULL;
	}

        id_create(&pv->id);
	if (!(pv->dev = dev_cache_get(name, ios->filter))) {
		log_err("Couldn't find device '%s'", name);
		goto bad;
	}

	pv->vg_name = NULL;
	pv->exported = NULL;
        pv->status = 0;

	if (!dev_get_size(pv->dev, &pv->size)) {
		log_err("Couldn't get size of device '%s'", name);
		goto bad;
	}

        pv->pe_size = 0;
        pv->pe_start = 0;
        pv->pe_count = 0;
        pv->pe_allocated = 0;
	return pv;

 bad:
	pool_free(ios->mem, pv);
	return NULL;
}



