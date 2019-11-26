/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/cache/lvmcache.h"
#include "lib/commands/toolcontext.h"
#include "lib/device/dev-cache.h"
#include "lib/locking/locking.h"
#include "lib/metadata/metadata.h"
#include "lib/mm/memlock.h"
#include "lib/format_text/format-text.h"
#include "lib/config/config.h"

/* One per device */
struct lvmcache_info {
	struct dm_list list;	/* Join VG members together */
	struct dm_list mdas;	/* list head for metadata areas */
	struct dm_list das;	/* list head for data areas */
	struct dm_list bas;	/* list head for bootloader areas */
	struct dm_list bad_mdas;/* list head for bad metadata areas */
	struct lvmcache_vginfo *vginfo;	/* NULL == unknown */
	struct label *label;
	const struct format_type *fmt;
	struct device *dev;
	uint64_t device_size;	/* Bytes */
	uint32_t ext_version;   /* Extension version */
	uint32_t ext_flags;	/* Extension flags */
	uint32_t status;
	bool mda1_bad;		/* label scan found bad metadata in mda1 */
	bool mda2_bad;		/* label scan found bad metadata in mda2 */
	bool summary_seqno_mismatch; /* two mdas on this dev has mismatching metadata */
	int summary_seqno;      /* vg seqno found on this dev during scan */
	int mda1_seqno;
	int mda2_seqno;
};

/* One per VG */
struct lvmcache_vginfo {
	struct dm_list list;	/* Join these vginfos together */
	struct dm_list infos;	/* List head for lvmcache_infos */
	struct dm_list outdated_infos; /* vg_read moves info from infos to outdated_infos */
	struct dm_list pvsummaries; /* pv_list taken directly from vgsummary */
	const struct format_type *fmt;
	char *vgname;		/* "" == orphan */
	uint32_t status;
	char vgid[ID_LEN + 1];
	char _padding[7];
	struct lvmcache_vginfo *next; /* Another VG with same name? */
	char *creation_host;
	char *system_id;
	char *lock_type;
	uint32_t mda_checksum;
	size_t mda_size;
	int seqno;
	bool scan_summary_mismatch; /* vgsummary from devs had mismatching seqno or checksum */
};

static struct dm_hash_table *_pvid_hash = NULL;
static struct dm_hash_table *_vgid_hash = NULL;
static struct dm_hash_table *_vgname_hash = NULL;
static DM_LIST_INIT(_vginfos);
static DM_LIST_INIT(_initial_duplicates);
static DM_LIST_INIT(_unused_duplicates);
static DM_LIST_INIT(_prev_unused_duplicate_devs);
static int _vgs_locked = 0;
static int _found_duplicate_vgnames = 0;

int lvmcache_init(struct cmd_context *cmd)
{
	/*
	 * FIXME add a proper lvmcache_locking_reset() that
	 * resets the cache so no previous locks are locked
	 */
	_vgs_locked = 0;

	dm_list_init(&_vginfos);
	dm_list_init(&_initial_duplicates);
	dm_list_init(&_unused_duplicates);
	dm_list_init(&_prev_unused_duplicate_devs);

	if (!(_vgname_hash = dm_hash_create(128)))
		return 0;

	if (!(_vgid_hash = dm_hash_create(128)))
		return 0;

	if (!(_pvid_hash = dm_hash_create(128)))
		return 0;

	return 1;
}

void lvmcache_lock_vgname(const char *vgname, int read_only __attribute__((unused)))
{
	_vgs_locked++;
}

void lvmcache_unlock_vgname(const char *vgname)
{
	/* FIXME Do this per-VG */
	if (!--_vgs_locked) {
		dev_size_seqno_inc(); /* invalidate all cached dev sizes */
	}
}

int lvmcache_found_duplicate_vgnames(void)
{
	return _found_duplicate_vgnames;
}

static struct device_list *_get_devl_in_device_list(struct device *dev, struct dm_list *head)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, head) {
		if (devl->dev == dev)
			return devl;
	}
	return NULL;
}

int dev_in_device_list(struct device *dev, struct dm_list *head)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, head) {
		if (devl->dev == dev)
			return 1;
	}
	return 0;
}

bool lvmcache_has_duplicate_devs(void)
{
	if (dm_list_empty(&_unused_duplicates) && dm_list_empty(&_initial_duplicates))
		return false;
	return true;
}

int lvmcache_get_unused_duplicates(struct cmd_context *cmd, struct dm_list *head)
{
	struct device_list *devl, *devl2;

	dm_list_iterate_items(devl, &_unused_duplicates) {
		if (!(devl2 = dm_pool_alloc(cmd->mem, sizeof(*devl2)))) {
			log_error("device_list element allocation failed");
			return 0;
		}
		devl2->dev = devl->dev;
		dm_list_add(head, &devl2->list);
	}
	return 1;
}

void lvmcache_del_dev_from_duplicates(struct device *dev)
{
	struct device_list *devl;

	if ((devl = _get_devl_in_device_list(dev, &_initial_duplicates))) {
		log_debug_cache("delete dev from initial duplicates %s", dev_name(dev));
		dm_list_del(&devl->list);
	}
	if ((devl = _get_devl_in_device_list(dev, &_unused_duplicates))) {
		log_debug_cache("delete dev from unused duplicates %s", dev_name(dev));
		dm_list_del(&devl->list);
	}
}

static void _destroy_device_list(struct dm_list *head)
{
	struct device_list *devl, *devl2;

	dm_list_iterate_items_safe(devl, devl2, head) {
		dm_list_del(&devl->list);
		free(devl);
	}
	dm_list_init(head);
}

bool lvmcache_has_bad_metadata(struct device *dev)
{
	struct lvmcache_info *info;

	if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
		/* shouldn't happen */
		log_error("No lvmcache info for checking bad metadata on %s", dev_name(dev));
		return false;
	}

	if (info->mda1_bad || info->mda2_bad)
		return true;
	return false;
}

void lvmcache_save_bad_mda(struct lvmcache_info *info, struct metadata_area *mda)
{
	if (mda->mda_num == 1)
		info->mda1_bad = true;
	else if (mda->mda_num == 2)
		info->mda2_bad = true;
	dm_list_add(&info->bad_mdas, &mda->list);
}

void lvmcache_get_bad_mdas(struct cmd_context *cmd,
			   const char *vgname, const char *vgid,
                           struct dm_list *bad_mda_list)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	struct mda_list *mdal;
	struct metadata_area *mda, *mda2;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		log_error(INTERNAL_ERROR "lvmcache_get_bad_mdas no vginfo %s", vgname);
		return;
	}

	dm_list_iterate_items(info, &vginfo->infos) {
		dm_list_iterate_items_safe(mda, mda2, &info->bad_mdas) {
			if (!(mdal = zalloc(sizeof(*mdal))))
				continue;
			mdal->mda = mda;
			dm_list_add(bad_mda_list, &mdal->list);
		}
	}
}

void lvmcache_get_mdas(struct cmd_context *cmd,
		       const char *vgname, const char *vgid,
                       struct dm_list *mda_list)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	struct mda_list *mdal;
	struct metadata_area *mda, *mda2;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		log_error(INTERNAL_ERROR "lvmcache_get_mdas no vginfo %s", vgname);
		return;
	}

	dm_list_iterate_items(info, &vginfo->infos) {
		dm_list_iterate_items_safe(mda, mda2, &info->mdas) {
			if (!(mdal = zalloc(sizeof(*mdal))))
				continue;
			mdal->mda = mda;
			dm_list_add(mda_list, &mdal->list);
		}
	}
}

static void _vginfo_attach_info(struct lvmcache_vginfo *vginfo,
				struct lvmcache_info *info)
{
	if (!vginfo)
		return;

	info->vginfo = vginfo;
	dm_list_add(&vginfo->infos, &info->list);
}

static void _vginfo_detach_info(struct lvmcache_info *info)
{
	if (!dm_list_empty(&info->list)) {
		dm_list_del(&info->list);
		dm_list_init(&info->list);
	}

	info->vginfo = NULL;
}

/* If vgid supplied, require a match. */
struct lvmcache_vginfo *lvmcache_vginfo_from_vgname(const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;

	if (!vgname)
		return lvmcache_vginfo_from_vgid(vgid);

	if (!_vgname_hash) {
		log_debug_cache(INTERNAL_ERROR "Internal lvmcache is no yet initialized.");
		return NULL;
	}

	if (!(vginfo = dm_hash_lookup(_vgname_hash, vgname))) {
		log_debug_cache("lvmcache has no info for vgname \"%s\"%s" FMTVGID ".",
				vgname, (vgid) ? " with VGID " : "", (vgid) ? : "");
		return NULL;
	}

	if (vgid)
		do
			if (!strncmp(vgid, vginfo->vgid, ID_LEN))
				return vginfo;
		while ((vginfo = vginfo->next));

	if  (!vginfo)
		log_debug_cache("lvmcache has not found vgname \"%s\"%s" FMTVGID ".",
				vgname, (vgid) ? " with VGID " : "", (vgid) ? : "");

	return vginfo;
}

struct lvmcache_vginfo *lvmcache_vginfo_from_vgid(const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	char id[ID_LEN + 1] __attribute__((aligned(8)));

	if (!_vgid_hash || !vgid) {
		log_debug_cache(INTERNAL_ERROR "Internal cache cannot lookup vgid.");
		return NULL;
	}

	/* vgid not necessarily NULL-terminated */
	(void) dm_strncpy(id, vgid, sizeof(id));

	if (!(vginfo = dm_hash_lookup(_vgid_hash, id))) {
		log_debug_cache("lvmcache has no info for vgid \"%s\"", id);
		return NULL;
	}

	return vginfo;
}

const char *lvmcache_vgname_from_vgid(struct dm_pool *mem, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	const char *vgname = NULL;

	if ((vginfo = lvmcache_vginfo_from_vgid(vgid)))
		vgname = vginfo->vgname;

	if (mem && vgname)
		return dm_pool_strdup(mem, vgname);

	return vgname;
}

const char *lvmcache_vgid_from_vgname(struct cmd_context *cmd, const char *vgname)
{
	struct lvmcache_vginfo *vginfo;

	if (!(vginfo = dm_hash_lookup(_vgname_hash, vgname)))
		return_NULL;

	if (!vginfo->next)
		return dm_pool_strdup(cmd->mem, vginfo->vgid);

	/*
	 * There are multiple VGs with this name to choose from.
	 * Return an error because we don't know which VG is intended.
	 */
	return NULL;
}

/*
 * If valid_only is set, data will only be returned if the cached data is
 * known still to be valid.
 *
 * When the device being worked with is known, pass that dev as the second arg.
 * This ensures that when duplicates exist, the wrong dev isn't used.
 */
struct lvmcache_info *lvmcache_info_from_pvid(const char *pvid, struct device *dev, int valid_only)
{
	struct lvmcache_info *info;
	char id[ID_LEN + 1] __attribute__((aligned(8)));

