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
#include "lib/device/device_id.h"
#include "lib/locking/locking.h"
#include "lib/metadata/metadata.h"
#include "lib/mm/memlock.h"
#include "lib/format_text/format-text.h"
#include "lib/config/config.h"
#include "lib/filters/filter.h"

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
	uint32_t summary_seqno;      /* vg seqno found on this dev during scan */
	uint32_t mda1_seqno;
	uint32_t mda2_seqno;
};

/* One per VG */
struct lvmcache_vginfo {
	struct dm_list list;	 /* _vginfos */
	struct dm_list infos;	/* List head for lvmcache_infos */
	struct dm_list outdated_infos; /* vg_read moves info from infos to outdated_infos */
	struct dm_list pvsummaries; /* pv_list taken directly from vgsummary */
	const struct format_type *fmt;
	char *vgname;		/* "" == orphan */
	uint32_t status;
	char vgid[ID_LEN + 1];
	char _padding[7];
	char *creation_host;
	char *system_id;
	char *lock_type;
	uint32_t mda_checksum;
	size_t mda_size;
	uint32_t seqno;
	bool scan_summary_mismatch; /* vgsummary from devs had mismatching seqno or checksum */
	bool has_duplicate_local_vgname;   /* this local vg and another local vg have same name */
	bool has_duplicate_foreign_vgname; /* this foreign vg and another foreign vg have same name */
};

/*
 * Each VG found during scan gets a vginfo struct.
 * Each vginfo is in _vginfos and _vgid_hash, and
 * _vgname_hash (unless disabled due to duplicate vgnames).
 */

static struct dm_hash_table *_pvid_hash = NULL;
static struct dm_hash_table *_vgid_hash = NULL;
static struct dm_hash_table *_vgname_hash = NULL;
static DM_LIST_INIT(_vginfos);
static DM_LIST_INIT(_initial_duplicates);
static DM_LIST_INIT(_unused_duplicates);
static int _vgs_locked = 0;
static int _found_duplicate_vgnames = 0;
static int _outdated_warning = 0;

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

	if (!(_vgname_hash = dm_hash_create(127)))
		return 0;

	if (!(_vgid_hash = dm_hash_create(126)))
		return 0;

	if (!(_pvid_hash = dm_hash_create(125)))
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

unsigned int lvmcache_vg_info_count(void)
{
	struct lvmcache_vginfo *vginfo;
	unsigned int count = 0;

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (is_orphan_vg(vginfo->vgname))
			continue;
		count++;
	}
	return count;
}

int lvmcache_found_duplicate_vgnames(void)
{
	return _found_duplicate_vgnames;
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

	if ((devl = device_list_find_dev(&_initial_duplicates, dev))) {
		log_debug_cache("delete dev from initial duplicates %s", dev_name(dev));
		dm_list_del(&devl->list);
	}
	if ((devl = device_list_find_dev(&_unused_duplicates, dev))) {
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

void lvmcache_save_bad_mda(struct lvmcache_info *info, struct metadata_area *mda)
{
	if (mda->mda_num == 1)
		info->mda1_bad = true;
	else if (mda->mda_num == 2)
		info->mda2_bad = true;
	dm_list_add(&info->bad_mdas, &mda->list);
}

void lvmcache_del_save_bad_mda(struct lvmcache_info *info, int mda_num, int bad_mda_flag)
{
	struct metadata_area *mda, *mda_safe;

	dm_list_iterate_items_safe(mda, mda_safe, &info->mdas) {
		if (mda->mda_num == mda_num) {
			dm_list_del(&mda->list);
			mda->bad_fields |= bad_mda_flag;
			lvmcache_save_bad_mda(info, mda);
			break;
		}
	}
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

struct metadata_area *lvmcache_get_dev_mda(struct device *dev, int mda_num)
{
	struct lvmcache_info *info;
	struct metadata_area *mda;

	if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0)))
		return NULL;

	dm_list_iterate_items(mda, &info->mdas) {
		if (mda->mda_num == mda_num)
			return mda;
	}
	return NULL;
}

static void _vginfo_detach_info(struct lvmcache_info *info)
{
	if (!dm_list_empty(&info->list)) {
		dm_list_del(&info->list);
		dm_list_init(&info->list);
	}

	info->vginfo = NULL;
}

static struct lvmcache_vginfo *_search_vginfos_list(const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;

	if (vgid) {
		dm_list_iterate_items(vginfo, &_vginfos) {
			if (!memcmp(vgid, vginfo->vgid, ID_LEN))
				return vginfo;
		}
	} else {
		dm_list_iterate_items(vginfo, &_vginfos) {
			if (!strcmp(vgname, vginfo->vgname))
				return vginfo;
		}
	}
	return NULL;
}

static struct lvmcache_vginfo *_vginfo_lookup(const char *vgname, const char *vgid_arg)
{
	char vgid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct lvmcache_vginfo *vginfo;

	/* In case vgid is not null terminated */
	if (vgid_arg)
		memcpy(vgid, vgid_arg, ID_LEN);

	if (vgid_arg) {
		if ((vginfo = dm_hash_lookup(_vgid_hash, vgid))) {
			if (vgname && strcmp(vginfo->vgname, vgname)) {
				log_warn("WARNING: lookup found duplicate VGID %s for VGs %s and %s.", vgid, vginfo->vgname, vgname);
				if ((vginfo = dm_hash_lookup(_vgname_hash, vgname))) {
					if (!memcmp(vginfo->vgid, vgid, ID_LEN))
						return vginfo;
				}
				return NULL;
			}
			return vginfo;
		} else {
			/* lookup by vgid that doesn't exist */
			return NULL;
		}
	}

	if (vgname && !_found_duplicate_vgnames) {
		if ((vginfo = dm_hash_lookup(_vgname_hash, vgname))) {
			if (vginfo->has_duplicate_local_vgname) {
				/* should never happen, found_duplicate_vgnames should be set */
				log_error(INTERNAL_ERROR "vginfo_lookup %s has_duplicate_local_vgname.", vgname);
				return NULL;
			}
			return vginfo;
		}
	}

	if (vgname && _found_duplicate_vgnames) {
		if ((vginfo = _search_vginfos_list(vgname, vgid[0] ? vgid : NULL))) {
			if (vginfo->has_duplicate_local_vgname) {
				log_debug("vginfo_lookup %s has_duplicate_local_vgname return none.", vgname);
				return NULL;
			}
			return vginfo;
		}
	}

	/* lookup by vgname that doesn't exist */
	return NULL;
}

struct lvmcache_vginfo *lvmcache_vginfo_from_vgname(const char *vgname, const char *vgid)
{
	return _vginfo_lookup(vgname, vgid);
}

struct lvmcache_vginfo *lvmcache_vginfo_from_vgid(const char *vgid)
{
	return _vginfo_lookup(NULL, vgid);
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

	if (_found_duplicate_vgnames) {
		if (!(vginfo = _search_vginfos_list(vgname, NULL)))
			return NULL;
	} else {
		if (!(vginfo = dm_hash_lookup(_vgname_hash, vgname)))
			return NULL;
	}

