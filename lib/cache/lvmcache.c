/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "lib.h"
#include "lvmcache.h"
#include "toolcontext.h"
#include "dev-cache.h"
#include "metadata.h"
#include "filter.h"
#include "memlock.h"
#include "str_list.h"

static struct dm_hash_table *_pvid_hash = NULL;
static struct dm_hash_table *_vgid_hash = NULL;
static struct dm_hash_table *_vgname_hash = NULL;
static struct dm_hash_table *_lock_hash = NULL;
static struct list _vginfos;
static int _has_scanned = 0;
static int _vgs_locked = 0;

int lvmcache_init(void)
{
	list_init(&_vginfos);

	if (!(_vgname_hash = dm_hash_create(128)))
		return 0;

	if (!(_vgid_hash = dm_hash_create(128)))
		return 0;

	if (!(_pvid_hash = dm_hash_create(128)))
		return 0;

	if (!(_lock_hash = dm_hash_create(128)))
		return 0;

	return 1;
}

void lvmcache_lock_vgname(const char *vgname, int read_only)
{
	if (!_lock_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		return;
	}

	if (!dm_hash_insert(_lock_hash, vgname, (void *) 1))
		log_error("Cache locking failure for %s", vgname);

	_vgs_locked++;
}

int vgname_is_locked(const char *vgname)
{
	if (!_lock_hash)
		return 0;

	return dm_hash_lookup(_lock_hash, vgname) ? 1 : 0;
}

void lvmcache_unlock_vgname(const char *vgname)
{
	/* FIXME: Clear all CACHE_LOCKED flags in this vg */
	dm_hash_remove(_lock_hash, vgname);

	/* FIXME Do this per-VG */
	if (!--_vgs_locked)
		dev_close_all();
}

int vgs_locked(void)
{
	return _vgs_locked;
}

struct lvmcache_vginfo *vginfo_from_vgname(const char *vgname)
{
	struct lvmcache_vginfo *vginfo;

	if (!_vgname_hash)
		return NULL;

	if (!(vginfo = dm_hash_lookup(_vgname_hash, vgname)))
		return NULL;

	return vginfo;
}

const struct format_type *fmt_from_vgname(const char *vgname)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	struct label *label;
	struct list *devh, *tmp;
	struct list devs;
	struct device_list *devl;

	if (!(vginfo = vginfo_from_vgname(vgname)))
		return NULL;

	/* This function is normally called before reading metadata so
 	 * we check cached labels here. Unfortunately vginfo is volatile. */
	list_init(&devs);
	list_iterate_items(info, &vginfo->infos) {
		devl = dm_malloc(sizeof(*devl));
		devl->dev = info->dev;
		list_add(&devs, &devl->list);
	}

	list_iterate_safe(devh, tmp, &devs) {
		devl = list_item(devh, struct device_list);
		label_read(devl->dev, &label);
		list_del(&devl->list);
		dm_free(devl);
	}

	return vginfo->fmt;
}

struct lvmcache_vginfo *vginfo_from_vgid(const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	char id[ID_LEN + 1];

	if (!_vgid_hash || !vgid)
		return NULL;

	/* vgid not necessarily NULL-terminated */
	strncpy(&id[0], vgid, ID_LEN);
	id[ID_LEN] = '\0';

	if (!(vginfo = dm_hash_lookup(_vgid_hash, id)))
		return NULL;

	return vginfo;
}

struct lvmcache_info *info_from_pvid(const char *pvid)
{
	struct lvmcache_info *info;
	char id[ID_LEN + 1];

	if (!_pvid_hash || !pvid)
		return NULL;

	strncpy(&id[0], pvid, ID_LEN);
	id[ID_LEN] = '\0';

	if (!(info = dm_hash_lookup(_pvid_hash, id)))
		return NULL;

	return info;
}

static void _rescan_entry(struct lvmcache_info *info)
{
	struct label *label;

	if (info->status & CACHE_INVALID)
		label_read(info->dev, &label);
}

static int _scan_invalid(void)
{
	dm_hash_iter(_pvid_hash, (dm_hash_iterate_fn) _rescan_entry);

	return 1;
}

