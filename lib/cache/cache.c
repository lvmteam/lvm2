/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "lib.h"
#include "cache.h"
#include "hash.h"
#include "toolcontext.h"
#include "dev-cache.h"
#include "metadata.h"

static struct hash_table *_pvid_hash = NULL;
static struct hash_table *_vgid_hash = NULL;
static struct hash_table *_vgname_hash = NULL;
static struct list _vginfos;
int _has_scanned = 0;

int cache_init(void)
{
	list_init(&_vginfos);

	if (!(_vgname_hash = hash_create(128)))
		return 0;

	if (!(_vgid_hash = hash_create(128)))
		return 0;

	if (!(_pvid_hash = hash_create(128)))
		return 0;

	return 1;
}

struct cache_vginfo *vginfo_from_vgname(const char *vgname)
{
	struct cache_vginfo *vginfo;

	if (!_vgname_hash)
		return NULL;

	if (!(vginfo = hash_lookup(_vgname_hash, vgname)))
		return NULL;

	return vginfo;
}

const struct format_type *fmt_from_vgname(const char *vgname)
{
	struct cache_vginfo *vginfo;

	if (!(vginfo = vginfo_from_vgname(vgname)))
		return NULL;

	return vginfo->fmt;
}

struct cache_vginfo *vginfo_from_vgid(const char *vgid)
{
	struct cache_vginfo *vginfo;

	if (!_vgid_hash || !vgid)
		return NULL;

	if (!(vginfo = hash_lookup_fixed(_vgid_hash, vgid, ID_LEN)))
		return NULL;

	return vginfo;
}

struct cache_info *info_from_pvid(const char *pvid)
{
	struct cache_info *info;

	if (!_pvid_hash || !pvid)
		return NULL;

	if (!(info = hash_lookup_fixed(_pvid_hash, pvid, ID_LEN)))
		return NULL;

	return info;
}

static void _rescan_entry(struct cache_info *info)
{
	struct label *label;

	if (info->status & CACHE_INVALID)
		label_read(info->dev, &label);
}

static int _scan_invalid(void)
{
	hash_iter(_pvid_hash, (iterate_fn) _rescan_entry);

	return 1;
}

int cache_label_scan(struct cmd_context *cmd, int full_scan)
{
	struct label *label;
	struct dev_iter *iter;
	struct device *dev;
	struct list *fmth;
	struct format_type *fmt;

	static int _scanning_in_progress = 0;
	int r = 0;

	/* Avoid recursion when a PVID can't be found! */
	if (_scanning_in_progress)
		return 0;

	_scanning_in_progress = 1;

	if (!_vgname_hash && !cache_init()) {
		log_error("Internal cache initialisation failed");
		goto out;
	}

	if (_has_scanned && !full_scan) {
		r = _scan_invalid();
		goto out;
	}

	if (!(iter = dev_iter_create(cmd->filter))) {
		log_error("dev_iter creation failed");
		goto out;
	}

	while ((dev = dev_iter_get(iter)))
		label_read(dev, &label);

	dev_iter_destroy(iter);

	_has_scanned = 1;

	/* Perform any format-specific scanning e.g. text files */
	list_iterate(fmth, &cmd->formats) {
		fmt = list_item(fmth, struct format_type);
		if (fmt->ops->scan && !fmt->ops->scan(fmt))
			goto out;
	}

	r = 1;

      out:
	_scanning_in_progress = 0;

	return r;
}

struct list *cache_get_vgnames(struct cmd_context *cmd, int full_scan)
{
	struct list *vgih, *vgnames;
	struct str_list *sl;

	cache_label_scan(cmd, full_scan);

	if (!(vgnames = pool_alloc(cmd->mem, sizeof(struct list)))) {
		log_error("vgnames list allocation failed");
		return NULL;
	}

	list_init(vgnames);

	list_iterate(vgih, &_vginfos) {
		if (!(sl = pool_alloc(cmd->mem, sizeof(*sl)))) {
			log_error("strlist allocation failed");
			return NULL;
		}
		if (!(sl->str = pool_strdup(cmd->mem,
					    list_item(vgih,
						      struct cache_vginfo)->
					    vgname))) {
			log_error("vgname allocation failed");
			return NULL;
		}
		list_add(vgnames, &sl->list);
	}

	return vgnames;
}