	if (vginfo->has_duplicate_local_vgname) {
		/*
		 * return NULL if there is a local VG with the same name since
		 * we don't know which to use.
		 */
		return NULL;
	}

	if (vginfo->has_duplicate_foreign_vgname)
		return NULL;

	return dm_pool_strdup(cmd->mem, vginfo->vgid);
}

bool lvmcache_has_duplicate_local_vgname(const char *vgid, const char *vgname)
{
	struct lvmcache_vginfo *vginfo;

	if (_found_duplicate_vgnames) {
		if (!(vginfo = _search_vginfos_list(vgname, vgid)))
			return false;
	} else {
		if (!(vginfo = dm_hash_lookup(_vgname_hash, vgname)))
			return false;
	}

	if (vginfo->has_duplicate_local_vgname)
		return true;
	return false;
}

/*
 * If valid_only is set, data will only be returned if the cached data is
 * known still to be valid.
 *
 * When the device being worked with is known, pass that dev as the second arg.
 * This ensures that when duplicates exist, the wrong dev isn't used.
 */
struct lvmcache_info *lvmcache_info_from_pvid(const char *pvid_arg, struct device *dev, int valid_only)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct lvmcache_info *info;

	if (!_pvid_hash || !pvid_arg)
		return NULL;

	/* For cases where pvid_arg is not null terminated. */
	memcpy(pvid, pvid_arg, ID_LEN);

	if (!(info = dm_hash_lookup(_pvid_hash, pvid)))
		return NULL;

	/*
	 * When handling duplicate PVs, more than one device can have this pvid.
	 */
	if (dev && info->dev && (info->dev != dev)) {
		log_debug_cache("Ignoring lvmcache info for dev %s because dev %s was requested for PVID %s.",
				dev_name(info->dev), dev_name(dev), pvid);
		return NULL;
	}

	return info;
}

struct lvmcache_info *lvmcache_info_from_pv_id(const struct id *pv_id_arg, struct device *dev, int valid_only)
{
	/*
	 * Since we know that lvmcache_info_from_pvid directly above
	 * does not assume pvid_arg is null-terminated, we make an
	 * exception here and cast a struct id to char *.
	 */
	return lvmcache_info_from_pvid((const char *)pv_id_arg, dev, valid_only);
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

static uint64_t _get_pvsummary_size(const char *pvid_arg)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct lvmcache_vginfo *vginfo;
	struct pv_list *pvl;

	/* In case pvid_arg is not null terminated. */
	memcpy(pvid, pvid_arg, ID_LEN);

	dm_list_iterate_items(vginfo, &_vginfos) {
		dm_list_iterate_items(pvl, &vginfo->pvsummaries) {
			if (!memcmp(pvid, &pvl->pv->id.uuid, ID_LEN))
				return pvl->pv->size;
		}
	}

	return 0;
}

static const char *_get_pvsummary_device_hint(const char *pvid_arg)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct lvmcache_vginfo *vginfo;
	struct pv_list *pvl;

	/* In case pvid_arg is not null terminated. */
	memcpy(pvid, pvid_arg, ID_LEN);

	dm_list_iterate_items(vginfo, &_vginfos) {
		dm_list_iterate_items(pvl, &vginfo->pvsummaries) {
			if (!memcmp(pvid, &pvl->pv->id.uuid, ID_LEN))
				return pvl->pv->device_hint;
		}
	}

	return NULL;
}

static const char *_get_pvsummary_device_id(const char *pvid_arg, const char **device_id_type)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct lvmcache_vginfo *vginfo;
	struct pv_list *pvl;

	/* In case pvid_arg is not null terminated. */
	memcpy(pvid, pvid_arg, ID_LEN);

	dm_list_iterate_items(vginfo, &_vginfos) {
		dm_list_iterate_items(pvl, &vginfo->pvsummaries) {
			if (!memcmp(&pvid, &pvl->pv->id.uuid, ID_LEN)) {
				*device_id_type = pvl->pv->device_id_type;
				return pvl->pv->device_id;
			}
		}
	}

	return NULL;
}

int lvmcache_pvsummary_count(const char *vgname)
{
	struct lvmcache_vginfo *vginfo;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return_0;

	return dm_list_size(&vginfo->pvsummaries);
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
			if (!memcmp(&pvl->pv->id.uuid, devl->dev->pvid, ID_LEN))
				return 1;
		}
	}
	return 0;
}

bool lvmcache_dev_is_unused_duplicate(struct device *dev)
{
	return device_list_find_dev(&_unused_duplicates, dev) ? true : false;
}

static void _warn_unused_duplicates(struct cmd_context *cmd)
{
	char pvid_dashed[64] __attribute__((aligned(8)));
	struct lvmcache_info *info;
	struct device_list *devl;
	struct id id;

	dm_list_iterate_items(devl, &_unused_duplicates) {
		memcpy(&id, devl->dev->pvid, ID_LEN);
		if (!id_write_format(&id, pvid_dashed, sizeof(pvid_dashed)))
			stack;

		log_warn("WARNING: Not using device %s for PV %s.", dev_name(devl->dev), pvid_dashed);
	}

	dm_list_iterate_items(devl, &_unused_duplicates) {
		/* info for the preferred device that we're actually using */
		if (!(info = lvmcache_info_from_pvid(devl->dev->pvid, NULL, 0)))
			continue;

		memcpy(&id, info->dev->pvid, ID_LEN);
		if (!id_write_format(&id, pvid_dashed, sizeof(pvid_dashed)))
			stack;

		log_warn("WARNING: PV %s prefers device %s because %s.",
			 pvid_dashed, dev_name(info->dev), info->dev->duplicate_prefer_reason);
	}
}

static int _all_multipath_components(struct cmd_context *cmd, struct lvmcache_info *info, const char *pvid,
				     struct dm_list *altdevs, struct device **dev_mpath)
{
	struct device_list *devl;
	struct device *dev_mp = NULL;
	struct device *dev1 = NULL;
	struct device *dev;
	char wwid1_buf[DEV_WWID_SIZE] = { 0 };
	char wwid_buf[DEV_WWID_SIZE] = { 0 };
	const char *wwid1 = NULL;
	const char *wwid = NULL;
	int diff_wwid = 0;
	int same_wwid = 0;
	int dev_is_mp;

	*dev_mpath = NULL;

	if (!find_config_tree_bool(cmd, devices_multipath_component_detection_CFG, NULL))
		return 0;

	/* This function only makes sense with more than one dev. */
	if ((info && dm_list_empty(altdevs)) || (!info && (dm_list_size(altdevs) == 1))) {
		log_debug("Skip multipath component checks with single device for PVID %s", pvid);
		return 0;
	}

	log_debug("Checking for multipath components for duplicate PVID %s", pvid);