int lvmcache_label_scan(struct cmd_context *cmd, int full_scan)
{
	struct label *label;
	struct dev_iter *iter;
	struct device *dev;
	struct format_type *fmt;

	static int _scanning_in_progress = 0;
	int r = 0;

	/* Avoid recursion when a PVID can't be found! */
	if (_scanning_in_progress)
		return 0;

	_scanning_in_progress = 1;

	if (!_vgname_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		goto out;
	}

	if (_has_scanned && !full_scan) {
		r = _scan_invalid();
		goto out;
	}

	if (!(iter = dev_iter_create(cmd->filter, (full_scan == 2) ? 1: 0))) {
		log_error("dev_iter creation failed");
		goto out;
	}

	while ((dev = dev_iter_get(iter)))
		label_read(dev, &label);

	dev_iter_destroy(iter);

	_has_scanned = 1;

	/* Perform any format-specific scanning e.g. text files */
	list_iterate_items(fmt, &cmd->formats) {
		if (fmt->ops->scan && !fmt->ops->scan(fmt))
			goto out;
	}

	r = 1;

      out:
	_scanning_in_progress = 0;

	return r;
}

struct list *lvmcache_get_vgnames(struct cmd_context *cmd, int full_scan)
{
	struct list *vgnames;
	struct lvmcache_vginfo *vgi;

	lvmcache_label_scan(cmd, full_scan);

	if (!(vgnames = str_list_create(cmd->mem))) {
		log_error("vgnames list allocation failed");
		return NULL;
	}

	list_iterate_items(vgi, &_vginfos) {
		if (!str_list_add(cmd->mem, vgnames, 
				  dm_pool_strdup(cmd->mem, vgi->vgname))) {
			log_error("strlist allocation failed");
			return NULL;
		}
	}

	return vgnames;
}

struct device *device_from_pvid(struct cmd_context *cmd, struct id *pvid)
{
	struct label *label;
	struct lvmcache_info *info;

	/* Already cached ? */
	if ((info = info_from_pvid((char *) pvid))) {
		if (label_read(info->dev, &label)) {
			info = (struct lvmcache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid))
				return info->dev;
		}
	}

	lvmcache_label_scan(cmd, 0);

	/* Try again */
	if ((info = info_from_pvid((char *) pvid))) {
		if (label_read(info->dev, &label)) {
			info = (struct lvmcache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid))
				return info->dev;
		}
	}

	if (memlock())
		return NULL;

	lvmcache_label_scan(cmd, 2);

	/* Try again */
	if ((info = info_from_pvid((char *) pvid))) {
		if (label_read(info->dev, &label)) {
			info = (struct lvmcache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid))
				return info->dev;
		}
	}

	return NULL;
}

static void _drop_vginfo(struct lvmcache_info *info)
{
	if (!list_empty(&info->list)) {
		list_del(&info->list);
		list_init(&info->list);
	}

	if (info->vginfo && list_empty(&info->vginfo->infos)) {
		dm_hash_remove(_vgname_hash, info->vginfo->vgname);
		if (info->vginfo->vgname)
			dm_free(info->vginfo->vgname);
		if (*info->vginfo->vgid)
			dm_hash_remove(_vgid_hash, info->vginfo->vgid);
		list_del(&info->vginfo->list);
		dm_free(info->vginfo);
	}

	info->vginfo = NULL;
}

/* Unused
void lvmcache_del(struct lvmcache_info *info)
{
	if (info->dev->pvid[0] && _pvid_hash)
		dm_hash_remove(_pvid_hash, info->dev->pvid);

	_drop_vginfo(info);

	info->label->labeller->ops->destroy_label(info->label->labeller,
						info->label); 
	dm_free(info);

	return;
} */