	if (!_pvid_hash || !pvid)
		return NULL;

	(void) dm_strncpy(id, pvid, sizeof(id));

	if (!(info = dm_hash_lookup(_pvid_hash, id)))
		return NULL;

	/*
	 * When handling duplicate PVs, more than one device can have this pvid.
	 */
	if (dev && info->dev && (info->dev != dev)) {
		log_debug_cache("Ignoring lvmcache info for dev %s because dev %s was requested for PVID %s.",
				dev_name(info->dev), dev_name(dev), id);
		return NULL;
	}

	return info;
}

const struct format_type *lvmcache_fmt_from_info(struct lvmcache_info *info)
{
	return info->fmt;
}

const char *lvmcache_vgname_from_info(struct lvmcache_info *info)
{
	if (info->vginfo)
		return info->vginfo->vgname;
	return NULL;
}

static uint64_t _get_pvsummary_size(char *pvid)
{
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	struct lvmcache_vginfo *vginfo;
	struct pv_list *pvl;

	dm_list_iterate_items(vginfo, &_vginfos) {
		dm_list_iterate_items(pvl, &vginfo->pvsummaries) {
			(void) dm_strncpy(pvid_s, (char *) &pvl->pv->id, sizeof(pvid_s));
			if (!strcmp(pvid_s, pvid))
				return pvl->pv->size;
		}
	}

	return 0;
}

static const char *_get_pvsummary_device_hint(char *pvid)
{
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	struct lvmcache_vginfo *vginfo;
	struct pv_list *pvl;

	dm_list_iterate_items(vginfo, &_vginfos) {
		dm_list_iterate_items(pvl, &vginfo->pvsummaries) {
			(void) dm_strncpy(pvid_s, (char *) &pvl->pv->id, sizeof(pvid_s));
			if (!strcmp(pvid_s, pvid))
				return pvl->pv->device_hint;
		}
	}

	return NULL;
}

/*
 * Check if any PVs in vg->pvs have the same PVID as any
 * entries in _unused_duplicates.
 */

int vg_has_duplicate_pvs(struct volume_group *vg)
{
	struct pv_list *pvl;
	struct device_list *devl;

	dm_list_iterate_items(pvl, &vg->pvs) {
		dm_list_iterate_items(devl, &_unused_duplicates) {
			if (id_equal(&pvl->pv->id, (const struct id *)devl->dev->pvid))
				return 1;
		}
	}
	return 0;
}

bool lvmcache_dev_is_unused_duplicate(struct device *dev)
{
	return dev_in_device_list(dev, &_unused_duplicates) ? true : false;
}

static void _warn_unused_duplicates(struct cmd_context *cmd)
{
	char uuid[64] __attribute__((aligned(8)));
	struct lvmcache_info *info;
	struct device_list *devl;

	dm_list_iterate_items(devl, &_unused_duplicates) {
		if (!id_write_format((const struct id *)devl->dev->pvid, uuid, sizeof(uuid)))
			stack;

		log_warn("WARNING: Not using device %s for PV %s.", dev_name(devl->dev), uuid);
	}

	dm_list_iterate_items(devl, &_unused_duplicates) {
		/* info for the preferred device that we're actually using */
		if (!(info = lvmcache_info_from_pvid(devl->dev->pvid, NULL, 0)))
			continue;

		if (!id_write_format((const struct id *)info->dev->pvid, uuid, sizeof(uuid)))
			stack;

		log_warn("WARNING: PV %s prefers device %s because %s.",
			 uuid, dev_name(info->dev), info->dev->duplicate_prefer_reason);
	}
}

/*
 * If we've found devices with the same PVID, decide which one
 * to use.
 *
 * Compare _initial_duplicates entries with the corresponding
 * dev (matching PVID) in lvmcache.  There may be multiple
 * entries in _initial_duplicates for a given PVID.  If a dev
 * from _initial is preferred over the comparable dev in lvmcache,
 * then drop the comparable dev from lvmcache and rescan the dev
 * from _initial (rescanning adds it to lvmcache.)
 *
 * When a preferred dev is chosen, the dispreferred duplicate for
 * it is kept in _unused_duplicates.
 *
 * For some duplicate entries, like a PV detected on an MD dev and
 * on a component of that MD dev, we simply ignore the component
 * dev, like it was excluded by a filter.  In this case we do not
 * keep the ignored dev on the _unused list.
 *
 * _initial_duplicates: duplicate devs found during label_scan.
 * The first dev with a given PVID is added to lvmcache, and any
 * subsequent devs with that PVID are not added to lvmcache, but
 * are kept in the _initial_duplicates list.  When label_scan is
 * done, the caller (lvmcache_label_scan) compares the dev in
 * lvmcache with the matching entries in _initial_duplicates to
 * decide which dev should be the one used by the command (which
 * will be the one kept in lvmcache.)
 *
 * _unused_duplicates: duplicate devs not chosen to be used.
 * After label_scan adds entries to _initial_duplicates, the
 * _initial entries are processed.  If the current lvmcache dev is
 * preferred over the _initial entry, then the _initial entry is
 * moved to _unused_duplicates.  If the current lvmcache dev
 * is dispreferred vs the _initial duplicate, then the current
 * lvmcache dev is added to _unused, the lvmcache info for it is
 * dropped, the _initial dev is removed, that _initial dev is
 * scanned and added to lvmcache.
 *
 * del_cache_devs: devices to drop from lvmcache
 * add_cache_devs: devices to scan to add to lvmcache
 */

static void _choose_duplicates(struct cmd_context *cmd,
			       struct dm_list *del_cache_devs,
			       struct dm_list *add_cache_devs)
{
	char *pvid;
	const char *reason;
	const char *device_hint;
	struct dm_list altdevs;
	struct dm_list new_unused;
	struct dev_types *dt = cmd->dev_types;
	struct device_list *devl, *devl_safe, *devl_add, *devl_del;
	struct lvmcache_info *info;
	struct device *dev1, *dev2;
	uint32_t dev1_major, dev1_minor, dev2_major, dev2_minor;
	uint64_t dev1_size, dev2_size, pvsummary_size;
	int in_subsys1, in_subsys2;
	int is_dm1, is_dm2;
	int has_fs1, has_fs2;
	int has_lv1, has_lv2;
	int same_size1, same_size2;
	int same_name1 = 0, same_name2 = 0;
	int prev_unchosen1, prev_unchosen2;
	int change;

	dm_list_init(&new_unused);

	/*
	 * Create a list of all alternate devs for the same pvid: altdevs.
	 */
next:
	dm_list_init(&altdevs);
	pvid = NULL;

	dm_list_iterate_items_safe(devl, devl_safe, &_initial_duplicates) {
		if (!pvid) {
			dm_list_move(&altdevs, &devl->list);
			pvid = devl->dev->pvid;
		} else {
			if (!strcmp(pvid, devl->dev->pvid))
				dm_list_move(&altdevs, &devl->list);
		}
	}

	/* done, no more entries to process */
	if (!pvid) {
		_destroy_device_list(&_unused_duplicates);
		dm_list_splice(&_unused_duplicates, &new_unused);
		return;
	}

	/*
	 * Get rid of any md components before comparing alternatives.
	 * (Since an md component can never be used, it's not an
	 * option to use like other kinds of alternatives.)
	 */

	info = lvmcache_info_from_pvid(pvid, NULL, 0);
	if (info && dev_is_md_component(info->dev, NULL, 1)) {
		/* does not go in del_cache_devs which become unused_duplicates */
		log_debug_cache("PV %s drop MD component from scan selection %s", pvid, dev_name(info->dev));
		lvmcache_del(info);
		info = NULL;
	}

	dm_list_iterate_items_safe(devl, devl_safe, &altdevs) {
		if (dev_is_md_component(devl->dev, NULL, 1)) {
			log_debug_cache("PV %s drop MD component from scan duplicates %s", pvid, dev_name(devl->dev));
			dm_list_del(&devl->list);
		}
	}

	if (dm_list_empty(&altdevs))
		goto next;


	/*
	 * Find the device for the pvid that's currently in lvmcache.
	 */

	if (!(info = lvmcache_info_from_pvid(pvid, NULL, 0))) {
		/*
		 * This will happen if the lvmcache dev was already recognized
		 * as an md component and already dropped from lvmcache.
		 * One of the altdev entries for the PVID should be added to
		 * lvmcache.
		 */
		if (dm_list_size(&altdevs) == 1) {
			devl = dm_list_item(dm_list_first(&altdevs), struct device_list);
			dm_list_del(&devl->list);
			dm_list_add(add_cache_devs, &devl->list);

			log_debug_cache("PV %s with duplicates unselected using %s.",
					pvid, dev_name(devl->dev));
			goto next;
		} else {
			devl = dm_list_item(dm_list_first(&altdevs), struct device_list);
			dev1 = devl->dev;

			log_debug_cache("PV %s with duplicates unselected comparing alternatives", pvid);
		}
	} else {
		log_debug_cache("PV %s with duplicates comparing alternatives for %s",
				pvid, dev_name(info->dev));
		dev1 = info->dev;
	}

	/*
	 * Compare devices for the given pvid to find one that's preferred.
	 */

