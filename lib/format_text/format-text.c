/*
 * Copyright (C) 2001-2002 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "format-text.h"
#include "import-export.h"

#include "lvm-file.h"
#include "log.h"
#include "pool.h"
#include "config.h"
#include "hash.h"
#include "display.h"
#include "dbg_malloc.h"
#include "toolcontext.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <limits.h>

/* Arbitrary limits copied from format1/disk_rep.h */
#define MAX_PV 256
#define MAX_LV 256
#define MAX_VG 99
#define MAX_PV_SIZE	((uint32_t) -1)	/* 2TB in sectors - 1 */

/*
 * NOTE: Currently there can be only one vg per file.
 */

struct text_c {
	char *path;
	char *desc;
	struct uuid_map *um;
};

static int _pv_setup(struct format_instance *fi, struct physical_volume *pv,
		     struct volume_group *vg)
{
	/* setup operations for the PV structure */
	if (pv->size > MAX_PV_SIZE)
		pv->size--;
	if (pv->size > MAX_PV_SIZE) {
		/* FIXME Limit hardcoded */
		log_error("Physical volumes cannot be bigger than 2TB");
		return 0;
	}

	return 1;
}

static int _vg_setup(struct format_instance *fi, struct volume_group *vg)
{
	/* just check max_pv and max_lv */
	if (vg->max_lv >= MAX_LV)
		vg->max_lv = MAX_LV - 1;

	if (vg->max_pv >= MAX_PV)
		vg->max_pv = MAX_PV - 1;

	return 1;
}

static int _lv_setup(struct format_instance *fi, struct logical_volume *lv)
{
	uint64_t max_size = UINT_MAX;

	if (lv->size > max_size) {
		char *dummy = display_size(max_size, SIZE_SHORT);
		log_error("logical volumes cannot be larger than %s", dummy);
		dbg_free(dummy);
		return 0;
	}

	return 1;
}

static struct volume_group *_vg_read(struct format_instance *fi,
				     const char *vg_name)
{
	struct text_c *tc = (struct text_c *) fi->private;
	struct volume_group *vg;
	time_t when;
	char *desc;

	if (!(vg = text_vg_import(fi->cmd, tc->path, tc->um, &when, &desc))) {
		stack;
		return NULL;
	}

	/*
	 * Currently you can only have a single volume group per
	 * text file (this restriction may remain).  We need to
	 * check that it contains the correct volume group.
	 */
	if (strcmp(vg_name, vg->name)) {
		pool_free(fi->cmd->mem, vg);
		log_err("'%s' does not contain volume group '%s'.",
			tc->path, vg_name);
		return NULL;
	}

	return vg;
}

static int _vg_write(struct format_instance *fi, struct volume_group *vg)
{
	struct text_c *tc = (struct text_c *) fi->private;

	FILE *fp;
	int fd;
	char *slash;
	char temp_file[PATH_MAX], temp_dir[PATH_MAX];

	slash = rindex(tc->path, '/');

	if (slash == 0)
		strcpy(temp_dir, ".");
	else if (slash - tc->path < PATH_MAX) {
		strncpy(temp_dir, tc->path, slash - tc->path);
		temp_dir[slash - tc->path] = '\0';

	} else {
		log_error("Text format failed to determine directory.");
		return 0;
	}

	if (!create_temp_name(temp_dir, temp_file, sizeof(temp_file), &fd)) {
		log_err("Couldn't create temporary text file name.");
		return 0;
	}

	if (!(fp = fdopen(fd, "w"))) {
		log_sys_error("fdopen", temp_file);
		close(fd);
		return 0;
	}

	if (!text_vg_export(fp, vg, tc->desc)) {
		log_error("Failed to write metadata to %s.", temp_file);
		fclose(fp);
		return 0;
	}

	if (fclose(fp)) {
		log_sys_error("fclose", tc->path);
		return 0;
	}

	if (rename(temp_file, tc->path)) {
		log_error("%s: rename to %s failed: %s", temp_file, tc->path,
			  strerror(errno));
		return 0;
	}

	return 1;
}

static struct list *_get_vgs(struct format_instance *fi)
{
	struct text_c *tc = (struct text_c *) fi->private;
	struct list *names = pool_alloc(fi->cmd->mem, sizeof(*names));
	struct name_list *nl;
	struct volume_group *vg;
	char *slash;
	char *vgname;

	if (!names) {
		stack;
		return NULL;
	}

	list_init(names);

	/* Determine the VG name from the file name */
	slash = rindex(tc->path, '/');
	if (slash) {
		vgname = pool_alloc(fi->cmd->mem, strlen(slash));
		strcpy(vgname, slash + 1);
	} else {
		vgname = pool_alloc(fi->cmd->mem, strlen(tc->path) + 1);
		strcpy(vgname, tc->path);
	}

	vg = _vg_read(fi, vgname);
	if (vg) {

		pool_free(fi->cmd->mem, vg);
		if (!(nl = pool_alloc(fi->cmd->mem, sizeof(*nl)))) {
			stack;
			goto bad;
		}
		nl->name = vgname;

		list_add(names, &nl->list);
	}

	return names;

      bad:
	pool_free(fi->cmd->mem, names);
	return NULL;
}