	if (info) {
		dev = info->dev;
		dev_is_mp = (cmd->dev_types->device_mapper_major == MAJOR(dev->dev)) && dev_has_mpath_uuid(cmd, dev, NULL);

		/*
		 * dev_mpath_component_wwid allocates wwid from dm_pool,
		 * device_id_system_read does not and needs free.
		 */

		if (dev_is_mp) {
			if ((wwid1 = dev_mpath_component_wwid(cmd, dev))) {
				strncpy(wwid1_buf, wwid1, DEV_WWID_SIZE-1);
				dev_mp = dev;
				dev1 = dev;
			}
		} else {
			if ((wwid1 = device_id_system_read(cmd, dev, DEV_ID_TYPE_SYS_WWID))) {
				strncpy(wwid1_buf, wwid1, DEV_WWID_SIZE-1);
				free((char *)wwid1);
				dev1 = dev;
			}
		}
	}

	dm_list_iterate_items(devl, altdevs) {
		dev = devl->dev;
		dev_is_mp = (cmd->dev_types->device_mapper_major == MAJOR(dev->dev)) && dev_has_mpath_uuid(cmd, dev, NULL);

		if (dev_is_mp) {
			if ((wwid = dev_mpath_component_wwid(cmd, dev)))
				strncpy(wwid_buf, wwid, DEV_WWID_SIZE-1);
		} else {
			if ((wwid = device_id_system_read(cmd, dev, DEV_ID_TYPE_SYS_WWID))) {
				strncpy(wwid_buf, wwid, DEV_WWID_SIZE-1);
				free((char *)wwid);
			}
		}

		if (!wwid_buf[0] && wwid1_buf[0]) {
			log_debug("Different wwids for duplicate PVs %s %s %s none",
				  dev_name(dev1), wwid1_buf, dev_name(dev));
			diff_wwid++;
			continue;
		}

		if (!wwid_buf[0])
			continue;

		if (!wwid1_buf[0]) {
			memcpy(wwid1_buf, wwid_buf, DEV_WWID_SIZE-1);
			dev1 = dev;
			continue;
		}

		/* Different wwids indicates these are not multipath components. */
		if (strcmp(wwid1_buf, wwid_buf)) {
			log_debug("Different wwids for duplicate PVs %s %s %s %s",
				  dev_name(dev1), wwid1_buf, dev_name(dev), wwid_buf);
			diff_wwid++;
			continue;
		}

		/* Different mpath devs with the same wwid shouldn't happen. */
		if (dev_is_mp && dev_mp) {
			log_print_unless_silent("Found multiple multipath devices for PVID %s WWID %s: %s %s.",
						pvid, wwid1_buf, dev_name(dev_mp), dev_name(dev));
			continue;
		}

		log_debug("Same wwids for duplicate PVs %s %s", dev_name(dev1), dev_name(dev));
		same_wwid++;

		/* Save the mpath device so it can be used as the PV. */
		if (dev_is_mp)
			dev_mp = dev;
	}

	if (diff_wwid || !same_wwid)
		return 0;

	if (dev_mp)
		log_debug("Found multipath device %s for PVID %s WWID %s.", dev_name(dev_mp), pvid, wwid1_buf);

	*dev_mpath = dev_mp;
	return 1;
}

static int _all_md_components(struct cmd_context *cmd, struct lvmcache_info *info, const char *pvid,
			      struct dm_list *altdevs, struct device **dev_md_out)
{
	struct device_list *devl;
	struct device *dev_md = NULL;
	struct device *dev;
	int real_dup = 0;
 
	*dev_md_out = NULL;

	/* There will often be no info struct because of the extra_md_checks function. */
 
	if (info && (cmd->dev_types->md_major == MAJOR(info->dev->dev)))
		dev_md = info->dev;
 
	dm_list_iterate_items(devl, altdevs) {
		dev = devl->dev;
 
		if (cmd->dev_types->md_major == MAJOR(dev->dev)) {
			if (dev_md) {
				/* md devs themselves are dups */
				log_debug("Found multiple md devices for PVID %s: %s %s",
					  pvid, dev_name(dev_md), dev_name(dev));
				real_dup = 1;
				break;
			} else
				dev_md = dev;
		} else {
			if (!dev_is_md_component(cmd, dev, NULL, 1)) {
				/* md dev copied to another device */
				real_dup = 1;
				break;
			}
		}
	}
 
	if (real_dup)
		return 0;
 
	if (dev_md)
		log_debug("Found md device %s for PVID %s.", dev_name(dev_md), pvid);
 
	*dev_md_out = dev_md;
	return 1;
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
	const char *pvid;
	const char *reason;
	const char *device_hint;
	struct dm_list altdevs;
	struct dm_list new_unused;
	struct dev_types *dt = cmd->dev_types;
	struct device_list *devl, *devl_safe, *devl_add, *devl_del;
	struct lvmcache_info *info;
	struct device *dev1, *dev2;
	struct device *dev_mpath, *dev_md;
	struct device *dev_drop;
	const char *device_id = NULL, *device_id_type = NULL;
	const char *idname1 = NULL, *idname2 = NULL;
	uint32_t dev1_major, dev1_minor, dev2_major, dev2_minor;
	uint64_t dev1_size, dev2_size, pvsummary_size;
	int in_subsys1, in_subsys2;
	int is_dm1, is_dm2;
	int has_fs1, has_fs2;
	int has_lv1, has_lv2;
	int same_size1, same_size2;
	int same_name1 = 0, same_name2 = 0;
	int same_id1 = 0, same_id2 = 0;
	int change;

	dm_list_init(&new_unused);

	/*
	 * Create a list of all alternate devs for the same pvid: altdevs.
	 */
next:
	dm_list_init(&altdevs);
	pvid = NULL;
	dev_mpath = NULL;
	dev_md = NULL;

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

	info = lvmcache_info_from_pvid(pvid, NULL, 0);

	/*
	 * Usually and ideally, components of md and multipath devs should have
	 * been excluded by filters, and not scanned for a PV.  In some unusual
	 * cases the components can get through the filters, and a PV can be
	 * found on them.  Detecting the same PVID on both the component and
	 * the md/mpath device gives us a last chance to drop the component.
	 * An md/mpath component device is completely ignored, as if it had
	 * been filtered, and not kept in the list unused duplicates.
	 *
	 * One issue related to eliminating mpath/md duplicate PVs here is
	 * that it occurs after label_scan, and hints are created based
	 * on what label_scan finds, so hints are disabled due to duplicate
	 * PVs that are later resolved here.
	 */