static int _lvmcache_update_pvid(struct lvmcache_info *info, const char *pvid)
{
	if (!strcmp(info->dev->pvid, pvid))
		return 1;
	if (*info->dev->pvid) {
		dm_hash_remove(_pvid_hash, info->dev->pvid);
	}
	strncpy(info->dev->pvid, pvid, sizeof(info->dev->pvid));
	if (!dm_hash_insert(_pvid_hash, pvid, info)) {
		log_error("_lvmcache_update: pvid insertion failed: %s", pvid);
		return 0;
	}

	return 1;
}

static int _lvmcache_update_vgid(struct lvmcache_info *info, const char *vgid)
{
	if (!vgid || !info->vginfo || !strncmp(info->vginfo->vgid, vgid,
					       sizeof(info->vginfo->vgid)))
		return 1;

	if (info->vginfo && *info->vginfo->vgid)
		dm_hash_remove(_vgid_hash, info->vginfo->vgid);
	if (!vgid) {
		log_debug("lvmcache: %s: clearing VGID", dev_name(info->dev));
		return 1;
	}

	strncpy(info->vginfo->vgid, vgid, sizeof(info->vginfo->vgid));
	info->vginfo->vgid[sizeof(info->vginfo->vgid) - 1] = '\0';
	if (!dm_hash_insert(_vgid_hash, info->vginfo->vgid, info->vginfo)) {
		log_error("_lvmcache_update: vgid hash insertion failed: %s",
			  info->vginfo->vgid);
		return 0;
	}

	log_debug("lvmcache: %s: setting %s VGID to %s", dev_name(info->dev),
		  info->vginfo->vgname, info->vginfo->vgid);

	return 1;
}

