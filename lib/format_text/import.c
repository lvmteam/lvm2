/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"
#include "import-export.h"
#include "pool.h"
#include "display.h"
#include "hash.h"
#include "toolcontext.h"
#include "lvmcache.h"

/* FIXME Use tidier inclusion method */
static struct text_vg_version_ops *(_text_vsn_list[2]);

struct volume_group *text_vg_import_fd(struct format_instance *fid,
				       const char *file,
				       struct device *dev,
				       off_t offset, uint32_t size,
				       off_t offset2, uint32_t size2,
				       checksum_fn_t checksum_fn,
				       uint32_t checksum,
				       time_t *when, char **desc)
{
	struct volume_group *vg = NULL;
	struct config_tree *cft;
	struct text_vg_version_ops **vsn;

	static int _initialised = 0;

	if (!_initialised) {
		_text_vsn_list[0] = text_vg_vsn1_init();
		_text_vsn_list[1] = NULL;
		_initialised = 1;
	}

	*desc = NULL;
	*when = 0;

	if (!(cft = create_config_tree())) {
		stack;
		goto out;
	}

	if ((!dev && !read_config_file(cft, file)) ||
	    (dev && !read_config_fd(cft, dev, offset, size,
				    offset2, size2, checksum_fn, checksum))) {
		log_error("Couldn't read volume group metadata.");
		goto out;
	}

	/* 
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(vg = (*vsn)->read_vg(fid, cft))) {
			stack;
			goto out;
		}

		(*vsn)->read_desc(fid->fmt->cmd->mem, cft, when, desc);
		break;
	}

      out:
	destroy_config_tree(cft);
	return vg;
}

struct volume_group *text_vg_import_file(struct format_instance *fid,
					 const char *file,
					 time_t *when, char **desc)
{
	return text_vg_import_fd(fid, file, NULL, 0, 0, 0, 0, NULL, 0,
				 when, desc);
}