	/*
	 * Get rid of multipath components based on matching wwids.
	 */
	if (_all_multipath_components(cmd, info, pvid, &altdevs, &dev_mpath)) {
		if (info && dev_mpath && (info->dev != dev_mpath)) {
			/*
			 * info should be dropped from lvmcache and info->dev
			 * should be treated as if it had been excluded by a filter.
			 * dev_mpath should be added to lvmcache by the caller.
			 */
			dev_drop = info->dev;

			/* Have caller add dev_mpath to lvmcache. */
			log_debug("Using multipath device %s for PVID %s.", dev_name(dev_mpath), pvid);
			if ((devl_add = zalloc(sizeof(*devl_add)))) {
				devl_add->dev = dev_mpath;
				dm_list_add(add_cache_devs, &devl_add->list);
			}

			/* Remove dev_mpath from altdevs. */
			if ((devl = device_list_find_dev(&altdevs, dev_mpath))) {
				dm_list_del(&devl->list);
				free(devl);
			}

			/* Remove info from lvmcache that came from the component dev. */
			log_debug("Ignoring multipath component %s with PVID %s (dropping info)", dev_name(dev_drop), pvid);
			lvmcache_del(info);
			info = NULL;

			/* Make the component dev look like it was filtered. */
			cmd->filter->wipe(cmd, cmd->filter, dev_drop, NULL);
			dev_drop->flags &= ~DEV_SCAN_FOUND_LABEL;
		}

		if (info && !dev_mpath) {
			/*
			 * Only mpath component devs were found and no actual
			 * multipath dev, so drop the component from lvmcache.
			 */
			dev_drop = info->dev;

			log_debug("Ignoring multipath component %s with PVID %s (dropping info)", dev_name(dev_drop), pvid);
			lvmcache_del(info);
			info = NULL;

			/* Make the component dev look like it was filtered. */
			cmd->filter->wipe(cmd, cmd->filter, dev_drop, NULL);
			dev_drop->flags &= ~DEV_SCAN_FOUND_LABEL;
		}

		dm_list_iterate_items_safe(devl, devl_safe, &altdevs) {
			/*
			 * The altdevs are all mpath components that should look
			 * like they were filtered, they are not in lvmcache.
			 */
			dev_drop = devl->dev;

			log_debug("Ignoring multipath component %s with PVID %s (dropping duplicate)", dev_name(dev_drop), pvid);
			dm_list_del(&devl->list);
			free(devl);

			cmd->filter->wipe(cmd, cmd->filter, dev_drop, NULL);
			dev_drop->flags &= ~DEV_SCAN_FOUND_LABEL;
		}
		goto next;
	}