	dm_list_iterate_items(devl, &altdevs) {
		dev2 = devl->dev;

		/* Took the first altdev to start with above. */
		if (dev1 == dev2)
			continue;

		prev_unchosen1 = dev_in_device_list(dev1, &_unused_duplicates);
		prev_unchosen2 = dev_in_device_list(dev2, &_unused_duplicates);

		if (!prev_unchosen1 && !prev_unchosen2) {
			/*
			 * The prev list saves the unchosen preference across
			 * lvmcache_destroy.  Sometimes a single command will
			 * fill lvmcache, destroy it, and refill it, and we
			 * want the same duplicate preference to be preserved
			 * in each instance of lvmcache for a single command.
			 */
			prev_unchosen1 = dev_in_device_list(dev1, &_prev_unused_duplicate_devs);
			prev_unchosen2 = dev_in_device_list(dev2, &_prev_unused_duplicate_devs);
		}

		dev1_major = MAJOR(dev1->dev);
		dev1_minor = MINOR(dev1->dev);
		dev2_major = MAJOR(dev2->dev);
		dev2_minor = MINOR(dev2->dev);

		if (!dev_get_size(dev1, &dev1_size))
			dev1_size = 0;
		if (!dev_get_size(dev2, &dev2_size))
			dev2_size = 0;

		pvsummary_size = _get_pvsummary_size(devl->dev->pvid);
		same_size1 = (dev1_size == pvsummary_size);
		same_size2 = (dev2_size == pvsummary_size);

		if ((device_hint = _get_pvsummary_device_hint(devl->dev->pvid))) {
			same_name1 = !strcmp(device_hint, dev_name(dev1));
			same_name2 = !strcmp(device_hint, dev_name(dev2));
		}

		has_lv1 = (dev1->flags & DEV_USED_FOR_LV) ? 1 : 0;
		has_lv2 = (dev2->flags & DEV_USED_FOR_LV) ? 1 : 0;

		in_subsys1 = dev_subsystem_part_major(dt, dev1);
		in_subsys2 = dev_subsystem_part_major(dt, dev2);

		is_dm1 = dm_is_dm_major(dev1_major);
		is_dm2 = dm_is_dm_major(dev2_major);

		has_fs1 = dm_device_has_mounted_fs(dev1_major, dev1_minor);
		has_fs2 = dm_device_has_mounted_fs(dev2_major, dev2_minor);

		log_debug_cache("PV %s compare duplicates: %s %u:%u. %s %u:%u. device_hint %s.",
				devl->dev->pvid,
				dev_name(dev1), dev1_major, dev1_minor,
				dev_name(dev2), dev2_major, dev2_minor,
				device_hint ?: "none");

		log_debug_cache("PV %s: size %llu. %s is %llu. %s is %llu.",
				devl->dev->pvid,
				(unsigned long long)pvsummary_size,
				dev_name(dev1), (unsigned long long)dev1_size,
				dev_name(dev2), (unsigned long long)dev2_size);

		log_debug_cache("PV %s: %s was prev %s. %s was prev %s.",
				devl->dev->pvid,
				dev_name(dev1), prev_unchosen1 ? "not chosen" : "<none>",
				dev_name(dev2), prev_unchosen2 ? "not chosen" : "<none>");

		log_debug_cache("PV %s: %s %s subsystem. %s %s subsystem.",
				devl->dev->pvid,
				dev_name(dev1), in_subsys1 ? "is in" : "is not in",
				dev_name(dev2), in_subsys2 ? "is in" : "is not in");

		log_debug_cache("PV %s: %s %s dm. %s %s dm.",
				devl->dev->pvid,
				dev_name(dev1), is_dm1 ? "is" : "is not",
				dev_name(dev2), is_dm2 ? "is" : "is not");

		log_debug_cache("PV %s: %s %s mounted fs. %s %s mounted fs.",
				devl->dev->pvid,
				dev_name(dev1), has_fs1 ? "has" : "has no",
				dev_name(dev2), has_fs2 ? "has" : "has no");

		log_debug_cache("PV %s: %s %s LV. %s %s LV.",
				devl->dev->pvid,
				dev_name(dev1), has_lv1 ? "is used for" : "is not used for",
				dev_name(dev2), has_lv2 ? "is used for" : "is not used for");

		change = 0;

		if (prev_unchosen1 && !prev_unchosen2) {
			/* change to 2 (NB when unchosen is set we unprefer) */
			change = 1;
			reason = "of previous preference";
		} else if (prev_unchosen2 && !prev_unchosen1) {
			/* keep 1 (NB when unchosen is set we unprefer) */
			reason = "of previous preference";
		} else if (has_lv1 && !has_lv2) {
			/* keep 1 */
			reason = "device is used by LV";
		} else if (has_lv2 && !has_lv1) {
			/* change to 2 */
			change = 1;
			reason = "device is used by LV";
		} else if (same_size1 && !same_size2) {
			/* keep 1 */
			reason = "device size is correct";
		} else if (same_size2 && !same_size1) {
			/* change to 2 */
			change = 1;
			reason = "device size is correct";
		} else if (same_name1 && !same_name2) {
			/* keep 1 */
			reason = "device name matches previous";
		} else if (same_name2 && !same_name1) {
			/* change to 2 */
			change = 1;
			reason = "device name matches previous";
		} else if (has_fs1 && !has_fs2) {
			/* keep 1 */
			reason = "device has fs mounted";
		} else if (has_fs2 && !has_fs1) {
			/* change to 2 */
			change = 1;
			reason = "device has fs mounted";
		} else if (is_dm1 && !is_dm2) {
			/* keep 1 */
			reason = "device is in dm subsystem";
		} else if (is_dm2 && !is_dm1) {
			/* change to 2 */
			change = 1;
			reason = "device is in dm subsystem";
		} else if (in_subsys1 && !in_subsys2) {
			/* keep 1 */
			reason = "device is in subsystem";
		} else if (in_subsys2 && !in_subsys1) {
			/* change to 2 */
			change = 1;
			reason = "device is in subsystem";
		} else {
			reason = "device was seen first";
		}

		if (change)
			dev1 = dev2;

		dev1->duplicate_prefer_reason = reason;
	}

	/*
	 * At the end of the loop, dev1 is the device we prefer to
	 * use.  If there's no info struct, it means there's no dev
	 * currently in lvmcache for this PVID, so just add the
	 * preferred one (dev1).  If dev1 is different from the dev
	 * currently in lvmcache, then drop the dev in lvmcache and
	 * add dev1 to lvmcache.  If dev1 is the same as the dev
	 * in lvmcache already, then no changes are needed and the
	 * altdevs all become unused duplicates.
	 */

	if (!info) {
		log_debug_cache("PV %s with duplicates will use %s.", pvid, dev_name(dev1));

		if (!(devl_add = _get_devl_in_device_list(dev1, &altdevs))) {
			/* shouldn't happen */
			log_error(INTERNAL_ERROR "PV %s with duplicates no alternate list entry for %s", pvid, dev_name(dev1));
			dm_list_splice(&new_unused, &altdevs);
			goto next;
		}

		dm_list_move(add_cache_devs, &devl_add->list);

	} else if (dev1 != info->dev) {
		log_debug_cache("PV %s with duplicates will change from %s to %s.",
				pvid, dev_name(info->dev), dev_name(dev1));

		/*
		 * Move the preferred device (dev1) from altdevs
		 * to add_cache_devs.  Create a del_cache_devs entry
		 * for the current lvmcache device to drop.
		 */

		if (!(devl_add = _get_devl_in_device_list(dev1, &altdevs))) {
			/* shouldn't happen */
			log_error(INTERNAL_ERROR "PV %s with duplicates no alternate list entry for %s", pvid, dev_name(dev1));
			dm_list_splice(&new_unused, &altdevs);
			goto next;
		}

		dm_list_move(add_cache_devs, &devl_add->list);

		if ((devl_del = zalloc(sizeof(*devl_del)))) {
			devl_del->dev = info->dev;
			dm_list_add(del_cache_devs, &devl_del->list);
		}

	} else {
		/*
		 * Keeping existing dev in lvmcache for this PVID.
		 */
		log_debug_cache("PV %s with duplicates will continue using %s.",
				pvid, dev_name(info->dev));
	}

	/*
	 * Any altdevs entries not chosen are moved to _unused_duplicates.
	 * del_cache_devs being dropped are moved to _unused_duplicates
	 * after being dropped.  So, _unused_duplicates represents all
	 * duplicates not being used in lvmcache.
	 */

	dm_list_splice(&new_unused, &altdevs);
	goto next;
}

/*
 * The initial label_scan at the start of the command is done without
 * holding VG locks.  Then for each VG identified during the label_scan,
 * vg_read(vgname) is called while holding the VG lock.  The labels
 * and metadata on this VG's devices could have changed between the
 * initial unlocked label_scan and the current vg_read().  So, we reread
 * the labels/metadata for each device in the VG now that we hold the
 * lock, and use this for processing the VG.
 *
 * A label scan is ultimately creating associations between devices
 * and VGs so that when vg_read wants to get VG metadata, it knows
 * which devices to read.
 *
 * It's possible that a VG is being modified during the first label
 * scan, causing the scan to see inconsistent metadata on different
 * devs in the VG.  It's possible that those modifications are
 * adding/removing devs from the VG, in which case the device/VG
 * associations in lvmcache after the scan are not correct.
 * NB. It's even possible the VG was removed completely between
 * label scan and here, in which case we'd not find the VG in
 * lvmcache after this rescan.
 *
 * A scan will also create in incorrect/incomplete picture of a VG
 * when devices have no metadata areas.  The scan does not use
 * VG metadata to figure out that a dev with no metadata belongs
 * to a particular VG, so a device with no mdas will not be linked
 * to that VG after a scan.
 */

static int _label_rescan_vg(struct cmd_context *cmd, const char *vgname, const char *vgid, int rw)
{
	struct dm_list devs;
	struct device_list *devl, *devl2;
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	dm_list_init(&devs);

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid)))
		return_0;

	dm_list_iterate_items(info, &vginfo->infos) {
		if (!(devl = malloc(sizeof(*devl)))) {
			log_error("device_list element allocation failed");
			return 0;
		}
		devl->dev = info->dev;
		dm_list_add(&devs, &devl->list);
	}

	/* Delete info for each dev, deleting the last info will delete vginfo. */
	dm_list_iterate_items(devl, &devs)
		lvmcache_del_dev(devl->dev);

	/* Dropping the last info struct is supposed to drop vginfo. */
	if ((vginfo = lvmcache_vginfo_from_vgname(vgname, vgid)))
		log_warn("VG info not dropped before rescan of %s", vgname);

	if (rw)
		label_scan_devs_rw(cmd, cmd->filter, &devs);
	else
		label_scan_devs(cmd, cmd->filter, &devs);

	dm_list_iterate_items_safe(devl, devl2, &devs) {
		dm_list_del(&devl->list);
		free(devl);
	}

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		log_warn("VG info not found after rescan of %s", vgname);
		return 0;
	}

	return 1;
}

int lvmcache_label_rescan_vg(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	return _label_rescan_vg(cmd, vgname, vgid, 0);
}

int lvmcache_label_rescan_vg_rw(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	return _label_rescan_vg(cmd, vgname, vgid, 1);
}

/*
 * Uses label_scan to populate lvmcache with 'vginfo' struct for each VG
 * and associated 'info' structs for those VGs.  Only VG summary information
 * is used to assemble the vginfo/info during the scan, so the resulting
 * representation of VG/PV state is incomplete and even incorrect.
 * Specifically, PVs with no MDAs are considered orphans and placed in the
 * orphan vginfo by lvmcache_label_scan.  This is corrected during the
 * processing phase as each vg_read() uses VG metadata for each VG to correct
 * the lvmcache state, i.e. it moves no-MDA PVs from the orphan vginfo onto
 * the correct vginfo.  Once vg_read() is finished for all VGs, all of the
 * incorrectly placed PVs should have been moved from the orphan vginfo
 * onto their correct vginfo's, and the orphan vginfo should (in theory)
 * represent only real orphan PVs.  (Note: if lvmcache_label_scan is run
 * after vg_read udpates to lvmcache state, then the lvmcache will be
 * incorrect again, so do not run lvmcache_label_scan during the
 * processing phase.)
 *
 * TODO: in this label scan phase, don't stash no-MDA PVs into the
 * orphan VG.  We know that's a fiction, and it can have harmful/damaging
 * results.  Instead, put them into a temporary list where they can be
 * pulled from later when vg_read uses metadata to resolve which VG
 * they actually belong to.
 */