static struct list *_get_pvs(struct format_instance *fi)
{
	struct pv_list *pvl;
	struct list *vgh;
	struct list *pvh;
	struct list *results = pool_alloc(fi->cmd->mem, sizeof(*results));
	struct list *vgs = _get_vgs(fi);

	list_init(results);

	list_iterate(vgh, vgs) {
		struct volume_group *vg;
		struct name_list *nl;

		nl = list_item(vgh, struct name_list);
		vg = _vg_read(fi, nl->name);
		if (vg) {
			list_iterate(pvh, &vg->pvs) {
				struct pv_list *vgpv =
				    list_item(pvh, struct pv_list);

				pvl = pool_alloc(fi->cmd->mem, sizeof(*pvl));
				if (!pvl) {
					stack;
					goto bad;
				}
				/* ?? do we need to clone the pv structure...really? Nah. */
				pvl->pv = vgpv->pv;
				list_add(results, &pvl->list);
			}
		}
	}
	return results;

      bad:
	pool_free(fi->cmd->mem, vgs);
	return NULL;
}

static int _pv_write(struct format_instance *fi, struct physical_volume *pv)
{
	struct volume_group *vg;
	struct list *pvh;

	vg = _vg_read(fi, pv->vg_name);

	/* Find the PV in this VG */
	if (vg) {
		list_iterate(pvh, &vg->pvs) {
			struct pv_list *vgpv = list_item(pvh, struct pv_list);

			if (id_equal(&pv->id, &vgpv->pv->id)) {
				vgpv->pv->status = pv->status;
				vgpv->pv->size = pv->size;

				/* Not sure if it's worth doing these */
				vgpv->pv->pe_size = pv->pe_size;
				vgpv->pv->pe_count = pv->pe_count;
				vgpv->pv->pe_start = pv->pe_start;
				vgpv->pv->pe_allocated = pv->pe_allocated;

				/* Write it back */
				_vg_write(fi, vg);
				pool_free(fi->cmd->mem, vg);
				return 1;
			}
		}
		pool_free(fi->cmd->mem, vg);
	}

	/* Can't handle PVs not in a VG */
	return 0;
}

static struct physical_volume *_pv_read(struct format_instance *fi,
					const char *pv_name)
{
	struct list *vgs = _get_vgs(fi);
	struct list *vgh;
	struct list *pvh;
	struct physical_volume *pv;

	/* Look for the PV */
	list_iterate(vgh, vgs) {
		struct volume_group *vg;
		struct name_list *nl;

		nl = list_item(vgh, struct name_list);
		vg = _vg_read(fi, nl->name);
		if (vg) {
			list_iterate(pvh, &vg->pvs) {
				struct pv_list *vgpv =
				    list_item(pvh, struct pv_list);

				if (!strcmp(dev_name(vgpv->pv->dev), pv_name)) {
					pv = pool_alloc(fi->cmd->mem,
							sizeof(*pv));
					if (!pv) {
						stack;
						pool_free(fi->cmd->mem, vg);
						return NULL;
					}
					/* Memberwise copy */
					*pv = *vgpv->pv;

					pv->vg_name =
					    pool_alloc(fi->cmd->mem,
						       strlen(vgpv->pv->
							      vg_name) + 1);
					if (!pv->vg_name) {
						stack;
						pool_free(fi->cmd->mem, vg);
						return NULL;
					}
					strcpy(pv->vg_name, vgpv->pv->vg_name);
					pool_free(fi->cmd->mem, vg);
					return pv;
				}
			}
			pool_free(fi->cmd->mem, vg);
		}
	}

	return NULL;
}

static void _destroy(struct format_instance *fi)
{
	struct text_c *tc = (struct text_c *) fi->private;

	dbg_free(tc->path);
	dbg_free(tc->desc);
	dbg_free(tc);
	dbg_free(fi);
}

static struct format_handler _text_handler = {
	get_vgs:	_get_vgs,
	get_pvs:	_get_pvs,
	pv_read:	_pv_read,
	pv_setup:	_pv_setup,
	pv_write:	_pv_write,
	vg_setup:	_vg_setup,
	lv_setup:	_lv_setup,
	vg_read:	_vg_read,
	vg_write:	_vg_write,
	destroy:	_destroy
};

struct format_instance *text_format_create(struct cmd_context *cmd,
					   const char *file,
					   struct uuid_map *um,
					   const char *desc)
{
	struct format_instance *fi;
	char *path, *d;
	struct text_c *tc;

	if (!(fi = dbg_malloc(sizeof(*fi)))) {
		stack;
		goto no_mem;
	}

	if (!(path = dbg_strdup(file))) {
		stack;
		goto no_mem;
	}

	if (!(d = dbg_strdup(desc))) {
		stack;
		goto no_mem;
	}

	if (!(tc = dbg_malloc(sizeof(*tc)))) {
		stack;
		goto no_mem;
	}

	tc->path = path;
	tc->desc = d;
	tc->um = um;

	fi->cmd = cmd;
	fi->ops = &_text_handler;
	fi->private = tc;

	return fi;

      no_mem:
	if (fi)
		dbg_free(fi);

	if (path)
		dbg_free(path);

	if (d)
		dbg_free(path);

	log_err("Couldn't allocate text format object.");
	return NULL;
}
