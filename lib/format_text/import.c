/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "import-export.h"
#include "config.h"

#if 0
struct importer {
	struct config_file *cf;
	struct volume_group *vg;
};

static struct volume_group *_read_vg(struct pool *mem, struct config_file *cf)
{
	struct config_node *vgn;
	struct volume_group *vg;

	if (!(vgn = find_config_node(cf->root, "volume_group", '/'))) {
		log_err("Couldn't find volume_group section.");
		return NULL;
	}

	if (!(vg = pool_zalloc(mem, sizeof(*vg)))) {
		stack;
		return NULL;
	}

	vg->
}

struct volume_group *text_vg_import(struct cmd_context *cmd,
				    const char *file)
{
	struct volume_group *vg = NULL;
	struct config_file *cf;

	if (!(cf = create_config_file())) {
		stack;
		goto out;
	}

	if (!read_config(cf, file)) {
		log_err("Couldn't read volume group file.");
		goto out;
	}

	if (!(vg = _read_vg(cmd->mem, cf)))
		stack;

 out:
	destroy_config_file(cf);
	return vg;
}
#endif