int lvmcache_label_scan(struct cmd_context *cmd)
{
	struct dm_list del_cache_devs;
	struct dm_list add_cache_devs;
	struct lvmcache_info *info;
	struct lvmcache_vginfo *vginfo;
	struct device_list *devl;
	int vginfo_count = 0;

	int r = 0;

	log_debug_cache("Finding VG info");

	/* FIXME: can this happen? */
	if (!cmd->filter) {
		log_error("label scan is missing filter");
		goto out;
	}

	if (!refresh_filters(cmd))
		log_error("Scan failed to refresh device filter.");

	/*
	 * Duplicates found during this label scan are added to _initial_duplicates.
	 */
	_destroy_device_list(&_initial_duplicates);
	_destroy_device_list(&_unused_duplicates);

	/*
	 * Do the actual scanning.  This populates lvmcache
	 * with infos/vginfos based on reading headers from
	 * each device, and a vg summary from each mda.
	 *
	 * Note that this will *skip* scanning a device if
	 * an info struct already exists in lvmcache for
	 * the device.
	 */
	label_scan(cmd);

	/*
	 * _choose_duplicates() returns:
	 *
	 * . del_cache_devs: a list of devs currently in lvmcache that should
	 * be removed from lvmcache because they will be replaced with
	 * alternative devs for the same PV.
	 *
	 * . add_cache_devs: a list of devs that are preferred over devs in
	 * lvmcache for the same PV.  These devices should be rescanned to
	 * populate lvmcache from them.
	 *
	 * First remove lvmcache info for the devs to be dropped, then rescan
	 * the devs that are preferred to add them to lvmcache.
	 *
	 * Keep a complete list of all devs that are unused by moving the
	 * del_cache_devs onto _unused_duplicates.
	 */

	if (!dm_list_empty(&_initial_duplicates)) {
		dm_list_init(&del_cache_devs);
		dm_list_init(&add_cache_devs);

		log_debug_cache("Resolving duplicate devices");

		_choose_duplicates(cmd, &del_cache_devs, &add_cache_devs);

		dm_list_iterate_items(devl, &del_cache_devs) {
			log_debug_cache("Dropping unchosen duplicate %s", dev_name(devl->dev));
			if ((info = lvmcache_info_from_pvid(devl->dev->pvid, NULL, 0)))
				lvmcache_del(info);
		}

		dm_list_iterate_items(devl, &add_cache_devs) {
			log_debug_cache("Adding chosen duplicate %s", dev_name(devl->dev));
			label_read(devl->dev);
		}

		dm_list_splice(&_unused_duplicates, &del_cache_devs);

		/* Warn about unused duplicates that the user might want to resolve. */
		_warn_unused_duplicates(cmd);
	}

	r = 1;

      out:
	dm_list_iterate_items(vginfo, &_vginfos) {
		if (is_orphan_vg(vginfo->vgname))
			continue;
		vginfo_count++;
	}

	log_debug_cache("Found VG info for %d VGs", vginfo_count);

	return r;
}

int lvmcache_get_vgnameids(struct cmd_context *cmd,
			   struct dm_list *vgnameids,
			   const char *only_this_vgname,
			   int include_internal)
{
	struct vgnameid_list *vgnl;
	struct lvmcache_vginfo *vginfo;

	if (only_this_vgname) {
		if (!(vgnl = dm_pool_alloc(cmd->mem, sizeof(*vgnl)))) {
			log_error("vgnameid_list allocation failed.");
			return 0;
		}

		vgnl->vg_name = dm_pool_strdup(cmd->mem, only_this_vgname);
		vgnl->vgid = NULL;
		dm_list_add(vgnameids, &vgnl->list);
		return 1;
	}

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (!include_internal && is_orphan_vg(vginfo->vgname))
			continue;

		if (!(vgnl = dm_pool_alloc(cmd->mem, sizeof(*vgnl)))) {
			log_error("vgnameid_list allocation failed.");
			return 0;
		}

		vgnl->vgid = dm_pool_strdup(cmd->mem, vginfo->vgid);
		vgnl->vg_name = dm_pool_strdup(cmd->mem, vginfo->vgname);

		if (!vgnl->vgid || !vgnl->vg_name) {
			log_error("vgnameid_list member allocation failed.");
			return 0;
		}

		dm_list_add(vgnameids, &vgnl->list);
	}

	return 1;
}

static struct device *_device_from_pvid(const struct id *pvid, uint64_t *label_sector)
{
	struct lvmcache_info *info;

	if ((info = lvmcache_info_from_pvid((const char *) pvid, NULL, 0))) {
		if (info->label && label_sector)
			*label_sector = info->label->sector;
		return info->dev;
	}

	return NULL;
}

struct device *lvmcache_device_from_pvid(struct cmd_context *cmd, const struct id *pvid, uint64_t *label_sector)
{
	struct device *dev;

	dev = _device_from_pvid(pvid, label_sector);
	if (dev)
		return dev;

	log_debug_devs("No device with uuid %s.", (const char *)pvid);
	return NULL;
}

int lvmcache_pvid_in_unused_duplicates(const char *pvid)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, &_unused_duplicates) {
		if (!strncmp(devl->dev->pvid, pvid, ID_LEN))
			return 1;
	}
	return 0;
}

static int _free_vginfo(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_vginfo *primary_vginfo, *vginfo2;
	int r = 1;

	vginfo2 = primary_vginfo = lvmcache_vginfo_from_vgname(vginfo->vgname, NULL);

	if (vginfo == primary_vginfo) {
		dm_hash_remove(_vgname_hash, vginfo->vgname);
		if (vginfo->next && !dm_hash_insert(_vgname_hash, vginfo->vgname,
						    vginfo->next)) {
			log_error("_vgname_hash re-insertion for %s failed",
				  vginfo->vgname);
			r = 0;
		}
	} else
		while (vginfo2) {
			if (vginfo2->next == vginfo) {
				vginfo2->next = vginfo->next;
				break;
			}
			vginfo2 = vginfo2->next;
		}

	free(vginfo->system_id);
	free(vginfo->vgname);
	free(vginfo->creation_host);

	if (*vginfo->vgid && _vgid_hash &&
	    lvmcache_vginfo_from_vgid(vginfo->vgid) == vginfo)
		dm_hash_remove(_vgid_hash, vginfo->vgid);

	dm_list_del(&vginfo->list);

	free(vginfo);

	return r;
}

/*
 * vginfo must be info->vginfo unless info is NULL
 */
static int _drop_vginfo(struct lvmcache_info *info, struct lvmcache_vginfo *vginfo)
{
	if (info)
		_vginfo_detach_info(info);

	/* vginfo still referenced? */
	if (!vginfo || is_orphan_vg(vginfo->vgname) ||
	    !dm_list_empty(&vginfo->infos))
		return 1;

	if (!_free_vginfo(vginfo))
		return_0;

	return 1;
}

void lvmcache_del(struct lvmcache_info *info)
{
	if (info->dev->pvid[0] && _pvid_hash)
		dm_hash_remove(_pvid_hash, info->dev->pvid);

	_drop_vginfo(info, info->vginfo);

	info->label->labeller->ops->destroy_label(info->label->labeller,
						  info->label);
	label_destroy(info->label);
	free(info);
}

void lvmcache_del_dev(struct device *dev)
{
	struct lvmcache_info *info;

	if ((info = lvmcache_info_from_pvid((const char *)dev->pvid, dev, 0)))
		lvmcache_del(info);
}

/*
 * vginfo must be info->vginfo unless info is NULL (orphans)
 */
static int _lvmcache_update_vgid(struct lvmcache_info *info,
				 struct lvmcache_vginfo *vginfo,
				 const char *vgid)
{
	if (!vgid || !vginfo ||
	    !strncmp(vginfo->vgid, vgid, ID_LEN))
		return 1;

	if (vginfo && *vginfo->vgid)
		dm_hash_remove(_vgid_hash, vginfo->vgid);
	if (!vgid) {
		/* FIXME: unreachable code path */
		log_debug_cache("lvmcache: %s: clearing VGID", info ? dev_name(info->dev) : vginfo->vgname);
		return 1;
	}

	(void) dm_strncpy(vginfo->vgid, vgid, sizeof(vginfo->vgid));
	if (!dm_hash_insert(_vgid_hash, vginfo->vgid, vginfo)) {
		log_error("_lvmcache_update: vgid hash insertion failed: %s",
			  vginfo->vgid);
		return 0;
	}

	if (!is_orphan_vg(vginfo->vgname))
		log_debug_cache("lvmcache %s: VG %s: set VGID to " FMTVGID ".",
				(info) ? dev_name(info->dev) : "",
				vginfo->vgname, vginfo->vgid);

	return 1;
}

static int _insert_vginfo(struct lvmcache_vginfo *new_vginfo, const char *vgid,
			  uint32_t vgstatus, const char *creation_host,
			  struct lvmcache_vginfo *primary_vginfo)
{
	struct lvmcache_vginfo *last_vginfo = primary_vginfo;
	char uuid_primary[64] __attribute__((aligned(8)));
	char uuid_new[64] __attribute__((aligned(8)));
	int use_new = 0;

	/* Pre-existing VG takes precedence. Unexported VG takes precedence. */
	if (primary_vginfo) {
		if (!id_write_format((const struct id *)vgid, uuid_new, sizeof(uuid_new)))
			return_0;

		if (!id_write_format((const struct id *)&primary_vginfo->vgid, uuid_primary,
				     sizeof(uuid_primary)))
			return_0;

		_found_duplicate_vgnames = 1;

		/*
		 * vginfo is kept for each VG with the same name.
		 * They are saved with the vginfo->next list.
		 * These checks just decide the ordering of
		 * that list.
		 *
		 * FIXME: it should no longer matter what order
		 * the vginfo's are kept in, so we can probably
		 * remove these comparisons and reordering entirely.
		 *
		 * If   Primary not exported, new exported => keep
		 * Else Primary exported, new not exported => change
		 * Else Primary has hostname for this machine => keep
		 * Else Primary has no hostname, new has one => change
		 * Else New has hostname for this machine => change
		 * Else Keep primary.
		 */
		if (!(primary_vginfo->status & EXPORTED_VG) &&
		    (vgstatus & EXPORTED_VG))
			log_verbose("Cache: Duplicate VG name %s: "
				    "Existing %s takes precedence over "
				    "exported %s", new_vginfo->vgname,
				    uuid_primary, uuid_new);
		else if ((primary_vginfo->status & EXPORTED_VG) &&
			   !(vgstatus & EXPORTED_VG)) {
			log_verbose("Cache: Duplicate VG name %s: "
				    "%s takes precedence over exported %s",
				    new_vginfo->vgname, uuid_new,
				    uuid_primary);
			use_new = 1;
		} else if (primary_vginfo->creation_host &&
			   !strcmp(primary_vginfo->creation_host,
				   primary_vginfo->fmt->cmd->hostname))
			log_verbose("Cache: Duplicate VG name %s: "
				    "Existing %s (created here) takes precedence "
				    "over %s", new_vginfo->vgname, uuid_primary,
				    uuid_new);
		else if (!primary_vginfo->creation_host && creation_host) {
			log_verbose("Cache: Duplicate VG name %s: "
				    "%s (with creation_host) takes precedence over %s",
				    new_vginfo->vgname, uuid_new,
				    uuid_primary);
			use_new = 1;
		} else if (creation_host &&
			   !strcmp(creation_host,
				   primary_vginfo->fmt->cmd->hostname)) {
			log_verbose("Cache: Duplicate VG name %s: "
				    "%s (created here) takes precedence over %s",
				    new_vginfo->vgname, uuid_new,
				    uuid_primary);
			use_new = 1;
		} else {
			log_verbose("Cache: Duplicate VG name %s: "
				    "Prefer existing %s vs new %s",
				    new_vginfo->vgname, uuid_primary, uuid_new);
		}

		if (!use_new) {
			while (last_vginfo->next)
				last_vginfo = last_vginfo->next;
			last_vginfo->next = new_vginfo;
			return 1;
		}

		dm_hash_remove(_vgname_hash, primary_vginfo->vgname);
	}