	/*
	 * Get rid of any md components.
	 */
	if (_all_md_components(cmd, info, pvid, &altdevs, &dev_md)) {
		if (info && dev_md && (info->dev != dev_md)) {
			/*
			 * info should be dropped from lvmcache and info->dev
			 * should be treated as if it had been excluded by a filter.
			 * dev_md should be added to lvmcache by the caller.
			 * Often this info struct has been removed by
			 * lvmcache_extra_md_component_checks.
			 */
			dev_drop = info->dev;

			/* Have caller add dev_md to lvmcache. */
			log_debug("Using md device %s for PVID %s.", dev_name(dev_md), pvid);
			if ((devl_add = zalloc(sizeof(*devl_add)))) {
				devl_add->dev = dev_md;
				dm_list_add(add_cache_devs, &devl_add->list);
			}

			/* Remove dev_md from altdevs. */
			if ((devl = device_list_find_dev(&altdevs, dev_md))) {
				dm_list_del(&devl->list);
				free(devl);
			}

			/* Remove info from lvmcache that came from the component dev. */
			log_debug("Ignoring md component %s with PVID %s (dropping info)", dev_name(dev_drop), pvid);
			lvmcache_del(info);
			info = NULL;

			/* Make the component dev look like it was filtered. */
			cmd->filter->wipe(cmd, cmd->filter, dev_drop, NULL);
			dev_drop->flags &= ~DEV_SCAN_FOUND_LABEL;
		}

		if (!info && dev_md) {
			/*
			 * The info struct was from a component and was dropped
			 * and the actual md dev was found on initial_duplicates
			 * and the caller should add it to lvmcache.
			 */

			/* Have caller add dev_md to lvmcache. */
			log_debug("Using md device %s for PVID %s.", dev_name(dev_md), pvid);
			if ((devl_add = zalloc(sizeof(*devl_add)))) {
				devl_add->dev = dev_md;
				dm_list_add(add_cache_devs, &devl_add->list);
			}

			/* Remove dev_md from altdevs. */
			if ((devl = device_list_find_dev(&altdevs, dev_md))) {
				dm_list_del(&devl->list);
				free(devl);
			}
		}

		if (info && !dev_md) {
			/*
			 * Only md component devs were found and no actual
			 * md dev, so drop the component from lvmcache.
			 */
			dev_drop = info->dev;

			log_debug("Ignoring md component %s with PVID %s (dropping info)", dev_name(dev_drop), pvid);
			lvmcache_del(info);
			info = NULL;

			/* Make the component dev look like it was filtered. */
			cmd->filter->wipe(cmd, cmd->filter, dev_drop, NULL);
			dev_drop->flags &= ~DEV_SCAN_FOUND_LABEL;
		}

		dm_list_iterate_items_safe(devl, devl_safe, &altdevs) {
			/*
			 * The altdevs are all md components that should look
			 * like they were filtered, they are not in lvmcache.
			 */
			dev_drop = devl->dev;

			log_debug("Ignoring md component %s with PVID %s (dropping duplicate)", dev_name(dev_drop), pvid);
			dm_list_del(&devl->list);
			free(devl);

			cmd->filter->wipe(cmd, cmd->filter, dev_drop, NULL);
			dev_drop->flags &= ~DEV_SCAN_FOUND_LABEL;
		}
		goto next;
	}

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
		} else if (dm_list_empty(&altdevs)) {
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

		if ((device_id = _get_pvsummary_device_id(devl->dev->pvid, &device_id_type))) {
			uint16_t idtype = idtype_from_str(device_id_type);

			if (idtype) {
				idname1 = device_id_system_read(cmd, dev1, idtype);
				idname2 = device_id_system_read(cmd, dev2, idtype);
			}
			if (idname1)
				same_id1 = !strcmp(idname1, device_id);
			if (idname2)
				same_id2 = !strcmp(idname2, device_id);
		}

		has_lv1 = dev_is_used_by_active_lv(cmd, dev1, NULL, NULL, NULL, NULL);
		has_lv2 = dev_is_used_by_active_lv(cmd, dev2, NULL, NULL, NULL, NULL);

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

		log_debug_cache("PV %s: device_id %s. %s is %s. %s is %s.",
				devl->dev->pvid,
				device_id ?: ".",
				dev_name(dev1), idname1 ?: ".",
				dev_name(dev2), idname2 ?: ".");

		log_debug_cache("PV %s: size %llu. %s is %llu. %s is %llu.",
				devl->dev->pvid,
				(unsigned long long)pvsummary_size,
				dev_name(dev1), (unsigned long long)dev1_size,
				dev_name(dev2), (unsigned long long)dev2_size);

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

		free((void *)idname1);
		free((void *)idname2);
		idname1 = NULL;
		idname2 = NULL;

		change = 0;

		if (same_id1 && !same_id2) {
			/* keep 1 */
			reason = "device id";
		} else if (same_id2 && !same_id1) {
			/* change to 2 */
			change = 1;
			reason = "device id";
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

		if (!(devl_add = device_list_find_dev(&altdevs, dev1))) {
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

		if (!(devl_add = device_list_find_dev(&altdevs, dev1))) {
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

int lvmcache_label_reopen_vg_rw(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid)))
		return_0;

	dm_list_iterate_items(info, &vginfo->infos) {
		if (!label_scan_reopen_rw(info->dev))
			return_0;
	}

	return 1;
}

/*
 * During label_scan, the md component filter is applied to each device after
 * the device header has been read.  This often just checks the start of the
 * device for an md header, and if the device has an md header at the end, the
 * md component filter wouldn't detect it.  In some cases, the full md filter
 * is enabled during label_scan, in which case the md component filter will
 * check both the start and end of the device for md superblocks.
 *
 * In this function, after label_scan is done, we may decide that a full md
 * component check should be applied to a device if it hasn't been yet.  This
 * is based on some clues or uncertainty that arose during label_scan.
 *
 * label_scan saved metadata info about pvs in lvmcache pvsummaries.  That
 * pvsummary metadata includes the pv size.  So now, after label_scan is done,
 * we can compare the pv size with the size of the device the pv was read from.
 * If the pv and dev sizes do not match, it can sometimes be normal, but other
 * times it can be a clue that label_scan mistakenly read the pv from an md
 * component device instead of from the md device itself.  So for unmatching
 * sizes, we do a full md component check on the device.
 *
 * It might be nice to do this checking in the filter (when passes_filter is
 * called after the initial read), but that doesn't work because passes_filter
 * is called before _text_read so metadata/pvsummary info is not yet available
 * which this function uses.
 *
 * The unique value of this function is that it can eliminate md components
 * without there being duplicate PVs.  But, there will often be duplicate PVs,
 * handled by _all_md_components(), where other devs with the same pvid will be
 * in _initial_duplicates.  One could be the md device itself which will be
 * added to lvmcache by choose_duplicates, and other duplicates that are
 * components will be dropped.
 */

void lvmcache_extra_md_component_checks(struct cmd_context *cmd)
{
	struct lvmcache_vginfo *vginfo, *vginfo2;
	struct lvmcache_info *info, *info2;
	struct device *dev;
	const char *device_hint;
	uint64_t devsize, pvsize;
	int do_check_size, do_check_name;
	int md_check_start;

	/*
	 * use_full_md_check: if set then no more needs to be done here,
	 * all devs have already been fully checked as md components.
	 *
	 * md_component_checks "full": use_full_md_check was set, and caused
	 *  filter-md to already do a full check, no more is needed.
	 *
	 * md_component_checks "start": skip end of device md component checks,
	 *  the start of device has already been checked by filter-md.
	 *
	 * md_component_checks "auto": do full checks only when lvm finds some
	 * clue or reasons to believe it might be useful, which is what this
	 * function is looking for.
	 */
	if (!cmd->md_component_detection || cmd->use_full_md_check ||
	    !strcmp(cmd->md_component_checks, "none"))
		return;

	md_check_start = !strcmp(cmd->md_component_checks, "start");

	/*
	 * We want to avoid extra scanning for end-of-device md superblocks
	 * whenever possible, since it can add up to a lot of extra io if we're
	 * not careful to do it only when there's a good reason to believe a
	 * dev is an md component.
	 *
	 * If the pv/dev size mismatches are commonly occurring for
	 * non-md-components then we'll want to stop using that as a trigger
	 * for the full md check.
	 */

	dm_list_iterate_items_safe(vginfo, vginfo2, &_vginfos) {
		char vgid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
		memcpy(vgid, vginfo->vgid, ID_LEN);

		dm_list_iterate_items_safe(info, info2, &vginfo->infos) {
			dev = info->dev;
			device_hint = _get_pvsummary_device_hint(dev->pvid);
			pvsize = _get_pvsummary_size(dev->pvid);
			devsize = dev->size;
			do_check_size = 0;
			do_check_name = 0;

			if (!devsize && !dev_get_size(dev, &devsize))
				log_debug("No size for %s.", dev_name(dev));

			/*
			 * PV larger than dev not common; dev larger than PV
			 * can be common, but not as often as PV larger.
			 */
			if (pvsize && devsize && (pvsize != devsize))
				do_check_size = 1;
			if (device_hint && !strncmp(device_hint, "/dev/md", 7) &&
			    (MAJOR(info->dev->dev) != cmd->dev_types->md_major))
				do_check_name = 1;

			if (!do_check_size && !do_check_name)
				continue;

			/*
			 * If only the size is different (which can be fairly
			 * common for non-md-component devs) and the user has
			 * set "start" to disable full md checks, then skip it.
			 * If the size is different, *and* the device name hint
			 * looks like an md device, then it seems very likely
			 * to be an md component, so do a full check on it even
			 * if the user has set "start".
			 * 
			 * In "auto" mode, do a full check if either the size
			 * or the name indicates a possible md component.
			 */
			if (do_check_size && !do_check_name && md_check_start) {
				log_debug("extra md component check skip %llu %llu device_hint %s dev %s",
					  (unsigned long long)pvsize, (unsigned long long)devsize,
					  device_hint ?: "none", dev_name(dev));
				continue;
			}

			log_debug("extra md component check %llu %llu device_hint %s dev %s",
				  (unsigned long long)pvsize, (unsigned long long)devsize,
				  device_hint ?: "none", dev_name(dev));

			if (dev_is_md_component(cmd, dev, NULL, 1)) {
				log_debug("Ignoring PV from md component %s with PVID %s (metadata %s %llu)",
					  dev_name(dev), dev->pvid, device_hint ?: "none", (unsigned long long)pvsize);
				dev->flags &= ~DEV_SCAN_FOUND_LABEL;
				/* lvmcache_del will also delete vginfo if info was last one */
				lvmcache_del(info);
				cmd->filter->wipe(cmd, cmd->filter, dev, NULL);

				/* If vginfo was deleted don't continue using vginfo->infos */
				if (!_search_vginfos_list(NULL, vgid))
					break;
			}
		}
	}
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
 * after vg_read updates to lvmcache state, then the lvmcache will be
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
	struct device_list *devl;

	log_debug_cache("lvmcache label scan begin");

	/*
	 * Duplicates found during this label scan are added to _initial_duplicates.
	 */
	_destroy_device_list(&_initial_duplicates);
	_destroy_device_list(&_unused_duplicates);

	/*
	 * Do the actual scanning.  This populates lvmcache
	 * with infos/vginfos based on reading headers from
	 * each device, and a vg summary from each mda.
	 */
	if (!label_scan(cmd))
		return_0;

	/*
	 * device_ids_validate() found devices using a sys_serial device id
	 * which had a PVID on disk that did not match the PVID in the devices
	 * file.  Serial numbers may not always be unique, so any device with
	 * the same serial number is found and searched for the correct PVID.
	 * If the PVID is found on a device that has not been scanned, then
	 * it needs to be scanned so it can be used.
	 */
	if (!dm_list_empty(&cmd->device_ids_check_serial)) {
		struct dm_list scan_devs;
		dm_list_init(&scan_devs);
		device_ids_check_serial(cmd, &scan_devs, 0, NULL);
		if (!dm_list_empty(&scan_devs))
			label_scan_devs(cmd, cmd->filter, &scan_devs);
	}

	/*
	 * device_ids_invalid is set by device_ids_validate() when there
	 * are entries in the devices file that need to be corrected,
	 * i.e. device IDs read from the system and/or PVIDs read from
	 * disk do not match info in the devices file.  This is usually
	 * related to incorrect device names which routinely change on
	 * reboot.  When device names change for entries that use
	 * IDTYPE=devname, it often means that all devs on the system
	 * need to be scanned to find the new device for the PVIDs.
	 * device_ids_validate() will update the devices file to correct
	 * some info, but to locate new devices for PVIDs, it defers
	 * to device_ids_search() which involves label scanning.
	 *
	 * device_ids_refresh_trigger is set by device_ids_read() when
	 * it sees that the local machine doesn't match the machine
	 * that wrote the devices file, and device IDs of all types
	 * may need to be replaced for the PVIDs in the devices file.
	 * This also means that all devs on the system need to be
	 * scanned to find the new devices for the PVIDs.
	 *
	 * When device_ids_search() locates the correct devices
	 * for the PVs in the devices file, it returns those new
	 * devices in the refresh_devs list.  Those devs need to
	 * be passed to label_scan to populate lvmcache info.
	 */
	if (cmd->device_ids_invalid || cmd->device_ids_refresh_trigger) {
		struct dm_list new_devs;
		dm_list_init(&new_devs);
		device_ids_search(cmd, &new_devs, 0, 0, NULL);
		if (!dm_list_empty(&new_devs))
			label_scan_devs(cmd, cmd->filter, &new_devs);
	}

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
			label_scan_dev(cmd, devl->dev);
		}

		_destroy_device_list(&add_cache_devs);

		dm_list_splice(&_unused_duplicates, &del_cache_devs);

		/* Warn about unused duplicates that the user might want to resolve. */
		_warn_unused_duplicates(cmd);
	}

	log_debug_cache("lvmcache label scan done");
	return 1;
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