static int _lvmcache_update_vgname(struct lvmcache_info *info,
				   const char *vgname)
{
	struct lvmcache_vginfo *vginfo;

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
		if (!(vginfo = dm_malloc(sizeof(*vginfo)))) {
			log_error("lvmcache_update_vgname: list alloc failed");
			return 0;
		}
		memset(vginfo, 0, sizeof(*vginfo));
		if (!(vginfo->vgname = dm_strdup(vgname))) {
			dm_free(vginfo);
			log_error("cache vgname alloc failed for %s", vgname);
			return 0;
		}
		list_init(&vginfo->infos);
		if (!dm_hash_insert(_vgname_hash, vginfo->vgname, vginfo)) {
			log_error("cache_update: vg hash insertion failed: %s",
				  vginfo->vgname);
			dm_free(vginfo->vgname);
			dm_free(vginfo);
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

	log_debug("lvmcache: %s: now %s%s%s%s%s", dev_name(info->dev),
		  *vgname ? "in VG " : "orphaned", vgname,
		  vginfo->vgid[0] ? " (" : "",
		  vginfo->vgid[0] ? vginfo->vgid : "",
		  vginfo->vgid[0] ? ")" : "");

	return 1;
}

int lvmcache_update_vgname_and_id(struct lvmcache_info *info, const char *vgname, const char *vgid)
{
	if (!_lvmcache_update_vgname(info, vgname) ||
	    !_lvmcache_update_vgid(info, vgid))
		return_0;

	return 1;
}

int lvmcache_update_vg(struct volume_group *vg)
{
	struct pv_list *pvl;
	struct lvmcache_info *info;
	char pvid_s[ID_LEN + 1];

	pvid_s[sizeof(pvid_s) - 1] = '\0';

	list_iterate_items(pvl, &vg->pvs) {
		strncpy(pvid_s, (char *) &pvl->pv->id, sizeof(pvid_s) - 1);
		/* FIXME Could pvl->pv->dev->pvid ever be different? */
		if ((info = info_from_pvid(pvid_s)) &&
		    !lvmcache_update_vgname_and_id(info, vg->name,
						   (char *) &vg->id))
			return_0;
	}

	return 1;
}

struct lvmcache_info *lvmcache_add(struct labeller *labeller, const char *pvid,
				   struct device *dev,
				   const char *vgname, const char *vgid)
{
	struct label *label;
	struct lvmcache_info *existing, *info;
	char pvid_s[ID_LEN + 1];

	if (!_vgname_hash && !lvmcache_init()) {
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
		if (!(info = dm_malloc(sizeof(*info)))) {
			log_error("lvmcache_info allocation failed");
			label_destroy(label);
			return NULL;
		}
		memset(info, 0, sizeof(*info));

		label->info = info;
		info->label = label;
		list_init(&info->list);
		info->dev = dev;
	} else {
		if (existing->dev != dev) {
			/* Is the existing entry a duplicate pvid e.g. md ? */
			if (MAJOR(existing->dev->dev) == md_major() &&
			    MAJOR(dev->dev) != md_major()) {
				log_very_verbose("Ignoring duplicate PV %s on "
						 "%s - using md %s",
						 pvid, dev_name(dev),
						 dev_name(existing->dev));
				return NULL;
			} else if (dm_is_dm_major(MAJOR(existing->dev->dev)) &&
				   !dm_is_dm_major(MAJOR(dev->dev))) {
				log_very_verbose("Ignoring duplicate PV %s on "
						 "%s - using dm %s",
						 pvid, dev_name(dev),
						 dev_name(existing->dev));
				return NULL;
			} else if (MAJOR(existing->dev->dev) != md_major() &&
				   MAJOR(dev->dev) == md_major())
				log_very_verbose("Duplicate PV %s on %s - "
						 "using md %s", pvid,
						 dev_name(existing->dev),
						 dev_name(dev));
			else if (!dm_is_dm_major(MAJOR(existing->dev->dev)) &&
				 dm_is_dm_major(MAJOR(dev->dev)))
				log_very_verbose("Duplicate PV %s on %s - "
						 "using dm %s", pvid,
						 dev_name(existing->dev),
						 dev_name(dev));
			/* FIXME If both dm, check dependencies */
			//else if (dm_is_dm_major(MAJOR(existing->dev->dev)) &&
				 //dm_is_dm_major(MAJOR(dev->dev)))
				 //
			else
				log_error("Found duplicate PV %s: using %s not "
					  "%s", pvid, dev_name(dev),
					  dev_name(existing->dev));
		}
		/* Switch over to new preferred device */
		existing->dev = dev;
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

	if (!_lvmcache_update_pvid(info, pvid_s)) {
		if (!existing) {
			dm_free(info);
			label_destroy(label);
		}
		return NULL;
	}

	if (!lvmcache_update_vgname_and_id(info, vgname, vgid)) {
		if (!existing) {
			dm_hash_remove(_pvid_hash, pvid_s);
			strcpy(info->dev->pvid, "");
			dm_free(info);
			label_destroy(label);
		}
		return NULL;
	}

	return info;
}

static void _lvmcache_destroy_entry(struct lvmcache_info *info)
{
	if (!list_empty(&info->list))
		list_del(&info->list);
	strcpy(info->dev->pvid, "");
	label_destroy(info->label);
	dm_free(info);
}

static void _lvmcache_destroy_vgnamelist(struct lvmcache_vginfo *vginfo)
{
	if (vginfo->vgname)
		dm_free(vginfo->vgname);
	dm_free(vginfo);
}

static void _lvmcache_destroy_lockname(int present)
{
	/* Nothing to do */
}

void lvmcache_destroy(void)
{
	log_verbose("Wiping internal VG cache");

	_has_scanned = 0;

	if (_vgid_hash) {
		dm_hash_destroy(_vgid_hash);
		_vgid_hash = NULL;
	}

	if (_pvid_hash) {
		dm_hash_iter(_pvid_hash, (dm_hash_iterate_fn) _lvmcache_destroy_entry);
		dm_hash_destroy(_pvid_hash);
		_pvid_hash = NULL;
	}

	if (_vgname_hash) {
		dm_hash_iter(_vgname_hash,
			  (dm_hash_iterate_fn) _lvmcache_destroy_vgnamelist);
		dm_hash_destroy(_vgname_hash);
		_vgname_hash = NULL;
	}

	if (_lock_hash) {
		dm_hash_iter(_lock_hash, (dm_hash_iterate_fn) _lvmcache_destroy_lockname);
		dm_hash_destroy(_lock_hash);
		_lock_hash = NULL;
	}

	list_init(&_vginfos);
}