	if (!dm_hash_insert(_vgname_hash, new_vginfo->vgname, new_vginfo)) {
		log_error("cache_update: vg hash insertion failed: %s",
		  	new_vginfo->vgname);
		return 0;
	}

	if (primary_vginfo)
		new_vginfo->next = primary_vginfo;

	return 1;
}

static int _lvmcache_update_vgname(struct lvmcache_info *info,
				   const char *vgname, const char *vgid,
				   uint32_t vgstatus, const char *creation_host,
				   const struct format_type *fmt)
{
	struct lvmcache_vginfo *vginfo, *primary_vginfo;
	char mdabuf[32];

	if (!vgname || (info && info->vginfo && !strcmp(info->vginfo->vgname, vgname)))
		return 1;

	/* Remove existing vginfo entry */
	if (info)
		_drop_vginfo(info, info->vginfo);

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		/*
	 	 * Create a vginfo struct for this VG and put the vginfo
	 	 * into the hash table.
	 	 */

		if (!(vginfo = zalloc(sizeof(*vginfo)))) {
			log_error("lvmcache_update_vgname: list alloc failed");
			return 0;
		}
		if (!(vginfo->vgname = strdup(vgname))) {
			free(vginfo);
			log_error("cache vgname alloc failed for %s", vgname);
			return 0;
		}
		dm_list_init(&vginfo->infos);
		dm_list_init(&vginfo->outdated_infos);
		dm_list_init(&vginfo->pvsummaries);

		/*
		 * A different VG (different uuid) can exist with the same name.
		 * In this case, the two VGs will have separate vginfo structs,
		 * but the second will be linked onto the existing vginfo->next,
		 * not in the hash.
		 */
		primary_vginfo = lvmcache_vginfo_from_vgname(vgname, NULL);

		if (!_insert_vginfo(vginfo, vgid, vgstatus, creation_host, primary_vginfo)) {
			free(vginfo->vgname);
			free(vginfo);
			return 0;
		}

		/* Ensure orphans appear last on list_iterate */
		if (is_orphan_vg(vgname))
			dm_list_add(&_vginfos, &vginfo->list);
		else
			dm_list_add_h(&_vginfos, &vginfo->list);
	}

	if (info)
		_vginfo_attach_info(vginfo, info);
	else if (!_lvmcache_update_vgid(NULL, vginfo, vgid)) /* Orphans */
		return_0;

	/* FIXME Check consistency of list! */
	vginfo->fmt = fmt;

	if (info) {
		if (info->mdas.n)
			sprintf(mdabuf, " with %u mda(s)", dm_list_size(&info->mdas));
		else
			mdabuf[0] = '\0';
		log_debug_cache("lvmcache %s: now in VG %s%s%s%s%s.",
				dev_name(info->dev),
				vgname, vginfo->vgid[0] ? " (" : "",
				vginfo->vgid[0] ? vginfo->vgid : "",
				vginfo->vgid[0] ? ")" : "", mdabuf);
	} else
		log_debug_cache("lvmcache: Initialised VG %s.", vgname);

	return 1;
}

static int _lvmcache_update_vgstatus(struct lvmcache_info *info, uint32_t vgstatus,
				     const char *creation_host, const char *lock_type,
				     const char *system_id)
{
	if (!info || !info->vginfo)
		return 1;

	if ((info->vginfo->status & EXPORTED_VG) != (vgstatus & EXPORTED_VG))
		log_debug_cache("lvmcache %s: VG %s %s exported.",
				dev_name(info->dev), info->vginfo->vgname,
				vgstatus & EXPORTED_VG ? "now" : "no longer");

	info->vginfo->status = vgstatus;

	if (!creation_host)
		goto set_lock_type;

	if (info->vginfo->creation_host && !strcmp(creation_host,
						   info->vginfo->creation_host))
		goto set_lock_type;

	free(info->vginfo->creation_host);

	if (!(info->vginfo->creation_host = strdup(creation_host))) {
		log_error("cache creation host alloc failed for %s.",
			  creation_host);
		return 0;
	}

	log_debug_cache("lvmcache %s: VG %s: set creation host to %s.",
			dev_name(info->dev), info->vginfo->vgname, creation_host);

set_lock_type:

	if (!lock_type)
		goto set_system_id;

	if (info->vginfo->lock_type && !strcmp(lock_type, info->vginfo->lock_type))
		goto set_system_id;

	free(info->vginfo->lock_type);

	if (!(info->vginfo->lock_type = strdup(lock_type))) {
		log_error("cache lock_type alloc failed for %s", lock_type);
		return 0;
	}

	log_debug_cache("lvmcache %s: VG %s: set lock_type to %s.",
			dev_name(info->dev), info->vginfo->vgname, lock_type);

set_system_id:

	if (!system_id)
		goto out;

	if (info->vginfo->system_id && !strcmp(system_id, info->vginfo->system_id))
		goto out;

	free(info->vginfo->system_id);

	if (!(info->vginfo->system_id = strdup(system_id))) {
		log_error("cache system_id alloc failed for %s", system_id);
		return 0;
	}

	log_debug_cache("lvmcache %s: VG %s: set system_id to %s.",
			dev_name(info->dev), info->vginfo->vgname, system_id);

out:
	return 1;
}

int lvmcache_add_orphan_vginfo(const char *vgname, struct format_type *fmt)
{
	return _lvmcache_update_vgname(NULL, vgname, vgname, 0, "", fmt);
}

static void _lvmcache_update_pvsummaries(struct lvmcache_vginfo *vginfo, struct lvmcache_vgsummary *vgsummary)
{
	struct pv_list *pvl, *safe;

	dm_list_init(&vginfo->pvsummaries);

	dm_list_iterate_items_safe(pvl, safe, &vgsummary->pvsummaries) {
		dm_list_del(&pvl->list);
		dm_list_add(&vginfo->pvsummaries, &pvl->list);
	}
}

/*
 * Returning 0 causes the caller to remove the info struct for this
 * device from lvmcache, which will make it look like a missing device.
 */
int lvmcache_update_vgname_and_id(struct lvmcache_info *info, struct lvmcache_vgsummary *vgsummary)
{
	const char *vgname = vgsummary->vgname;
	const char *vgid = (char *)&vgsummary->vgid;
	struct lvmcache_vginfo *vginfo;

	if (!vgname && !info->vginfo) {
		log_error(INTERNAL_ERROR "NULL vgname handed to cache");
		/* FIXME Remove this */
		vgname = info->fmt->orphan_vg_name;
		vgid = vgname;
	}

	/* If PV without mdas is already in a real VG, don't make it orphan */
	if (is_orphan_vg(vgname) && info->vginfo &&
	    mdas_empty_or_ignored(&info->mdas) &&
	    !is_orphan_vg(info->vginfo->vgname) && critical_section())
		return 1;

	/*
	 * Creates a new vginfo struct for this vgname/vgid if none exists,
	 * and attaches the info struct for the dev to the vginfo.
	 * Puts the vginfo into the vgname hash table.
	 */
	if (!_lvmcache_update_vgname(info, vgname, vgid, vgsummary->vgstatus, vgsummary->creation_host, info->fmt)) {
		/* shouldn't happen, internal error */
		log_error("Failed to update VG %s info in lvmcache.", vgname);
		return 0;
	}

	/*
	 * Puts the vginfo into the vgid hash table.
	 */
	if (!_lvmcache_update_vgid(info, info->vginfo, vgid)) {
		/* shouldn't happen, internal error */
		log_error("Failed to update VG %s info in lvmcache.", vgname);
		return 0;
	}

	/*
	 * FIXME: identify which case this is and why this is needed, then
	 * change that so it doesn't use this function and we can remove
	 * this special case.
	 * (I think this distinguishes the scan path, where these things
	 * are set from the vg_read path where lvmcache_update_vg() is
	 * called which calls this function without seqno/mda_size/mda_checksum.)
	 */
	if (!vgsummary->seqno && !vgsummary->mda_size && !vgsummary->mda_checksum)
		return 1;

	/*
	 * Keep track of which devs/mdas have old versions of the metadata.
	 * The values we keep in vginfo are from the metadata with the largest
	 * seqno.  One dev may have more recent metadata than another dev, and
	 * one mda may have more recent metadata than the other mda on the same
	 * device.
	 *
	 * When a device holds old metadata, the info struct for the device
	 * remains in lvmcache, so the device is not treated as missing.
	 * Also the mda struct containing the old metadata is kept on
	 * info->mdas.  This means that vg_read will read metadata from
	 * the mda again (and probably see the same old metadata).  It
	 * also means that vg_write will use the mda to write new metadata
	 * into the mda that currently has the old metadata.
	 */
	if (vgsummary->mda_num == 1)
		info->mda1_seqno = vgsummary->seqno;
	else if (vgsummary->mda_num == 2)
		info->mda2_seqno = vgsummary->seqno;

	if (!info->summary_seqno)
		info->summary_seqno = vgsummary->seqno;
	else {
		if (info->summary_seqno == vgsummary->seqno) {
			/* This mda has the same metadata as the prev mda on this dev. */
			return 1;

		} else if (info->summary_seqno > vgsummary->seqno) {
			/* This mda has older metadata than the prev mda on this dev. */
			info->summary_seqno_mismatch = true;

		} else if (info->summary_seqno < vgsummary->seqno) {
			/* This mda has newer metadata than the prev mda on this dev. */
			info->summary_seqno_mismatch = true;
			info->summary_seqno = vgsummary->seqno;
		}
	}

	/* this shouldn't happen */
	if (!(vginfo = info->vginfo))
		return 1;

	if (!vginfo->seqno) {
		vginfo->seqno = vgsummary->seqno;
		vginfo->mda_checksum = vgsummary->mda_checksum;
		vginfo->mda_size = vgsummary->mda_size;

		log_debug_cache("lvmcache %s mda%d VG %s set seqno %u checksum %x mda_size %zu",
				dev_name(info->dev), vgsummary->mda_num, vgname,
				vgsummary->seqno, vgsummary->mda_checksum, vgsummary->mda_size);
		goto update_vginfo;

	} else if (vgsummary->seqno < vginfo->seqno) {
		vginfo->scan_summary_mismatch = true;

		log_debug_cache("lvmcache %s mda%d VG %s older seqno %u checksum %x mda_size %zu",
				dev_name(info->dev), vgsummary->mda_num, vgname,
				vgsummary->seqno, vgsummary->mda_checksum, vgsummary->mda_size);
		return 1;

	} else if (vgsummary->seqno > vginfo->seqno) {
		vginfo->scan_summary_mismatch = true;

		/* Replace vginfo values with values from newer metadata. */
		vginfo->seqno = vgsummary->seqno;
		vginfo->mda_checksum = vgsummary->mda_checksum;
		vginfo->mda_size = vgsummary->mda_size;

		log_debug_cache("lvmcache %s mda%d VG %s newer seqno %u checksum %x mda_size %zu",
				dev_name(info->dev), vgsummary->mda_num, vgname,
				vgsummary->seqno, vgsummary->mda_checksum, vgsummary->mda_size);

		goto update_vginfo;
	} else {
		/*
		 * Same seqno as previous metadata we saw for this VG.
		 * If the metadata somehow has a different checksum or size,
		 * even though it has the same seqno, something has gone wrong.
		 * FIXME: test this case: VG has two PVs, first goes missing,
		 * second updated to seqno 4, first comes back and second goes
		 * missing, first updated to seqno 4, second comes back, now
		 * both are present with same seqno but different checksums.
		 */

		if ((vginfo->mda_size != vgsummary->mda_size) || (vginfo->mda_checksum != vgsummary->mda_checksum)) {
			log_warn("WARNING: scan of VG %s from %s mda%d found mda_checksum %x mda_size %zu vs %x %zu",
				 vgname, dev_name(info->dev), vgsummary->mda_num,
				 vgsummary->mda_checksum, vgsummary->mda_size,
				 vginfo->mda_checksum, vginfo->mda_size);
			vginfo->scan_summary_mismatch = true;
			vgsummary->mismatch = 1;
			return 0;
		}

		/*
		 * The seqno and checksum matches what was previously seen;
		 * the summary values have already been saved in vginfo.
		 */
		return 1;
	}

 update_vginfo:
	if (!_lvmcache_update_vgstatus(info, vgsummary->vgstatus, vgsummary->creation_host,
				       vgsummary->lock_type, vgsummary->system_id)) {
		/*
		 * This shouldn't happen, it's an internal errror, and we can leave
		 * the info in place without saving the summary values in vginfo.
		 */
		log_error("Failed to update VG %s info in lvmcache.", vgname);
	}

	_lvmcache_update_pvsummaries(vginfo, vgsummary);

	return 1;
}

