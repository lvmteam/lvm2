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
#include "vgcache.h"
#include "lvm-string.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/file.h>
#include <limits.h>
#include <dirent.h>

/* Arbitrary limits copied from format1/disk_rep.h */
#define MAX_PV 256
#define MAX_LV 256
#define MAX_VG 99
#define MAX_PV_SIZE	((uint32_t) -1)	/* 2TB in sectors - 1 */

struct dir_list {
	struct list list;
	char dir[0];
};

struct text_context {
	char *path_live;	/* Path to file holding live metadata */
	char *path_edit;	/* Path to file holding edited metadata */
	char *desc;		/* Description placed inside file */
};

/*
 * NOTE: Currently there can be only one vg per file.
 */

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

	if (vg->extent_size & (vg->extent_size - 1)) {
		log_error("Extent size must be power of 2");
		return 0;
	}

	return 1;
}

static int _lv_setup(struct format_instance *fi, struct logical_volume *lv)
{
	uint64_t max_size = UINT_MAX;

	if (!*lv->lvid.s)
		lvid_create(&lv->lvid, &lv->vg->id);

	if (lv->size > max_size) {
		char *dummy = display_size(max_size, SIZE_SHORT);
		log_error("logical volumes cannot be larger than %s", dummy);
		dbg_free(dummy);
		return 0;
	}

	return 1;
}

static struct volume_group *_vg_read(struct format_instance *fi,
				     const char *vgname, void *mdl)
{
	struct text_context *tc = (struct text_context *) mdl;
	struct volume_group *vg;
	time_t when;
	char *desc;

	if (!(vg = text_vg_import(fi, tc->path_live, fi->fmt->cmd->um, &when,
				  &desc))) {
		stack;
		return NULL;
	}

	/*
	 * Currently you can only have a single volume group per
	 * text file (this restriction may remain).  We need to
	 * check that it contains the correct volume group.
	 */
	if (strcmp(vgname, vg->name)) {
		pool_free(fi->fmt->cmd->mem, vg);
		log_err("'%s' does not contain volume group '%s'.",
			tc->path_live, vgname);
		return NULL;
	}

	return vg;
}

