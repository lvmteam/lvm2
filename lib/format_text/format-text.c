/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "format-text.h"

#include "log.h"
#include "pool.h"
#include "config.h"
#include "hash.h"

/*
 * NOTE: Currently there can be only one vg per file.
 */

static void _not_written(const char *cmd)
{
	log_err("The text format is lacking an implementation for '%s'", cmd);
}

struct list *_get_vgs(struct format_instance *fi)
{
	_not_written("_get_vgs");
	return NULL;
}

struct list *_get_pvs(struct format_instance *fi)
{
	_not_written("_get_vgs");
	return NULL;
}

struct physical_volume *_pv_read(struct format_instance *fi,
				 const char *pv_name)
{
	_not_written("_get_vgs");
	return NULL;
}

int _pv_setup(struct format_instance *fi, struct physical_volume *pv,
	      struct volume_group *vg)
{
	_not_written("_get_vgs");
	return NULL;
}

int _pv_write(struct format_instance *fi, struct physical_volume *pv)
{
	_not_written("_get_vgs");
	return NULL;
}

int _vg_setup(struct format_instance *fi, struct volume_group *vg)
{
	_not_written("_get_vgs");
	return NULL;
}

struct volume_group *_vg_read(struct format_instance *fi,
			      const char *vg_name)
{
	_not_written("_get_vgs");
	return NULL;
}

int _vg_write(struct format_instance *fi, struct volume_group *vg)
{
	FILE *fp;
	char *file = (char *) fi->private;

	/* FIXME: should be opened exclusively */
	if (!(fp = fopen(file, "w"))) {
		log_err("Couldn't open text file '%s'.", file);
		return 0;
	}

	if (!text_vg_export(fp, vg)) {
		log_err("Couldn't write text format file.");
		return 0;
	}

	return 1;
}

void _destroy(struct format_instance *fi)
{
	pool_free(cmd->mem, fi);
}

static struct format_handler _text_handler = {
	get_vgs: _get_vgs,
	get_pvs: _get_pvs,
	pv_read: _pv_read,
	pv_setup: _pv_setup,
	pv_write: _pv_write,
	vg_setup: _vg_setup,
	vg_read: _vg_read,
	vg_write: _vg_write,
	destroy: _destroy
};

struct format_instance *text_format_create(struct cmd_context *cmd,
					   const char *file)
{
	const char *no_alloc = "Couldn't allocate text format object.";

	struct format_instance *fi;
	char *file;

	if (!(fi = pool_alloc(cmd->mem, sizeof(*fi)))) {
		log_err(no_alloc);
		return NULL;
	}

	if (!(file = pool_strdup(cmd->mem, file))) {
		pool_free(fi);
		log_err(no_alloc);
		return NULL;
	}

	fi->cmd = cmd;
	fi->ops = _text_handler;
	fi->private = file;

	return fi;
}