/*
 * FIXME: quit trying to mirror changes that a command is making into lvmcache.
 *
 * First, it's complicated and hard to ensure it's done correctly in every case
 * (it would be much easier and safer to just toss out what's in lvmcache and
 * reread the info to recreate it from scratch instead of trying to make sure
 * every possible discrete state change is correct.)
 *
 * Second, it's unnecessary if commands just use the vg they are modifying
 * rather than also trying to get info from lvmcache.  The lvmcache state
 * should be populated by label_scan, used to perform vg_read's, and then
 * ignored (or dropped so it can't be used).
 *
 * lvmcache info is already used very little after a command begins its
 * operation.  The code that's supposed to keep the lvmcache in sync with
 * changes being made to disk could be half wrong and we wouldn't know it.
 * That creates a landmine for someone who might try to use a bit of it that
 * isn't being updated correctly.
 */

int lvmcache_update_vg_from_write(struct volume_group *vg)
{
	struct pv_list *pvl;
	struct lvmcache_info *info;
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	struct lvmcache_vgsummary vgsummary = {
		.vgname = vg->name,
		.vgstatus = vg->status,
		.vgid = vg->id,
		.system_id = vg->system_id,
		.lock_type = vg->lock_type
	};

	dm_list_iterate_items(pvl, &vg->pvs) {
		(void) dm_strncpy(pvid_s, (char *) &pvl->pv->id, sizeof(pvid_s));
		/* FIXME Could pvl->pv->dev->pvid ever be different? */
		if ((info = lvmcache_info_from_pvid(pvid_s, pvl->pv->dev, 0)) &&
		    !lvmcache_update_vgname_and_id(info, &vgsummary))
			return_0;
	}

	return 1;
}

/*
 * The lvmcache representation of a VG after label_scan can be incorrect
 * because the label_scan does not use the full VG metadata to construct
 * vginfo/info.  PVs that don't hold VG metadata weren't attached to the vginfo
 * during label scan, and PVs with outdated metadata (claiming to be in the VG,
 * but not listed in the latest metadata) were attached to the vginfo, but
 * shouldn't be.  After vg_read() gets the full metdata in the form of a 'vg',
 * this function is called to fix up the lvmcache representation of the VG
 * using the 'vg'.
 */

int lvmcache_update_vg_from_read(struct volume_group *vg, unsigned precommitted)
{
	struct pv_list *pvl;
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info, *info2;
	struct metadata_area *mda;
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	struct lvmcache_vgsummary vgsummary = {
		.vgname = vg->name,
		.vgstatus = vg->status,
		.vgid = vg->id,
		.system_id = vg->system_id,
		.lock_type = vg->lock_type
	};

	if (!(vginfo = lvmcache_vginfo_from_vgname(vg->name, (const char *)&vg->id))) {
		log_error(INTERNAL_ERROR "lvmcache_update_vg %s no vginfo", vg->name);
		return 0;
	}

	/*
	 * The label scan doesn't know when a PV with old metadata has been
	 * removed from the VG.  Now with the vg we can tell, so remove the
	 * info for a PV that has been removed from the VG with
	 * vgreduce --removemissing.
	 */
	dm_list_iterate_items_safe(info, info2, &vginfo->infos) {
		int found = 0;
		dm_list_iterate_items(pvl, &vg->pvs) {
			if (pvl->pv->dev != info->dev)
				continue;
			found = 1;
			break;
		}

		if (found)
			continue;

		log_warn("WARNING: outdated PV %s seqno %u has been removed in current VG %s seqno %u.",
			 dev_name(info->dev), info->summary_seqno, vg->name, vginfo->seqno);

		_drop_vginfo(info, vginfo); /* remove from vginfo->infos */
		dm_list_add(&vginfo->outdated_infos, &info->list);
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		(void) dm_strncpy(pvid_s, (char *) &pvl->pv->id, sizeof(pvid_s));

		if (!(info = lvmcache_info_from_pvid(pvid_s, pvl->pv->dev, 0))) {
			log_debug_cache("lvmcache_update_vg %s no info for %s %s",
					vg->name,
					(char *) &pvl->pv->id,
					pvl->pv->dev ? dev_name(pvl->pv->dev) : "missing");
			continue;
		}

		log_debug_cache("lvmcache_update_vg %s for info %s",
				vg->name, dev_name(info->dev));
		
		/*
		 * FIXME: use a different function that just attaches info's that
		 * had no metadata onto the correct vginfo.
		 *
		 * info's for PVs without metadata were not connected to the
		 * vginfo by label_scan, so do it here.
		 */
		if (!lvmcache_update_vgname_and_id(info, &vgsummary)) {
			log_debug_cache("lvmcache_update_vg %s failed to update info for %s",
					vg->name, dev_name(info->dev));
		}

		/*
		 * Ignored mdas were not copied from info->mdas to
		 * fid->metadata_areas... when create_text_instance (at the
		 * start of vg_read) called lvmcache_fid_add_mdas_vg because at
		 * that point the info's were not connected to the vginfo
		 * (since label_scan didn't know this without metadata.)
		 */
		dm_list_iterate_items(mda, &info->mdas) {
			if (!mda_is_ignored(mda))
				continue;
			log_debug("lvmcache_update_vg %s copy ignored mdas for %s", vg->name, dev_name(info->dev));
			if (!lvmcache_fid_add_mdas_pv(info, vg->fid)) {
				log_debug_cache("lvmcache_update_vg %s failed to update mdas for %s",
					        vg->name, dev_name(info->dev));
			}
			break;
		}
	}

	return 1;
}

/*
 * We can see multiple different devices with the
 * same pvid, i.e. duplicates.
 *
 * There may be different reasons for seeing two
 * devices with the same pvid:
 * - multipath showing two paths to the same thing
 * - one device copied to another, e.g. with dd,
 *   also referred to as cloned devices.
 * - a "subsystem" taking a device and creating
 *   another device of its own that represents the
 *   underlying device it is using, e.g. using dm
 *   to create an identity mapping of a PV.
 *
 * Given duplicate devices, we have to choose one
 * of them to be the "preferred" dev, i.e. the one
 * that will be referenced in lvmcache, by pv->dev.
 * We can keep the existing dev, that's currently
 * used in lvmcache, or we can replace the existing
 * dev with the new duplicate.
 *
 * Regardless of which device is preferred, we need
 * to print messages explaining which devices were
 * found so that a user can sort out for themselves
 * what has happened if the preferred device is not
 * the one they are interested in.
 *
 * If a user wants to use the non-preferred device,
 * they will need to filter out the device that
 * lvm is preferring.
 *
 * The dev_subsystem calls check if the major number
 * of the dev is part of a subsystem like DM/MD/DRBD.
 * A dev that's part of a subsystem is preferred over a
 * duplicate of that dev that is not part of a
 * subsystem.
 *
 * FIXME: there may be other reasons to prefer one
 * device over another:
 *
 * . are there other use/open counts we could check
 *   beyond the holders?
 *
 * . check if either is bad/usable and prefer
 *   the good one?
 *
 * . prefer the one with smaller minor number?
 *   Might avoid disturbing things due to a new
 *   transient duplicate?
 */

static struct lvmcache_info * _create_info(struct labeller *labeller, struct device *dev, uint64_t label_sector)
{
	struct lvmcache_info *info;
	struct label *label;

	if (!(label = label_create(labeller)))
		return_NULL;
	if (!(info = zalloc(sizeof(*info)))) {
		log_error("lvmcache_info allocation failed");
		label_destroy(label);
		return NULL;
	}

	label->dev = dev;
	label->sector = label_sector;

	info->dev = dev;
	info->fmt = labeller->fmt;

	label->info = info;
	info->label = label;

	dm_list_init(&info->list);
	lvmcache_del_mdas(info);
	lvmcache_del_das(info);
	lvmcache_del_bas(info);

	return info;
}