struct device *lvmcache_device_from_pv_id(struct cmd_context *cmd, const struct id *pvid, uint64_t *label_sector)
{
	struct lvmcache_info *info;

	if ((info = lvmcache_info_from_pv_id(pvid, NULL, 0))) {
		if (info->label && label_sector)
			*label_sector = info->label->sector;
		return info->dev;
	}
	return NULL;
}

int lvmcache_pvid_in_unused_duplicates(const char *pvid)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, &_unused_duplicates) {
		if (!memcmp(devl->dev->pvid, pvid, ID_LEN))
			return 1;
	}
	return 0;
}

static void _free_vginfo(struct lvmcache_vginfo *vginfo)
{
	free(vginfo->vgname);
	free(vginfo->system_id);
	free(vginfo->creation_host);
	free(vginfo->lock_type);
	free(vginfo);
}

/*
 * Remove vginfo from standard lists/hashes.
 */
static void _drop_vginfo(struct lvmcache_info *info, struct lvmcache_vginfo *vginfo)
{
	if (info)
		_vginfo_detach_info(info);

	/* vginfo still referenced? */
	if (!vginfo || is_orphan_vg(vginfo->vgname) ||
	    !dm_list_empty(&vginfo->infos))
		return;

	if (dm_hash_lookup(_vgname_hash, vginfo->vgname) == vginfo)
		dm_hash_remove(_vgname_hash, vginfo->vgname);

	dm_hash_remove(_vgid_hash, vginfo->vgid);

	dm_list_del(&vginfo->list); /* _vginfos list */

	_free_vginfo(vginfo);
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

	if ((info = lvmcache_info_from_pvid(dev->pvid, dev, 0)))
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
	    !memcmp(vginfo->vgid, vgid, ID_LEN))
		return 1;

	if (vginfo && *vginfo->vgid)
		dm_hash_remove(_vgid_hash, vginfo->vgid);
	if (!vgid) {
		/* FIXME: unreachable code path */
		log_debug_cache("lvmcache: %s: clearing VGID", info ? dev_name(info->dev) : vginfo->vgname);
		return 1;
	}

	memset(vginfo->vgid, 0, sizeof(vginfo->vgid));
	memcpy(vginfo->vgid, vgid, ID_LEN);
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