struct device *device_from_pvid(struct cmd_context *cmd, struct id *pvid)
{
	struct label *label;
	struct cache_info *info;

	/* Already cached ? */
	if ((info = info_from_pvid((char *) pvid))) {
		if (label_read(info->dev, &label)) {
			info = (struct cache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid))
				return info->dev;
		}
	}

	cache_label_scan(cmd, 0);

	/* Try again */
	if ((info = info_from_pvid((char *) pvid))) {
		if (label_read(info->dev, &label)) {
			info = (struct cache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid))
				return info->dev;
		}
	}

	cache_label_scan(cmd, 1);

	/* Try again */
	if ((info = info_from_pvid((char *) pvid))) {
		if (label_read(info->dev, &label)) {
			info = (struct cache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid))
				return info->dev;
		}
	}

	return NULL;
}

static void _drop_vginfo(struct cache_info *info)
{
	if (!list_empty(&info->list)) {
		list_del(&info->list);
		list_init(&info->list);
	}

	if (info->vginfo && list_empty(&info->vginfo->infos)) {
		hash_remove(_vgname_hash, info->vginfo->vgname);
		if (info->vginfo->vgname)
			dbg_free(info->vginfo->vgname);
		if (*info->vginfo->vgid)
			hash_remove(_vgid_hash, info->vginfo->vgid);
		list_del(&info->vginfo->list);
		dbg_free(info->vginfo);
	}

	info->vginfo = NULL;
}

/* Unused
void cache_del(struct cache_info *info)
{
	if (info->dev->pvid[0] && _pvid_hash)
		hash_remove(_pvid_hash, info->dev->pvid);

	_drop_vginfo(info);

	info->label->labeller->ops->destroy_label(info->label->labeller,
						info->label); 
	dbg_free(info);

	return;
} */

static int _cache_update_pvid(struct cache_info *info, const char *pvid)
{
	if (!strcmp(info->dev->pvid, pvid))
		return 1;
	if (*info->dev->pvid) {
		hash_remove(_pvid_hash, info->dev->pvid);
	}
	strncpy(info->dev->pvid, pvid, sizeof(info->dev->pvid));
	if (!hash_insert(_pvid_hash, pvid, info)) {
		log_error("_cache_update: pvid insertion failed: %s", pvid);
		return 0;
	}

	return 1;
}

static int _cache_update_vgid(struct cache_info *info, const char *vgid)
{
	if (!vgid || !info->vginfo || !strncmp(info->vginfo->vgid, vgid,
					       sizeof(info->vginfo->vgid)))
		return 1;

	if (info->vginfo && *info->vginfo->vgid)
		hash_remove(_vgid_hash, info->vginfo->vgid);
	if (!vgid)
		return 1;

	strncpy(info->vginfo->vgid, vgid, sizeof(info->vginfo->vgid));
	info->vginfo->vgid[sizeof(info->vginfo->vgid) - 1] = '\0';
	if (!hash_insert(_vgid_hash, vgid, info->vginfo)) {
		log_error("_cache_update: vgid hash insertion failed: %s",
			  vgid);
		return 0;
	}

	return 1;
}

int cache_update_vgname(struct cache_info *info, const char *vgname)
{
	struct cache_vginfo *vginfo;

	/* If vgname is NULL and we don't already have a vgname, 
	 * assume ORPHAN - we want every entry to have a vginfo
	 * attached for scanning reasons.
	 */
	if (!vgname && !info->vginfo)
		vgname = ORPHAN;

	if (!vgname || (info->vginfo && !strcmp(info->vginfo->vgname, vgname)))
		return 1;

	/* Remove existing vginfo entry */
	_drop_vginfo(info);

	/* Get existing vginfo or create new one */
	if (!(vginfo = vginfo_from_vgname(vgname))) {
		if (!(vginfo = dbg_malloc(sizeof(*vginfo)))) {
			log_error("cache_update_vgname: list alloc failed");
			return 0;
		}
		memset(vginfo, 0, sizeof(*vginfo));
		if (!(vginfo->vgname = dbg_strdup(vgname))) {
			dbg_free(vginfo);
			log_error("cache vgname alloc failed for %s", vgname);
			return 0;
		}
		list_init(&vginfo->infos);
		if (!hash_insert(_vgname_hash, vginfo->vgname, vginfo)) {
			log_error("cache_update: vg hash insertion failed: %s",
				  vginfo->vgname);
			dbg_free(vginfo->vgname);
			dbg_free(vginfo);
			return 0;
		}
		/* Ensure orphans appear last on list_iterate */
		if (!*vgname)
			list_add(&_vginfos, &vginfo->list);
		else
			list_add_h(&_vginfos, &vginfo->list);
	}

	info->vginfo = vginfo;
	list_add(&vginfo->infos, &info->list);

	/* FIXME Check consistency of list! */
	vginfo->fmt = info->fmt;

	return 1;
}