struct lvmcache_info *lvmcache_add(struct labeller *labeller,
				   const char *pvid, struct device *dev, uint64_t label_sector,
				   const char *vgname, const char *vgid, uint32_t vgstatus,
				   int *is_duplicate)
{
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	char uuid[64] __attribute__((aligned(8)));
	struct lvmcache_vgsummary vgsummary = { 0 };
	struct lvmcache_info *info;
	struct lvmcache_info *info_lookup;
	struct device_list *devl;
	int created = 0;

	(void) dm_strncpy(pvid_s, pvid, sizeof(pvid_s));

	if (!id_write_format((const struct id *)&pvid_s, uuid, sizeof(uuid)))
		stack;

	/*
	 * Find existing info struct in _pvid_hash or create a new one.
	 *
	 * Don't pass the known "dev" as an arg here.  The mismatching
	 * devs for the duplicate case is checked below.
	 */

	info = lvmcache_info_from_pvid(pvid_s, NULL, 0);

	if (!info)
		info = lvmcache_info_from_pvid(dev->pvid, NULL, 0);

	if (!info) {
		info = _create_info(labeller, dev, label_sector);
		created = 1;
	}

	if (!info)
		return_NULL;

	/*
	 * If an existing info struct was found, check if any values are new.
	 */
	if (!created) {
		if (info->dev != dev) {
			log_debug_cache("Saving initial duplicate device %s previously seen on %s with PVID %s.",
					dev_name(dev), dev_name(info->dev), uuid);

			strncpy(dev->pvid, pvid_s, sizeof(dev->pvid));

			/* shouldn't happen */
			if (dev_in_device_list(dev, &_initial_duplicates))
				log_debug_cache("Initial duplicate already in list %s", dev_name(dev));
			else {
				/*
				 * Keep the existing PV/dev in lvmcache, and save the
				 * new duplicate in the list of duplicates.  After
				 * scanning is complete, compare the duplicate devs
				 * with those in lvmcache to check if one of the
				 * duplicates is preferred and if so switch lvmcache to
				 * use it.
				 */

				if (!(devl = zalloc(sizeof(*devl))))
					return_NULL;
				devl->dev = dev;

				dm_list_add(&_initial_duplicates, &devl->list);
			}

			if (is_duplicate)
				*is_duplicate = 1;
			return NULL;
		}

		if (info->dev->pvid[0] && pvid[0] && strcmp(pvid_s, info->dev->pvid)) {
			/* This happens when running pvcreate on an existing PV. */
			log_debug_cache("Changing pvid on dev %s from %s to %s",
					dev_name(info->dev), info->dev->pvid, pvid_s);
		}

		if (info->label->labeller != labeller) {
			log_verbose("Changing labeller on dev %s from %s to %s",
				    dev_name(info->dev),
				    info->label->labeller->fmt->name,
				    labeller->fmt->name);
			label_destroy(info->label);
			if (!(info->label = label_create(labeller)))
				return_NULL;
			info->label->info = info;
		}
	}

	/*
	 * Add or update the _pvid_hash mapping, pvid to info.
	 */

	info_lookup = dm_hash_lookup(_pvid_hash, pvid_s);
	if ((info_lookup == info) && !strcmp(info->dev->pvid, pvid_s))
		goto update_vginfo;

	if (info->dev->pvid[0])
		dm_hash_remove(_pvid_hash, info->dev->pvid);

	strncpy(info->dev->pvid, pvid_s, sizeof(info->dev->pvid));

	if (!dm_hash_insert(_pvid_hash, pvid_s, info)) {
		log_error("Adding pvid to hash failed %s", pvid_s);
		return NULL;
	}

update_vginfo:
	vgsummary.vgstatus = vgstatus;
	vgsummary.vgname = vgname;
	if (vgid)
		strncpy((char *)&vgsummary.vgid, vgid, sizeof(vgsummary.vgid));

	if (!lvmcache_update_vgname_and_id(info, &vgsummary)) {
		if (created) {
			dm_hash_remove(_pvid_hash, pvid_s);
			strcpy(info->dev->pvid, "");
			free(info->label);
			free(info);
		}
		return NULL;
	}

	return info;
}

static void _lvmcache_destroy_entry(struct lvmcache_info *info)
{
	_vginfo_detach_info(info);
	info->dev->pvid[0] = 0;
	label_destroy(info->label);
	free(info);
}

static void _lvmcache_destroy_vgnamelist(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_vginfo *next;

	do {
		next = vginfo->next;
		if (!_free_vginfo(vginfo))
			stack;
	} while ((vginfo = next));
}

void lvmcache_destroy(struct cmd_context *cmd, int retain_orphans, int reset)
{
	log_debug_cache("Dropping VG info");

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

	if (!dm_list_empty(&_vginfos))
		log_error(INTERNAL_ERROR "_vginfos list should be empty");
	dm_list_init(&_vginfos);

	/*
	 * Move the current _unused_duplicates to _prev_unused_duplicate_devs
	 * before destroying _unused_duplicates.
	 *
	 * One command can init/populate/destroy lvmcache multiple times.  Each
	 * time it will encounter duplicates and choose the preferrred devs.
	 * We want the same preferred devices to be chosen each time, so save
	 * the unpreferred devs here so that _choose_preferred_devs can use
	 * this to make the same choice each time.
	 */
	_destroy_device_list(&_prev_unused_duplicate_devs);
	dm_list_splice(&_prev_unused_duplicate_devs, &_unused_duplicates);
	_destroy_device_list(&_unused_duplicates);
	_destroy_device_list(&_initial_duplicates); /* should be empty anyway */

	if (retain_orphans) {
		struct format_type *fmt;

		if (!lvmcache_init(cmd))
			stack;

		dm_list_iterate_items(fmt, &cmd->formats) {
			if (!lvmcache_add_orphan_vginfo(fmt->orphan_vg_name, fmt))
				stack;
		}
	}
}

int lvmcache_fid_add_mdas(struct lvmcache_info *info, struct format_instance *fid,
			  const char *id, int id_len)
{
	return fid_add_mdas(fid, &info->mdas, id, id_len);
}

int lvmcache_fid_add_mdas_pv(struct lvmcache_info *info, struct format_instance *fid)
{
	return lvmcache_fid_add_mdas(info, fid, info->dev->pvid, ID_LEN);
}

/*
 * This is the linkage where information is passed from
 * the label_scan to vg_read.
 *
 * Called by create_text_instance in vg_read to copy the
 * mda's found during label_scan and saved in info->mdas,
 * to fid->metadata_areas_in_use which is used by vg_read.
 */
int lvmcache_fid_add_mdas_vg(struct lvmcache_vginfo *vginfo, struct format_instance *fid)
{
	struct lvmcache_info *info;
	dm_list_iterate_items(info, &vginfo->infos) {
		if (!lvmcache_fid_add_mdas_pv(info, fid))
			return_0;
	}
	return 1;
}

int lvmcache_populate_pv_fields(struct lvmcache_info *info,
				struct volume_group *vg,
				struct physical_volume *pv)
{
	struct data_area_list *da;
	
	if (!info->label) {
		log_error("No cached label for orphan PV %s", pv_dev_name(pv));
		return 0;
	}

	pv->label_sector = info->label->sector;
	pv->dev = info->dev;
	pv->fmt = info->fmt;
	pv->size = info->device_size >> SECTOR_SHIFT;
	pv->vg_name = FMT_TEXT_ORPHAN_VG_NAME;
	memcpy(&pv->id, &info->dev->pvid, sizeof(pv->id));

	if (!pv->size) {
		log_error("PV %s size is zero.", dev_name(info->dev));
		return 0;
	}

	/* Currently only support exactly one data area */
	if (dm_list_size(&info->das) != 1) {
		log_error("Must be exactly one data area (found %d) on PV %s",
			  dm_list_size(&info->das), dev_name(info->dev));
		return 0;
	}

	/* Currently only support one bootloader area at most */
	if (dm_list_size(&info->bas) > 1) {
		log_error("Must be at most one bootloader area (found %d) on PV %s",
			  dm_list_size(&info->bas), dev_name(info->dev));
		return 0;
	}

	dm_list_iterate_items(da, &info->das)
		pv->pe_start = da->disk_locn.offset >> SECTOR_SHIFT;

	dm_list_iterate_items(da, &info->bas) {
		pv->ba_start = da->disk_locn.offset >> SECTOR_SHIFT;
		pv->ba_size = da->disk_locn.size >> SECTOR_SHIFT;
	}

	return 1;
}

int lvmcache_check_format(struct lvmcache_info *info, const struct format_type *fmt)
{
	if (info->fmt != fmt) {
		log_error("PV %s is a different format (seqno %s)",
			  dev_name(info->dev), info->fmt->name);
		return 0;
	}
	return 1;
}

void lvmcache_del_mdas(struct lvmcache_info *info)
{
	if (info->mdas.n)
		del_mdas(&info->mdas);
	dm_list_init(&info->mdas);

	if (info->bad_mdas.n)
		del_mdas(&info->bad_mdas);
	dm_list_init(&info->bad_mdas);
}

void lvmcache_del_das(struct lvmcache_info *info)
{
	if (info->das.n)
		del_das(&info->das);
	dm_list_init(&info->das);
}

void lvmcache_del_bas(struct lvmcache_info *info)
{
	if (info->bas.n)
		del_bas(&info->bas);
	dm_list_init(&info->bas);
}

int lvmcache_add_mda(struct lvmcache_info *info, struct device *dev,
		     uint64_t start, uint64_t size, unsigned ignored,
		     struct metadata_area **mda_new)
{
	return add_mda(info->fmt, NULL, &info->mdas, dev, start, size, ignored, mda_new);
}

int lvmcache_add_da(struct lvmcache_info *info, uint64_t start, uint64_t size)
{
	return add_da(NULL, &info->das, start, size);
}

int lvmcache_add_ba(struct lvmcache_info *info, uint64_t start, uint64_t size)
{
	return add_ba(NULL, &info->bas, start, size);
}

void lvmcache_update_pv(struct lvmcache_info *info, struct physical_volume *pv,
			const struct format_type *fmt)
{
	info->device_size = pv->size << SECTOR_SHIFT;
	info->fmt = fmt;
}

int lvmcache_update_das(struct lvmcache_info *info, struct physical_volume *pv)
{
	struct data_area_list *da;
	if (info->das.n) {
		if (!pv->pe_start)
			dm_list_iterate_items(da, &info->das)
				pv->pe_start = da->disk_locn.offset >> SECTOR_SHIFT;
		del_das(&info->das);
	} else
		dm_list_init(&info->das);

	if (!add_da(NULL, &info->das, pv->pe_start << SECTOR_SHIFT, 0 /*pv->size << SECTOR_SHIFT*/))
		return_0;

	return 1;
}

int lvmcache_update_bas(struct lvmcache_info *info, struct physical_volume *pv)
{
	struct data_area_list *ba;
	if (info->bas.n) {
		if (!pv->ba_start && !pv->ba_size)
			dm_list_iterate_items(ba, &info->bas) {
				pv->ba_start = ba->disk_locn.offset >> SECTOR_SHIFT;
				pv->ba_size = ba->disk_locn.size >> SECTOR_SHIFT;
			}
		del_das(&info->bas);
	} else
		dm_list_init(&info->bas);

	if (!add_ba(NULL, &info->bas, pv->ba_start << SECTOR_SHIFT, pv->ba_size << SECTOR_SHIFT))
		return_0;

	return 1;
}