static int _vg_write(struct format_instance *fi, struct volume_group *vg,
		     void *mdl)
{
	struct text_context *tc = (struct text_context *) mdl;

	FILE *fp;
	int fd;
	char *slash;
	char temp_file[PATH_MAX], temp_dir[PATH_MAX];

	slash = rindex(tc->path_edit, '/');

	if (slash == 0)
		strcpy(temp_dir, ".");
	else if (slash - tc->path_edit < PATH_MAX) {
		strncpy(temp_dir, tc->path_edit, slash - tc->path_edit);
		temp_dir[slash - tc->path_edit] = '\0';

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

	if (fsync(fd)) {
		log_sys_error("fsync", tc->path_edit);
		fclose(fp);
		return 0;
	}

	if (fclose(fp)) {
		log_sys_error("fclose", tc->path_edit);
		return 0;
	}

	if (rename(temp_file, tc->path_edit)) {
		log_error("%s: rename to %s failed: %s", temp_file,
			  tc->path_edit, strerror(errno));
		return 0;
	}

	return 1;
}

static int _pv_commit(struct format_instance *fi, struct physical_volume *pv,
		      void *mdl)
{
	// struct text_context *tc = (struct text_context *) mdl;

	return 1;
}

static int _vg_commit(struct format_instance *fi, struct volume_group *vg,
		      void *mdl)
{
	struct text_context *tc = (struct text_context *) mdl;

	if (rename(tc->path_edit, tc->path_live)) {
		log_error("%s: rename to %s failed: %s", tc->path_edit,
			  tc->path_edit, strerror(errno));
		return 0;
	}

	sync();

	return 1;
}

static int _vg_remove(struct format_instance *fi, struct volume_group *vg,
		      void *mdl)
{
	struct text_context *tc = (struct text_context *) mdl;

	if (path_exists(tc->path_edit) && unlink(tc->path_edit)) {
		log_sys_error("unlink", tc->path_edit);
		return 0;
	}

	if (path_exists(tc->path_live) && unlink(tc->path_live)) {
		log_sys_error("unlink", tc->path_live);
		return 0;
	}

	sync();

	return 1;
}

/* Add vgname to list if it's not already there */
static int _add_vgname(struct format_type *fmt, struct list *names,
		       char *vgname)
{
	struct list *nlh;
	struct name_list *nl;

	list_iterate(nlh, names) {
		nl = list_item(nlh, struct name_list);
		if (!strcmp(vgname, nl->name))
			return 1;
	}

	vgcache_add(vgname, NULL, NULL, fmt);

	if (!(nl = pool_alloc(fmt->cmd->mem, sizeof(*nl)))) {
		stack;
		return 0;
	}

	if (!(nl->name = pool_strdup(fmt->cmd->mem, vgname))) {
		log_error("strdup %s failed", vgname);
		return 0;
	}

	list_add(names, &nl->list);
	return 1;
}

static struct list *_get_vgs(struct format_type *fmt, struct list *names)
{
	struct dirent *dirent;
	struct dir_list *dl;
	struct list *dlh, *dir_list;
	char *tmp;
	DIR *d;

	dir_list = (struct list *) fmt->private;

	list_iterate(dlh, dir_list) {
		dl = list_item(dlh, struct dir_list);
		if (!(d = opendir(dl->dir))) {
			log_sys_error("opendir", dl->dir);
			continue;
		}
		while ((dirent = readdir(d)))
			if (strcmp(dirent->d_name, ".") &&
			    strcmp(dirent->d_name, "..") &&
			    (!(tmp = strstr(dirent->d_name, ".tmp")) ||
			     tmp != dirent->d_name + strlen(dirent->d_name)
			     - 4))
				if (!_add_vgname(fmt, names, dirent->d_name))
					return NULL;

		if (closedir(d))
			log_sys_error("closedir", dl->dir);
	}

	return names;
}

static struct list *_get_pvs(struct format_type *fmt, struct list *results)
{
	struct pv_list *pvl, *rhl;
	struct list *vgh;
	struct list *pvh;
	struct list *names = pool_alloc(fmt->cmd->mem, sizeof(*names));
	struct list *rh;
	struct name_list *nl;
	struct volume_group *vg;

	list_init(names);
	if (!_get_vgs(fmt, names)) {
		stack;
		return NULL;
	}

	list_iterate(vgh, names) {

		nl = list_item(vgh, struct name_list);
		if (!(vg = vg_read(fmt->cmd, nl->name))) {
			log_error("format_text: _get_pvs failed to read VG %s",
				  nl->name);
			continue;
		}
		/* FIXME Use temp hash! */
		list_iterate(pvh, &vg->pvs) {
			pvl = list_item(pvh, struct pv_list);

			/* If in use, remove from list of orphans */
			list_iterate(rh, results) {
				rhl = list_item(rh, struct pv_list);
				if (id_equal(&rhl->pv->id, &pvl->pv->id)) {
					if (*rhl->pv->vg_name)
						log_err("PV %s in two VGs "
							"%s and %s",
							dev_name(rhl->pv->dev),
							rhl->pv->vg_name,
							vg->name);
					else
						memcpy(&rhl->pv, &pvl->pv,
						       sizeof(struct
							      physical_volume));
				}
			}
		}
	}

	pool_free(fmt->cmd->mem, names);
	return results;
}

static int _pv_write(struct format_instance *fi, struct physical_volume *pv,
		     void *mdl)
{
	/* No on-disk PV structure change required! */
	/* FIXME vgcache could be wrong */
	return 1;
	//return (fi->fmt->cmd->fmt1->ops->pv_write(fi, pv, NULL));
/*** FIXME Not required?
	struct volume_group *vg;
	struct list *pvh;

	vg = _vg_read(fi, pv->vg_name);

	// Find the PV in this VG 
	if (vg) {
		list_iterate(pvh, &vg->pvs) {
			struct pv_list *vgpv = list_item(pvh, struct pv_list);

			if (id_equal(&pv->id, &vgpv->pv->id)) {
				vgpv->pv->status = pv->status;
				vgpv->pv->size = pv->size;

				// Not sure if it's worth doing these 
				vgpv->pv->pe_size = pv->pe_size;
				vgpv->pv->pe_count = pv->pe_count;
				vgpv->pv->pe_start = pv->pe_start;
				vgpv->pv->pe_alloc_count = pv->pe_alloc_count;

				// Write it back 
				_vg_write(fi, vg);
				pool_free(fi->fmt->cmd->mem, vg);
				return 1;
			}
		}
		pool_free(fi->fmt->cmd->mem, vg);
	}

	// Can't handle PVs not in a VG 
	return 0;
***/
}

static int _pv_read(struct format_type *fmt, const char *pv_name,
		    struct physical_volume *pv)
{
	struct pv_list *pvl;
	struct list *vgh;
	struct list *pvh;
	struct list *names = pool_alloc(fmt->cmd->mem, sizeof(*names));
	struct name_list *nl;
	struct volume_group *vg;
	struct id *id;

	/* FIXME Push up to pv_read */
	if (!(id = uuid_map_lookup_label(fmt->cmd->mem, fmt->cmd->um, pv_name))) {
		stack;
		return 0;
	}

	list_init(names);
	if (!_get_vgs(fmt, names)) {
		stack;
		return 0;
	}

	list_iterate(vgh, names) {

		nl = list_item(vgh, struct name_list);
		if (!(vg = vg_read(fmt->cmd, nl->name))) {
			log_error("format_text: _pv_read failed to read VG %s",
				  nl->name);
			return 0;
		}
		list_iterate(pvh, &vg->pvs) {
			pvl = list_item(pvh, struct pv_list);
			if (id_equal(&pvl->pv->id, id)) {
				memcpy(pv, pvl->pv, sizeof(*pv));
				break;
			}
		}
	}

	pool_free(fmt->cmd->mem, names);
	return 1;
}

static void _destroy_instance(struct format_instance *fid)
{
	return;
}

static void _free_dirs(struct list *dir_list)
{
	struct list *dl, *tmp;

	list_iterate_safe(dl, tmp, dir_list) {
		list_del(dl);
		dbg_free(dl);
	}
}

static void _destroy(struct format_type *fmt)
{
	if (fmt->private) {
		_free_dirs((struct list *) fmt->private);
		dbg_free(fmt->private);
	}

	dbg_free(fmt);
}

static struct format_instance *_create_text_instance(struct format_type *fmt,
						     const char *vgname,
						     void *context)
{
	struct format_instance *fid;
	struct metadata_area *mda;
	struct dir_list *dl;
	struct list *dlh, *dir_list;
	char path[PATH_MAX];

	if (!(fid = pool_alloc(fmt->cmd->mem, sizeof(*fid)))) {
		log_error("Couldn't allocate format instance object.");
		return NULL;
	}

	fid->fmt = fmt;

	list_init(&fid->metadata_areas);

	if (!vgname) {
		if (!(mda = pool_alloc(fmt->cmd->mem, sizeof(*mda)))) {
			stack;
			return NULL;
		}
		mda->metadata_locn = context;
		list_add(&fid->metadata_areas, &mda->list);
	} else {
		dir_list = (struct list *) fmt->private;

		list_iterate(dlh, dir_list) {
			dl = list_item(dlh, struct dir_list);
			if (lvm_snprintf(path, PATH_MAX, "%s/%s",
					 dl->dir, vgname) < 0) {
				log_error("Name too long %s/%s", dl->dir,
					  vgname);
				return NULL;
			}

			context = create_text_context(fmt, path, NULL);
			if (!(mda = pool_alloc(fmt->cmd->mem, sizeof(*mda)))) {
				stack;
				return NULL;
			}
			mda->metadata_locn = context;
			list_add(&fid->metadata_areas, &mda->list);
		}
	}

	return fid;

}

void *create_text_context(struct format_type *fmt, const char *path,
			  const char *desc)
{
	struct text_context *tc;
	char *tmp;

	if ((tmp = strstr(path, ".tmp")) && (tmp == path + strlen(path) - 4)) {
		log_error("%s: Volume group filename may not end in .tmp",
			  path);
		return NULL;
	}

	if (!(tc = pool_alloc(fmt->cmd->mem, sizeof(*tc)))) {
		stack;
		return NULL;
	}

	if (!(tc->path_live = pool_strdup(fmt->cmd->mem, path))) {
		stack;
		goto no_mem;
	}

	if (!(tc->path_edit = pool_alloc(fmt->cmd->mem, strlen(path) + 5))) {
		stack;
		goto no_mem;
	}
	sprintf(tc->path_edit, "%s.tmp", path);

	if (!desc)
		desc = "";

	if (!(tc->desc = pool_strdup(fmt->cmd->mem, desc))) {
		stack;
		goto no_mem;
	}

	return (void *) tc;

      no_mem:
	pool_free(fmt->cmd->mem, tc);

	log_err("Couldn't allocate text format context object.");
	return NULL;
}

static struct format_handler _text_handler = {
	get_vgs:	_get_vgs,
	get_pvs:	_get_pvs,
	pv_read:	_pv_read,
	pv_setup:	_pv_setup,
	pv_write:	_pv_write,
	pv_commit:	_pv_commit,
	vg_setup:	_vg_setup,
	lv_setup:	_lv_setup,
	vg_read:	_vg_read,
	vg_write:	_vg_write,
	vg_remove:	_vg_remove,
	vg_commit:	_vg_commit,
	create_instance:_create_text_instance,
	destroy_instance:_destroy_instance,
	destroy:	_destroy
};

static int _add_dir(const char *dir, struct list *dir_list)
{
	struct dir_list *dl;

	if (create_dir(dir)) {
		if (!(dl = dbg_malloc(sizeof(struct list) + strlen(dir) + 1))) {
			log_error("_add_dir allocation failed");
			return 0;
		}
		strcpy(dl->dir, dir);
		list_add(dir_list, &dl->list);
		return 1;
	}

	return 0;
}

struct format_type *create_text_format(struct cmd_context *cmd)
{
	struct format_type *fmt;
	struct config_node *cn;
	struct config_value *cv;
	struct list *dir_list;

	if (!(fmt = dbg_malloc(sizeof(*fmt)))) {
		stack;
		return NULL;
	}

	fmt->cmd = cmd;
	fmt->ops = &_text_handler;
	fmt->name = FMT_TEXT_NAME;
	fmt->features = FMT_SEGMENTS;

	if (!(dir_list = dbg_malloc(sizeof(struct list)))) {
		log_error("Failed to allocate dir_list");
		return NULL;
	}

	list_init(dir_list);
	fmt->private = (void *) dir_list;

	if (!(cn = find_config_node(cmd->cf->root, "metadata/dirs", '/'))) {
		log_verbose("metadata/dirs not in config file: Defaulting "
			    "to /etc/lvm/metadata");
		_add_dir("/etc/lvm/metadata", dir_list);
		return fmt;
	}

	for (cv = cn->v; cv; cv = cv->next) {
		if (cv->type != CFG_STRING) {
			log_error("Invalid string in config file: "
				  "metadata/dirs");
			goto err;
		}

		if (!_add_dir(cv->v.str, dir_list)) {
			log_error("Failed to add %s to internal device cache",
				  cv->v.str);
			goto err;
		}
	}

	return fmt;

      err:
	_free_dirs(dir_list);

	dbg_free(fmt);
	return NULL;
}