static int _lvmcache_update_vgname(struct cmd_context *cmd,
				   struct lvmcache_info *info,
				   const char *vgname, const char *vgid,
				   const char *system_id,
				   const struct format_type *fmt)
{
	char vgid_dashed[64] __attribute__((aligned(8)));
	char other_dashed[64] __attribute__((aligned(8)));
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_vginfo *other;
	int vginfo_is_allowed;
	int other_is_allowed;

	if (!vgname || (info && info->vginfo && !strcmp(info->vginfo->vgname, vgname)))
		return 1;

	if (!id_write_format((const struct id *)vgid, vgid_dashed, sizeof(vgid_dashed)))
		stack;

	/*
	 * Add vginfo for orphan VG
	 */
	if (!info) {
		if (!(vginfo = zalloc(sizeof(*vginfo)))) {
			log_error("lvmcache adding vg list alloc failed %s", vgname);
			return 0;
		}
		if (!(vginfo->vgname = strdup(vgname))) {
			free(vginfo);
			log_error("lvmcache adding vg name alloc failed %s", vgname);
			return 0;
		}
		dm_list_init(&vginfo->infos);
		dm_list_init(&vginfo->outdated_infos);
		dm_list_init(&vginfo->pvsummaries);
		vginfo->fmt = fmt;

		if (!dm_hash_insert(_vgname_hash, vgname, vginfo)) {
			free(vginfo->vgname);
			free(vginfo);
			return_0;
		}

		if (!_lvmcache_update_vgid(NULL, vginfo, vgid)) {
			free(vginfo->vgname);
			free(vginfo);
			return_0;
		}

		/* Ensure orphans appear last on list_iterate */
		dm_list_add(&_vginfos, &vginfo->list);
		return 1;
	}

	_drop_vginfo(info, info->vginfo);

	vginfo = lvmcache_vginfo_from_vgid(vgid);
	if (vginfo && strcmp(vginfo->vgname, vgname)) {
		log_warn("WARNING: fix duplicate VGID %s for VGs %s and %s (see vgchange -u).", vgid_dashed, vgname, vginfo->vgname);
		vginfo = lvmcache_vginfo_from_vgname(vgname, NULL);
		if (vginfo && memcmp(vginfo->vgid, vgid, ID_LEN)) {
			log_error("Ignoring %s with conflicting VG info %s %s.", dev_name(info->dev), vgid_dashed, vgname);
			return_0;
		}
	}

	if (!vginfo) {
		/*
	 	 * Create a vginfo struct for this VG and put the vginfo
	 	 * into the hash table.
	 	 */

		log_debug_cache("lvmcache adding vginfo for %s %s", vgname, vgid_dashed);

		if (!(vginfo = zalloc(sizeof(*vginfo)))) {
			log_error("lvmcache adding vg list alloc failed %s", vgname);
			return 0;
		}
		if (!(vginfo->vgname = strdup(vgname))) {
			free(vginfo);
			log_error("lvmcache adding vg name alloc failed %s", vgname);
			return 0;
		}
		dm_list_init(&vginfo->infos);
		dm_list_init(&vginfo->outdated_infos);
		dm_list_init(&vginfo->pvsummaries);

		if ((other = dm_hash_lookup(_vgname_hash, vgname))) {
			log_debug_cache("lvmcache adding vginfo found duplicate VG name %s", vgname);

			/*
			 * A different VG (different uuid) can exist with the
			 * same name.  In this case, the two VGs will have
			 * separate vginfo structs, but one will be in the
			 * vgname_hash.  If both vginfos are local/accessible,
			 * then _found_duplicate_vgnames is set which will
			 * disable any further use of the vgname_hash.
			 */

			if (!memcmp(other->vgid, vgid, ID_LEN)) {
				/* shouldn't happen since we looked up by vgid above */
				log_error(INTERNAL_ERROR "lvmcache_update_vgname %s %s %s %s",
					  vgname, vgid, other->vgname, other->vgid);
				free(vginfo->vgname);
				free(vginfo);
				return 0;
			}

			vginfo_is_allowed = is_system_id_allowed(cmd, system_id);
			other_is_allowed = is_system_id_allowed(cmd, other->system_id);

			if (vginfo_is_allowed && other_is_allowed) {
				if (!id_write_format((const struct id *)other->vgid, other_dashed, sizeof(other_dashed)))
					stack;

				vginfo->has_duplicate_local_vgname = 1;
				other->has_duplicate_local_vgname = 1;
				_found_duplicate_vgnames = 1;

				log_warn("WARNING: VG name %s is used by VGs %s and %s.",
					 vgname, vgid_dashed, other_dashed);
				log_warn("Fix duplicate VG names with vgrename uuid, a device filter, or system IDs.");
			}

			if (!vginfo_is_allowed && !other_is_allowed) {
				vginfo->has_duplicate_foreign_vgname = 1;
				other->has_duplicate_foreign_vgname = 1;
			}

			if (!other_is_allowed && vginfo_is_allowed) {
				/* the accessible vginfo must be in vgnames_hash */
				dm_hash_remove(_vgname_hash, vgname);
				if (!dm_hash_insert(_vgname_hash, vgname, vginfo)) {
					log_error("lvmcache adding vginfo to name hash failed %s", vgname);
					return 0;
				}
			}
		} else {
			if (!dm_hash_insert(_vgname_hash, vgname, vginfo)) {
				log_error("lvmcache adding vg to name hash failed %s", vgname);
				free(vginfo->vgname);
				free(vginfo);
				return 0;
			}
		}

		dm_list_add_h(&_vginfos, &vginfo->list);
	}

	vginfo->fmt = fmt;
	info->vginfo = vginfo;
	dm_list_add(&vginfo->infos, &info->list);

	log_debug_cache("lvmcache %s: now in VG %s %s", dev_name(info->dev), vgname, vgid);

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

int lvmcache_add_orphan_vginfo(struct cmd_context *cmd, const char *vgname, struct format_type *fmt)
{
	return _lvmcache_update_vgname(cmd, NULL, vgname, vgname, "", fmt);
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
int lvmcache_update_vgname_and_id(struct cmd_context *cmd, struct lvmcache_info *info, struct lvmcache_vgsummary *vgsummary)
{
	const char *vgname = vgsummary->vgname;
	const char *vgid = vgsummary->vgid;
	struct lvmcache_vginfo *vginfo;

	if (!vgname && !info->vginfo) {
		log_error(INTERNAL_ERROR "NULL vgname handed to cache");
		/* FIXME Remove this */
		vgname = info->fmt->orphan_vg_name;
		vgid = vgname;
	}

	/* FIXME: remove this, it shouldn't be needed */
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
	if (!_lvmcache_update_vgname(cmd, info, vgname, vgid, vgsummary->system_id, info->fmt)) {
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
		 * FIXME: we should check if the majority of mda copies have one
		 * checksum and if so use that copy of metadata, but if there's
		 * not a majority, don't allow the VG to be modified/activated.
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
		 * This shouldn't happen, it's an internal error, and we can leave
		 * the info in place without saving the summary values in vginfo.
		 */
		log_error("Failed to update VG %s info in lvmcache.", vgname);
	}

	_lvmcache_update_pvsummaries(vginfo, vgsummary);

	return 1;
}

/*
 * The lvmcache representation of a VG after label_scan can be incorrect
 * because the label_scan does not use the full VG metadata to construct
 * vginfo/info.  PVs that don't hold VG metadata weren't attached to the vginfo
 * during label scan, and PVs with outdated metadata (claiming to be in the VG,
 * but not listed in the latest metadata) were attached to the vginfo, but
 * shouldn't be.  After vg_read() gets the full metadata in the form of a 'vg',
 * this function is called to fix up the lvmcache representation of the VG
 * using the 'vg'.
 */

int lvmcache_update_vg_from_read(struct volume_group *vg, unsigned precommitted)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	char vgid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct pv_list *pvl;
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info, *info2;
	struct metadata_area *mda;
	struct lvmcache_vgsummary vgsummary = {
		.vgname = vg->name,
		.vgstatus = vg->status,
		.system_id = vg->system_id,
		.lock_type = vg->lock_type
	};

	memcpy(vgid, &vg->id, ID_LEN);
	memcpy(vgsummary.vgid, vgid, ID_LEN);

	if (!(vginfo = lvmcache_vginfo_from_vgname(vg->name, vgid))) {
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

		if (!_outdated_warning++)
			log_warn("See vgck --updatemetadata to clear outdated metadata.");

		_drop_vginfo(info, vginfo); /* remove from vginfo->infos */
		dm_list_add(&vginfo->outdated_infos, &info->list);
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		memcpy(pvid, &pvl->pv->id.uuid, ID_LEN);

		if (!(info = lvmcache_info_from_pvid(pvid, pvl->pv->dev, 0))) {
			log_debug_cache("lvmcache_update_vg %s no info for %s %s",
					vg->name, pvid,
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
		if (!lvmcache_update_vgname_and_id(vg->cmd, info, &vgsummary)) {
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

struct lvmcache_info *lvmcache_add(struct cmd_context *cmd, struct labeller *labeller,
				   const char *pvid_arg, struct device *dev, uint64_t label_sector,
				   const char *vgname, const char *vgid_arg, uint32_t vgstatus,
				   int *is_duplicate)
{
	const char *pvid = pvid_arg;
	const char *vgid = vgid_arg;
	struct lvmcache_vgsummary vgsummary = { 0 };
	struct lvmcache_info *info;
	struct lvmcache_info *info_lookup;
	struct device_list *devl;
	int created = 0;

	/*
	 * Note: ensure that callers of lvmcache_add() pass null terminated
	 * pvid and vgid strings, and do not pass char* that is type cast
	 * from struct id.
	 */

	log_debug_cache("Found PVID %s on %s", pvid, dev_name(dev));

	/*
	 * Find existing info struct in _pvid_hash or create a new one.
	 *
	 * Don't pass the known "dev" as an arg here.  The mismatching
	 * devs for the duplicate case is checked below.
	 */

	info = lvmcache_info_from_pvid(pvid, NULL, 0);

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
					dev_name(dev), dev_name(info->dev), pvid);

			memset(&dev->pvid, 0, sizeof(dev->pvid));
			memcpy(dev->pvid, pvid, ID_LEN);

			/* shouldn't happen */
			if (device_list_find_dev(&_initial_duplicates, dev))
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

		if (info->dev->pvid[0] && pvid[0] && memcmp(pvid, info->dev->pvid, ID_LEN)) {
			/* This happens when running pvcreate on an existing PV. */
			log_debug_cache("Changing pvid on dev %s from %s to %s",
					dev_name(info->dev), info->dev->pvid, pvid);
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

	info_lookup = dm_hash_lookup(_pvid_hash, pvid);
	if ((info_lookup == info) && !memcmp(info->dev->pvid, pvid, ID_LEN))
		goto update_vginfo;

	if (info->dev->pvid[0])
		dm_hash_remove(_pvid_hash, info->dev->pvid);

	memset(info->dev->pvid, 0, sizeof(info->dev->pvid));
	memcpy(info->dev->pvid, pvid, ID_LEN);

	if (!dm_hash_insert(_pvid_hash, pvid, info)) {
		log_error("Adding pvid to hash failed %s", pvid);
		return NULL;
	}

update_vginfo:
	vgsummary.vgstatus = vgstatus;
	vgsummary.vgname = vgname;
	if (vgid && vgid[0])
		memcpy(vgsummary.vgid, vgid, ID_LEN);

	if (!lvmcache_update_vgname_and_id(cmd, info, &vgsummary)) {
		if (created) {
			dm_hash_remove(_pvid_hash, pvid);
			info->dev->pvid[0] = 0;
			free(info->label);
			free(info);
		}
		return NULL;
	}

	return info;
}

static void _lvmcache_destroy_info(struct lvmcache_info *info)
{
	_vginfo_detach_info(info);
	info->dev->pvid[0] = 0;
	label_destroy(info->label);
	free(info);
}

void lvmcache_destroy(struct cmd_context *cmd, int retain_orphans, int reset)
{
	struct lvmcache_vginfo *vginfo, *vginfo2;

	log_debug_cache("Destroy lvmcache content");

	if (_vgid_hash) {
		dm_hash_destroy(_vgid_hash);
		_vgid_hash = NULL;
	}

	if (_pvid_hash) {
		dm_hash_iter(_pvid_hash, (dm_hash_iterate_fn) _lvmcache_destroy_info);
		dm_hash_destroy(_pvid_hash);
		_pvid_hash = NULL;
	}

	if (_vgname_hash) {
		dm_hash_destroy(_vgname_hash);
		_vgname_hash = NULL;
	}

	dm_list_iterate_items_safe(vginfo, vginfo2, &_vginfos) {
		dm_list_del(&vginfo->list);
		_free_vginfo(vginfo);
	}

	if (!dm_list_empty(&_vginfos))
		log_error(INTERNAL_ERROR "vginfos list should be empty");

	dm_list_init(&_vginfos);

	_destroy_device_list(&_unused_duplicates);
	_destroy_device_list(&_initial_duplicates); /* should be empty anyway */

	if (retain_orphans) {
		struct format_type *fmt;

		if (!lvmcache_init(cmd))
			stack;

		dm_list_iterate_items(fmt, &cmd->formats) {
			if (!lvmcache_add_orphan_vginfo(cmd, fmt->orphan_vg_name, fmt))
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
	memset(&pv->id, 0, sizeof(pv->id));
	memcpy(&pv->id, &info->dev->pvid, ID_LEN);

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
			memset(&vgsummary->vgid, 0, sizeof(vgsummary->vgid));
			memcpy(&vgsummary->vgid, vginfo->vgid, ID_LEN);
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
		if (!is_orphan_vg(vginfo->vgname)) {
			len = strlen(vginfo->vgname);
			if (*vg_max_name_len < len)
				*vg_max_name_len = len;
		}

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

int lvmcache_vg_is_lockd_type(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct lvmcache_vginfo *vginfo;

	if ((vginfo = lvmcache_vginfo_from_vgname(vgname, vgid)))
		return is_lockd_type(vginfo->lock_type);

	return 0;
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

int lvmcache_vginfo_has_pvid(struct lvmcache_vginfo *vginfo, const char *pvid_arg)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct lvmcache_info *info;

	/* In case pvid_arg is not null terminated. */
	memcpy(pvid, pvid_arg, ID_LEN);

	dm_list_iterate_items(info, &vginfo->infos) {
		if (!memcmp(info->dev->pvid, pvid, ID_LEN))
			return 1;
	}
	return 0;
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
		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
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

const char *dev_filtered_reason(struct device *dev)
{
	if (dev->filtered_flags & DEV_FILTERED_REGEX)
		return "device is rejected by filter config";
	if (dev->filtered_flags & DEV_FILTERED_INTERNAL)
		return "device is restricted internally";
	if (dev->filtered_flags & DEV_FILTERED_MD_COMPONENT)
		return "device is an md component";
	if (dev->filtered_flags & DEV_FILTERED_MPATH_COMPONENT)
		return "device is a multipath component";
	if (dev->filtered_flags & DEV_FILTERED_PARTITIONED)
		return "device is partitioned";
	if (dev->filtered_flags & DEV_FILTERED_SIGNATURE)
		return "device has a signature";
	if (dev->filtered_flags & DEV_FILTERED_SYSFS)
		return "device is missing sysfs info";
	if (dev->filtered_flags & DEV_FILTERED_DEVTYPE)
		return "device type is unknown";
	if (dev->filtered_flags & DEV_FILTERED_MINSIZE)
		return "device is too small (pv_min_size)";
	if (dev->filtered_flags & DEV_FILTERED_UNUSABLE)
		return "device is not in a usable state";
	if (dev->filtered_flags & DEV_FILTERED_DEVICES_FILE)
		return "device is not in devices file";
	if (dev->filtered_flags & DEV_FILTERED_DEVICES_LIST)
		return "device is not in devices list";
	if (dev->filtered_flags & DEV_FILTERED_IS_LV)
		return "device is an LV";

	/* flag has not been added here */
	if (dev->filtered_flags)
		return "device is filtered";

	return "device cannot be used";
}

const char *devname_error_reason(const char *devname)
{
	struct device *dev;

	if ((dev = dev_cache_get_dev_by_name(devname))) {
		if (dev->filtered_flags)
			return dev_filtered_reason(dev);
		if (lvmcache_dev_is_unused_duplicate(dev))
			return "device is a duplicate";
		/* Avoid this case by adding by adding other more descriptive checks above. */
		return "device cannot be used";
	}

	return "device not found";
}