int lvmcache_foreach_pv(struct lvmcache_vginfo *vginfo,
			int (*fun)(struct lvmcache_info *, void *),
			void *baton)
{
	struct lvmcache_info *info;
	dm_list_iterate_items(info, &vginfo->infos) {
		if (!fun(info, baton))
			return_0;
	}

	return 1;
}

int lvmcache_foreach_mda(struct lvmcache_info *info,
			 int (*fun)(struct metadata_area *, void *),
			 void *baton)
{
	struct metadata_area *mda;
	dm_list_iterate_items(mda, &info->mdas) {
		if (!fun(mda, baton))
			return_0;
	}

	return 1;
}

unsigned lvmcache_mda_count(struct lvmcache_info *info)
{
	return dm_list_size(&info->mdas);
}

int lvmcache_foreach_da(struct lvmcache_info *info,
			int (*fun)(struct disk_locn *, void *),
			void *baton)
{
	struct data_area_list *da;
	dm_list_iterate_items(da, &info->das) {
		if (!fun(&da->disk_locn, baton))
			return_0;
	}

	return 1;
}

int lvmcache_foreach_ba(struct lvmcache_info *info,
			 int (*fun)(struct disk_locn *, void *),
			 void *baton)
{
	struct data_area_list *ba;
	dm_list_iterate_items(ba, &info->bas) {
		if (!fun(&ba->disk_locn, baton))
			return_0;
	}

	return 1;
}

struct label *lvmcache_get_dev_label(struct device *dev)
{
	struct lvmcache_info *info;

	if ((info = lvmcache_info_from_pvid(dev->pvid, NULL, 0))) {
		/* dev would be different for a duplicate */
		if (info->dev == dev)
			return info->label;
	}
	return NULL;
}

int lvmcache_has_dev_info(struct device *dev)
{
	if (lvmcache_info_from_pvid(dev->pvid, NULL, 0))
		return 1;
	return 0;
}

/*
 * The lifetime of the label returned is tied to the lifetime of the
 * lvmcache_info which is the same as lvmcache itself.
 */
struct label *lvmcache_get_label(struct lvmcache_info *info) {
	return info->label;
}

uint64_t lvmcache_device_size(struct lvmcache_info *info) {
	return info->device_size;
}

void lvmcache_set_device_size(struct lvmcache_info *info, uint64_t size) {
	info->device_size = size;
}

struct device *lvmcache_device(struct lvmcache_info *info) {
	return info->dev;
}
void lvmcache_set_ext_version(struct lvmcache_info *info, uint32_t version)
{
	info->ext_version = version;
}

uint32_t lvmcache_ext_version(struct lvmcache_info *info) {
	return info->ext_version;
}

void lvmcache_set_ext_flags(struct lvmcache_info *info, uint32_t flags) {
	info->ext_flags = flags;
}

uint32_t lvmcache_ext_flags(struct lvmcache_info *info) {
	return info->ext_flags;
}

uint64_t lvmcache_smallest_mda_size(struct lvmcache_info *info)
{
	if (!info)
		return UINT64_C(0);

	return find_min_mda_size(&info->mdas);
}

const struct format_type *lvmcache_fmt(struct lvmcache_info *info) {
	return info->fmt;
}

int lvmcache_lookup_mda(struct lvmcache_vgsummary *vgsummary)
{
	struct lvmcache_vginfo *vginfo;

	if (!vgsummary->mda_size)
		return 0;

	/* FIXME Index the checksums */
	dm_list_iterate_items(vginfo, &_vginfos) {
		if (vgsummary->mda_checksum == vginfo->mda_checksum &&
		    vgsummary->mda_size == vginfo->mda_size &&
		    !is_orphan_vg(vginfo->vgname)) {
			vgsummary->vgname = vginfo->vgname;
			vgsummary->creation_host = vginfo->creation_host;
			vgsummary->vgstatus = vginfo->status;
			vgsummary->seqno = vginfo->seqno;
			/* vginfo->vgid has 1 extra byte then vgsummary->vgid */
			memcpy(&vgsummary->vgid, vginfo->vgid, sizeof(vgsummary->vgid));

			return 1;
		}
	}

	return 0;
}

int lvmcache_contains_lock_type_sanlock(struct cmd_context *cmd)
{
	struct lvmcache_vginfo *vginfo;

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (vginfo->lock_type && !strcmp(vginfo->lock_type, "sanlock"))
			return 1;
	}

	return 0;
}

void lvmcache_get_max_name_lengths(struct cmd_context *cmd,
				   unsigned *pv_max_name_len,
				   unsigned *vg_max_name_len)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	unsigned len;

	*vg_max_name_len = 0;
	*pv_max_name_len = 0;

	dm_list_iterate_items(vginfo, &_vginfos) {
		len = strlen(vginfo->vgname);
		if (*vg_max_name_len < len)
			*vg_max_name_len = len;

		dm_list_iterate_items(info, &vginfo->infos) {
			len = strlen(dev_name(info->dev));
			if (*pv_max_name_len < len)
				*pv_max_name_len = len;
		}
	}
}

int lvmcache_vg_is_foreign(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	int ret = 0;

	if ((vginfo = lvmcache_vginfo_from_vgid(vgid)))
		ret = !is_system_id_allowed(cmd, vginfo->system_id);

	return ret;
}

/*
 * Example of reading four devs in sequence from the same VG:
 *
 * dev1:
 *    lvmcache: creates vginfo with initial values
 *
 * dev2: all checksums match.
 *    mda_header checksum matches vginfo from dev1
 *    metadata checksum matches vginfo from dev1
 *    metadata is not parsed, and the vgsummary values copied
 *    from lvmcache from dev1 and passed back to lvmcache for dev2.
 *    lvmcache: attach info for dev2 to existing vginfo
 *
 * dev3: mda_header and metadata have unmatching checksums.
 *    mda_header checksum matches vginfo from dev1
 *    metadata checksum doesn't match vginfo from dev1
 *    produces read error in config.c
 *    lvmcache: info for dev3 is deleted, FIXME: use a defective state
 *
 * dev4: mda_header and metadata have matching checksums, but
 *       does not match checksum in lvmcache from prev dev.
 *    mda_header checksum doesn't match vginfo from dev1
 *    lvmcache_lookup_mda returns 0, no vgname, no checksum_only
 *    lvmcache: update_vgname_and_id sees checksum from dev4 does not
 *    match vginfo from dev1, so vginfo->scan_summary_mismatch is set.
 *    attach info for dev4 to existing vginfo
 *
 * dev5: config parsing error.
 *    lvmcache: info for dev5 is deleted, FIXME: use a defective state
 */

bool lvmcache_scan_mismatch(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;

	if (!vgname || !vgid)
		return true;

	if ((vginfo = lvmcache_vginfo_from_vgid(vgid)))
		return vginfo->scan_summary_mismatch;

	return true;
}

static uint64_t _max_metadata_size;

void lvmcache_save_metadata_size(uint64_t val)
{
	if (!_max_metadata_size)
		_max_metadata_size = val;
	else if (_max_metadata_size < val)
		_max_metadata_size = val;
}

uint64_t lvmcache_max_metadata_size(void)
{
	return _max_metadata_size;
}

int lvmcache_vginfo_has_pvid(struct lvmcache_vginfo *vginfo, char *pvid)
{
	struct lvmcache_info *info;

	dm_list_iterate_items(info, &vginfo->infos) {
		if (!strcmp(info->dev->pvid, pvid))
			return 1;
	}
	return 0;
}

struct metadata_area *lvmcache_get_mda(struct cmd_context *cmd,
				       const char *vgname,
				       struct device *dev,
				       int use_mda_num)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	struct metadata_area *mda;

	if (!use_mda_num)
		use_mda_num = 1;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return NULL;

	dm_list_iterate_items(info, &vginfo->infos) {
		if (info->dev != dev)
			continue;

		dm_list_iterate_items(mda, &info->mdas) {
			if ((use_mda_num == 1) && (mda->status & MDA_PRIMARY))
				return mda;
			if ((use_mda_num == 2) && !(mda->status & MDA_PRIMARY))
				return mda;
		}
		return NULL;
	}
	return NULL;
}

/*
 * This is used by the metadata repair command to check if
 * the metadata on a dev needs repair because it's old.
 */
bool lvmcache_has_old_metadata(struct cmd_context *cmd, const char *vgname, const char *vgid, struct device *dev)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	/* shouldn't happen */
	if (!vgname || !vgid)
		return false;

	/* shouldn't happen */
	if (!(vginfo = lvmcache_vginfo_from_vgid(vgid)))
		return false;

	/* shouldn't happen */
	if (!(info = lvmcache_info_from_pvid(dev->pvid, NULL, 0)))
		return false;

	/* writing to a new PV */
	if (!info->summary_seqno)
		return false;

	/* on same dev, one mda has newer metadata than the other */
	if (info->summary_seqno_mismatch)
		return true;

	/* one or both mdas on this dev has older metadata than another dev */
	if (vginfo->seqno > info->summary_seqno)
		return true;

	return false;
}

void lvmcache_get_outdated_devs(struct cmd_context *cmd,
				const char *vgname, const char *vgid,
				struct dm_list *devs)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	struct device_list *devl;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		log_error(INTERNAL_ERROR "lvmcache_get_outdated_devs no vginfo %s", vgname);
		return;
	}

	dm_list_iterate_items(info, &vginfo->outdated_infos) {
		if (!(devl = zalloc(sizeof(*devl))))
			return;
		devl->dev = info->dev;
		dm_list_add(devs, &devl->list);
	}
}

void lvmcache_del_outdated_devs(struct cmd_context *cmd,
				const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info, *info2;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		log_error(INTERNAL_ERROR "lvmcache_del_outdated_devs no vginfo");
		return;
	}

	dm_list_iterate_items_safe(info, info2, &vginfo->outdated_infos)
		lvmcache_del(info);
}

void lvmcache_get_outdated_mdas(struct cmd_context *cmd,
				const char *vgname, const char *vgid,
				struct device *dev,
				struct dm_list **mdas)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	*mdas = NULL;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		log_error(INTERNAL_ERROR "lvmcache_get_outdated_mdas no vginfo");
		return;
	}

	dm_list_iterate_items(info, &vginfo->outdated_infos) {
		if (info->dev != dev)
			continue;
		*mdas = &info->mdas;
		return;
	}
}

bool lvmcache_is_outdated_dev(struct cmd_context *cmd,
			     const char *vgname, const char *vgid,
			     struct device *dev)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		log_error(INTERNAL_ERROR "lvmcache_get_outdated_mdas no vginfo");
		return false;
	}

	dm_list_iterate_items(info, &vginfo->outdated_infos) {
		if (info->dev == dev)
			return true;
	}

	return false;
}
