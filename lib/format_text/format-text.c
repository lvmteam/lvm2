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
 * The text format is a human readable metadata
 * format that will be used to store the metadata
 * backups from the LVM2 system.  By being human
 * readable it is hoped that people will be able
 * to recieve a lot more support when things go
 * wrong.
 *
 * The format instance is given a directory path
 * upon creation.  Each file in this directory
 * whose name is of the form '*.vg' is a config
 * file (see lib/config.[hc]), which contains a
 * description of a single volume group.
 *
 * The prefix of the config file gives the volume group
 * name.
 */


struct text_c {
	char *dir;
};



/*
 * Returns a list of config files, one for each
 * .vg file in the given directory.
 */
struct config_list {
	struct list list;
	struct config_file *cf;
};

struct list *_get_configs(struct pool *mem, const char *dir)
{
	
}

void _put_configs(struct pool *mem, struct list *configs)
{
	struct list *cfh;
	struct config_list *cl;

	list_iterate(cfh, configs) {
		cl = list_item(cfh, struct config_file);
		destroy_config_file(cl->cf);
	}

	pool_free(mem, configs);
}


/*
 * Just returns the vg->name fields
struct list *get_vgs(struct format_instance *fi)
{
	struct text_c *tc = (struct text_c *) fi->private;

	
}

struct list *get_pvs(struct format_instance *fi)
{

}

struct physical_volume *pv_read(struct format_instance *fi,
				const char *pv_name)
{

}

int pv_setup(struct format_instance *fi, struct physical_volume *pv,
		struct volume_group *vg)
{

}

int pv_write(struct format_instance *fi, struct physical_volume *pv)
{

}

int vg_setup(struct format_instance *fi, struct volume_group *vg)
{

}

struct volume_group *vg_read(struct format_instance *fi, const char *vg_name)
{

}

int vg_write(struct format_instance *fi, struct volume_group *vg)
{

}

void destroy(struct format_instance *fi)
{
	/*
	 * We don't need to do anything here since
	 * everything is allocated from the pool.
	 */
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
					   const char *dir)
{
	struct format_instance *fi;
	struct text_c *tc = NULL;

	if (!(tc = pool_zalloc(cmd->mem, sizeof(*tc)))) {
		stack;
		return NULL;
	}

	if (!(tc->dir = pool_strdup(cmd->mem, dir))) {
		stack;
		goto bad;
	}

	if (!(fi = pool_alloc(cmd->mem, sizeof(*fi)))) {
		stack;
		goto bad;
	}

	fi->cmd = cmd;
	fi->ops = _text_handler;
	fi->private = tc;

	return fi;

 bad:
	pool_free(mem, tc);
	return NULL;
}