int cache_update_vg(struct volume_group *vg)
{
	struct list *pvh;
	struct physical_volume *pv;
	struct cache_info *info;
	char pvid_s[ID_LEN + 1];
	int vgid_updated = 0;

	pvid_s[sizeof(pvid_s) - 1] = '\0';

	list_iterate(pvh, &vg->pvs) {
		pv = list_item(pvh, struct pv_list)->pv;
		strncpy(pvid_s, (char *) &pv->id, sizeof(pvid_s) - 1);
		/* FIXME Could pv->dev->pvid ever be different? */
		if ((info = info_from_pvid(pvid_s))) {
			cache_update_vgname(info, vg->name);
			if (!vgid_updated) {
				_cache_update_vgid(info, (char *) &vg->id);
				vgid_updated = 1;
			}
		}
	}

	return 1;
}

struct cache_info *cache_add(struct labeller *labeller, const char *pvid,
			     struct device *dev,
			     const char *vgname, const char *vgid)
{
	struct label *label;
	struct cache_info *existing, *info;
	char pvid_s[ID_LEN + 1];

	if (!_vgname_hash && !cache_init()) {
		log_error("Internal cache initialisation failed");
		return NULL;
	}

	strncpy(pvid_s, pvid, sizeof(pvid_s));
	pvid_s[sizeof(pvid_s) - 1] = '\0';

	if (!(existing = info_from_pvid(pvid_s)) &&
	    !(existing = info_from_pvid(dev->pvid))) {
		if (!(label = label_create(labeller))) {
			stack;
			return NULL;
		}
		if (!(info = dbg_malloc(sizeof(*info)))) {
			log_error("cache_info allocation failed");
			label_destroy(label);
			return NULL;
		}
		memset(info, 0, sizeof(*info));

		label->info = info;
		info->label = label;
		list_init(&info->list);
		info->dev = dev;
	} else {
		info = existing;
		/* Has labeller changed? */
		if (info->label->labeller != labeller) {
			label_destroy(info->label);
			if (!(info->label = label_create(labeller))) {
				/* FIXME leaves info without label! */
				stack;
				return NULL;
			}
			info->label->info = info;
		}
		label = info->label;
	}

	info->fmt = (const struct format_type *) labeller->private;
	info->status |= CACHE_INVALID;

	if (!_cache_update_pvid(info, pvid_s)) {
		if (!existing) {
			dbg_free(info);
			label_destroy(label);
		}
		return NULL;
	}

	if (!cache_update_vgname(info, vgname)) {
		if (!existing) {
			hash_remove(_pvid_hash, pvid_s);
			strcpy(info->dev->pvid, "");
			dbg_free(info);
			label_destroy(label);
		}
		return NULL;
	}

	if (!_cache_update_vgid(info, vgid))
		/* Non-critical */
		stack;

	return info;
}

static void _cache_destroy_entry(struct cache_info *info)
{
	if (!list_empty(&info->list))
		list_del(&info->list);
	strcpy(info->dev->pvid, "");
	label_destroy(info->label);
	dbg_free(info);
}

static void _cache_destroy_vgnamelist(struct cache_vginfo *vginfo)
{
	if (vginfo->vgname)
		dbg_free(vginfo->vgname);
	dbg_free(vginfo);
}

void cache_destroy(void)
{
	_has_scanned = 0;

	if (_vgid_hash) {
		hash_destroy(_vgid_hash);
		_vgid_hash = NULL;
	}

	if (_pvid_hash) {
		hash_iter(_pvid_hash, (iterate_fn) _cache_destroy_entry);
		hash_destroy(_pvid_hash);
		_pvid_hash = NULL;
	}

	if (_vgname_hash) {
		hash_iter(_vgname_hash, (iterate_fn) _cache_destroy_vgnamelist);
		hash_destroy(_vgname_hash);
		_vgname_hash = NULL;
	}
	list_init(&_vginfos);
}
