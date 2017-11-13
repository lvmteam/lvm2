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

#include "lib.h"
#include "lvmcache.h"
#include "toolcontext.h"
#include "dev-cache.h"
#include "locking.h"
#include "metadata.h"
#include "memlock.h"
#include "str_list.h"
#include "format-text.h"
#include "format_pool.h"
#include "format1.h"
#include "config.h"

#include "lvmetad.h"
#include "lvmetad-client.h"

#define CACHE_INVALID	0x00000001
#define CACHE_LOCKED	0x00000002

/* One per device */
struct lvmcache_info {
	struct dm_list list;	/* Join VG members together */
	struct dm_list mdas;	/* list head for metadata areas */
	struct dm_list das;	/* list head for data areas */
	struct dm_list bas;	/* list head for bootloader areas */
	struct lvmcache_vginfo *vginfo;	/* NULL == unknown */
	struct label *label;
	const struct format_type *fmt;
	struct device *dev;
	uint64_t device_size;	/* Bytes */
	uint32_t ext_version;   /* Extension version */
	uint32_t ext_flags;	/* Extension flags */
	uint32_t status;
};

/* One per VG */
struct lvmcache_vginfo {
	struct dm_list list;	/* Join these vginfos together */
	struct dm_list infos;	/* List head for lvmcache_infos */
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
	size_t vgmetadata_size;
	char *vgmetadata;	/* Copy of VG metadata as format_text string */
	struct dm_config_tree *cft; /* Config tree created from vgmetadata */
				    /* Lifetime is directly tied to vgmetadata */
	struct volume_group *cached_vg;
	unsigned holders;
	unsigned vg_use_count;	/* Counter of vg reusage */
	unsigned precommitted;	/* Is vgmetadata live or precommitted? */
	unsigned cached_vg_invalidated;	/* Signal to regenerate cached_vg */
};

static struct dm_hash_table *_pvid_hash = NULL;
static struct dm_hash_table *_vgid_hash = NULL;
static struct dm_hash_table *_vgname_hash = NULL;
static struct dm_hash_table *_lock_hash = NULL;
static DM_LIST_INIT(_vginfos);
static DM_LIST_INIT(_found_duplicate_devs);
static DM_LIST_INIT(_unused_duplicate_devs);
static int _scanning_in_progress = 0;
static int _has_scanned = 0;
static int _vgs_locked = 0;
static int _vg_global_lock_held = 0;	/* Global lock held when cache wiped? */
static int _found_duplicate_pvs = 0;	/* If we never see a duplicate PV we can skip checking for them later. */
static int _suppress_lock_ordering = 0;

int lvmcache_init(void)
{
	/*
	 * FIXME add a proper lvmcache_locking_reset() that
	 * resets the cache so no previous locks are locked
	 */
	_vgs_locked = 0;

	dm_list_init(&_vginfos);
	dm_list_init(&_found_duplicate_devs);
	dm_list_init(&_unused_duplicate_devs);

	if (!(_vgname_hash = dm_hash_create(128)))
		return 0;

	if (!(_vgid_hash = dm_hash_create(128)))
		return 0;

	if (!(_pvid_hash = dm_hash_create(128)))
		return 0;

	if (!(_lock_hash = dm_hash_create(128)))
		return 0;

	/*
	 * Reinitialising the cache clears the internal record of
	 * which locks are held.  The global lock can be held during
	 * this operation so its state must be restored afterwards.
	 */
	if (_vg_global_lock_held) {
		lvmcache_lock_vgname(VG_GLOBAL, 0);
		_vg_global_lock_held = 0;
	}

	return 1;
}

void lvmcache_seed_infos_from_lvmetad(struct cmd_context *cmd)
{
	if (!lvmetad_used() || _has_scanned)
		return;

	if (!lvmetad_pv_list_to_lvmcache(cmd)) {
		stack;
		return;
	}

	_has_scanned = 1;
}

/* Volume Group metadata cache functions */
static void _free_cached_vgmetadata(struct lvmcache_vginfo *vginfo)
{
	if (!vginfo || !vginfo->vgmetadata)
		return;

	dm_free(vginfo->vgmetadata);

	vginfo->vgmetadata = NULL;

	/* Release also cached config tree */
	if (vginfo->cft) {
		dm_config_destroy(vginfo->cft);
		vginfo->cft = NULL;
	}

	log_debug_cache("lvmcache: VG %s wiped.", vginfo->vgname);

	release_vg(vginfo->cached_vg);
}

/*
 * Cache VG metadata against the vginfo with matching vgid.
 */
static void _store_metadata(struct volume_group *vg, unsigned precommitted)
{
	char uuid[64] __attribute__((aligned(8)));
	struct lvmcache_vginfo *vginfo;
	char *data;
	size_t size;

	if (!(vginfo = lvmcache_vginfo_from_vgid((const char *)&vg->id))) {
		stack;
		return;
	}

	if (!(size = export_vg_to_buffer(vg, &data))) {
		stack;
		_free_cached_vgmetadata(vginfo);
		return;
	}

	/* Avoid reparsing of the same data string */
	if (vginfo->vgmetadata && vginfo->vgmetadata_size == size &&
	    strcmp(vginfo->vgmetadata, data) == 0)
		dm_free(data);
	else {
		_free_cached_vgmetadata(vginfo);
		vginfo->vgmetadata_size = size;
		vginfo->vgmetadata = data;
	}

	vginfo->precommitted = precommitted;

	if (!id_write_format((const struct id *)vginfo->vgid, uuid, sizeof(uuid))) {
		stack;
		return;
	}

	log_debug_cache("lvmcache: VG %s (%s) stored (%" PRIsize_t " bytes%s).",
			vginfo->vgname, uuid, size,
			precommitted ? ", precommitted" : "");
}

static void _update_cache_info_lock_state(struct lvmcache_info *info,
					  int locked,
					  int *cached_vgmetadata_valid)
{
	int was_locked = (info->status & CACHE_LOCKED) ? 1 : 0;

	/*
	 * Cache becomes invalid whenever lock state changes unless
	 * exclusive VG_GLOBAL is held (i.e. while scanning).
	 */
	if (!lvmcache_vgname_is_locked(VG_GLOBAL) && (was_locked != locked)) {
		info->status |= CACHE_INVALID;
		*cached_vgmetadata_valid = 0;
	}

	if (locked)
		info->status |= CACHE_LOCKED;
	else
		info->status &= ~CACHE_LOCKED;
}

static void _update_cache_vginfo_lock_state(struct lvmcache_vginfo *vginfo,
					    int locked)
{
	struct lvmcache_info *info;
	int cached_vgmetadata_valid = 1;

	dm_list_iterate_items(info, &vginfo->infos)
		_update_cache_info_lock_state(info, locked,
					      &cached_vgmetadata_valid);

	if (!cached_vgmetadata_valid)
		_free_cached_vgmetadata(vginfo);
}

static void _update_cache_lock_state(const char *vgname, int locked)
{
	struct lvmcache_vginfo *vginfo;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return;

	_update_cache_vginfo_lock_state(vginfo, locked);
}

static void _drop_metadata(const char *vgname, int drop_precommitted)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return;

	/*
	 * Invalidate cached PV labels.
	 * If cached precommitted metadata exists that means we
	 * already invalidated the PV labels (before caching it)
	 * and we must not do it again.
	 */
	if (!drop_precommitted && vginfo->precommitted && !vginfo->vgmetadata)
		log_error(INTERNAL_ERROR "metadata commit (or revert) missing before "
			  "dropping metadata from cache.");

	if (drop_precommitted || !vginfo->precommitted)
		dm_list_iterate_items(info, &vginfo->infos)
			info->status |= CACHE_INVALID;

	_free_cached_vgmetadata(vginfo);

	/* VG revert */
	if (drop_precommitted)
		vginfo->precommitted = 0;
}

/*
 * Remote node uses this to upgrade precommitted metadata to commited state
 * when receives vg_commit notification.
 * (Note that devices can be suspended here, if so, precommitted metadata are already read.)
 */
void lvmcache_commit_metadata(const char *vgname)
{
	struct lvmcache_vginfo *vginfo;

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)))
		return;

	if (vginfo->precommitted) {
		log_debug_cache("lvmcache: Upgraded pre-committed VG %s metadata to committed.",
				vginfo->vgname);
		vginfo->precommitted = 0;
	}
}

void lvmcache_drop_metadata(const char *vgname, int drop_precommitted)
{
	if (lvmcache_vgname_is_locked(VG_GLOBAL))
		return;

	/* For VG_ORPHANS, we need to invalidate all labels on orphan PVs. */
	if (!strcmp(vgname, VG_ORPHANS)) {
		_drop_metadata(FMT_TEXT_ORPHAN_VG_NAME, 0);
		_drop_metadata(FMT_LVM1_ORPHAN_VG_NAME, 0);
		_drop_metadata(FMT_POOL_ORPHAN_VG_NAME, 0);

		/* Indicate that PVs could now be missing from the cache */
		init_full_scan_done(0);
	} else
		_drop_metadata(vgname, drop_precommitted);
}

/*
 * Ensure vgname2 comes after vgname1 alphabetically.
 * Orphan locks come last.
 * VG_GLOBAL comes first.
 */
static int _vgname_order_correct(const char *vgname1, const char *vgname2)
{
	if (is_global_vg(vgname1))
		return 1;

	if (is_global_vg(vgname2))
		return 0;

	if (is_orphan_vg(vgname1))
		return 0;

	if (is_orphan_vg(vgname2))
		return 1;

	if (strcmp(vgname1, vgname2) < 0)
		return 1;

	return 0;
}

void lvmcache_lock_ordering(int enable)
{
	_suppress_lock_ordering = !enable;
}

/*
 * Ensure VG locks are acquired in alphabetical order.
 */
int lvmcache_verify_lock_order(const char *vgname)
{
	struct dm_hash_node *n;
	const char *vgname2;

	if (_suppress_lock_ordering)
		return 1;

	if (!_lock_hash)
		return_0;

	dm_hash_iterate(n, _lock_hash) {
		if (!dm_hash_get_data(_lock_hash, n))
			return_0;

		if (!(vgname2 = dm_hash_get_key(_lock_hash, n))) {
			log_error(INTERNAL_ERROR "VG lock %s hits NULL.",
				 vgname);
			return 0;
		}

		if (!_vgname_order_correct(vgname2, vgname)) {
			log_errno(EDEADLK, INTERNAL_ERROR "VG lock %s must "
				  "be requested before %s, not after.",
				  vgname, vgname2);
			return 0;
		}
	}

	return 1;
}

void lvmcache_lock_vgname(const char *vgname, int read_only __attribute__((unused)))
{
	if (!_lock_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		return;
	}

	if (dm_hash_lookup(_lock_hash, vgname))
		log_error(INTERNAL_ERROR "Nested locking attempted on VG %s.",
			  vgname);

	if (!dm_hash_insert(_lock_hash, vgname, (void *) 1))
		log_error("Cache locking failure for %s", vgname);

	if (strcmp(vgname, VG_GLOBAL)) {
		_update_cache_lock_state(vgname, 1);
		_vgs_locked++;
	}
}

int lvmcache_vgname_is_locked(const char *vgname)
{
	if (!_lock_hash)
		return 0;

	return dm_hash_lookup(_lock_hash, is_orphan_vg(vgname) ? VG_ORPHANS : vgname) ? 1 : 0;
}

void lvmcache_unlock_vgname(const char *vgname)
{
	if (!dm_hash_lookup(_lock_hash, vgname))
		log_error(INTERNAL_ERROR "Attempt to unlock unlocked VG %s.",
			  vgname);

	if (strcmp(vgname, VG_GLOBAL))
		_update_cache_lock_state(vgname, 0);

	dm_hash_remove(_lock_hash, vgname);

	/* FIXME Do this per-VG */
	if (strcmp(vgname, VG_GLOBAL) && !--_vgs_locked) {
		dev_close_all();
		dev_size_seqno_inc(); /* invalidate all cached dev sizes */
	}
}

int lvmcache_vgs_locked(void)
{
	return _vgs_locked;
}

/*
 * When lvmcache sees a duplicate PV, this is set.
 * process_each_pv() can avoid searching for duplicates
 * by checking this and seeing that no duplicate PVs exist.
 *
 *
 * found_duplicate_pvs tells the process_each_pv code
 * to search the devices list for duplicates, so that
 * devices can be processed together with their
 * duplicates (while processing the VG, rather than
 * reporting pv->dev under the VG, and its duplicate
 * outside the VG context.)
 */
int lvmcache_found_duplicate_pvs(void)
{
	return _found_duplicate_pvs;
}

int lvmcache_get_unused_duplicate_devs(struct cmd_context *cmd, struct dm_list *head)
{
	struct device_list *devl, *devl2;

	dm_list_iterate_items(devl, &_unused_duplicate_devs) {
		if (!(devl2 = dm_pool_alloc(cmd->mem, sizeof(*devl2)))) {
			log_error("device_list element allocation failed");
			return 0;
		}
		devl2->dev = devl->dev;
		dm_list_add(head, &devl2->list);
	}
	return 1;
}

void lvmcache_remove_unchosen_duplicate(struct device *dev)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, &_unused_duplicate_devs) {
		if (devl->dev == dev) {
			dm_list_del(&devl->list);
			return;
		}
	}
}

static void _destroy_duplicate_device_list(struct dm_list *head)
{
	struct device_list *devl, *devl2;

	dm_list_iterate_items_safe(devl, devl2, head) {
		dm_list_del(&devl->list);
		dm_free(devl);
	}
	dm_list_init(head);
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

const struct format_type *lvmcache_fmt_from_vgname(struct cmd_context *cmd,
						   const char *vgname, const char *vgid,
						   unsigned revalidate_labels)
{
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;
	struct label *label;
	struct dm_list *devh, *tmp;
	struct dm_list devs;
	struct device_list *devl;
	struct volume_group *vg;
	const struct format_type *fmt;
	char vgid_found[ID_LEN + 1] __attribute__((aligned(8)));

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
		if (!lvmetad_used())
			return NULL; /* too bad */
		/* If we don't have the info but we have lvmetad, we can ask
		 * there before failing. */
		if ((vg = lvmetad_vg_lookup(cmd, vgname, vgid))) {
			fmt = vg->fid->fmt;
			release_vg(vg);
			return fmt;
		}
		return NULL;
	}

	/*
	 * If this function is called repeatedly, only the first one needs to revalidate.
	 */
	if (!revalidate_labels)
		goto out;

	/*
	 * This function is normally called before reading metadata so
 	 * we check cached labels here. Unfortunately vginfo is volatile.
 	 */
	dm_list_init(&devs);
	dm_list_iterate_items(info, &vginfo->infos) {
		if (!(devl = dm_malloc(sizeof(*devl)))) {
			log_error("device_list element allocation failed");
			return NULL;
		}
		devl->dev = info->dev;
		dm_list_add(&devs, &devl->list);
	}

	memcpy(vgid_found, vginfo->vgid, sizeof(vgid_found));

	dm_list_iterate_safe(devh, tmp, &devs) {
		devl = dm_list_item(devh, struct device_list);
		(void) label_read(devl->dev, &label, UINT64_C(0));
		dm_list_del(&devl->list);
		dm_free(devl);
	}

	/* If vginfo changed, caller needs to rescan */
	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid_found)) ||
	    strncmp(vginfo->vgid, vgid_found, ID_LEN))
		return NULL;

out:
	return vginfo->fmt;
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
	strncpy(&id[0], vgid, ID_LEN);
	id[ID_LEN] = '\0';

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

static int _info_is_valid(struct lvmcache_info *info)
{
	if (info->status & CACHE_INVALID)
		return 0;

	/*
	 * The caller must hold the VG lock to manipulate metadata.
	 * In a cluster, remote nodes sometimes read metadata in the
	 * knowledge that the controlling node is holding the lock.
	 * So if the VG appears to be unlocked here, it should be safe
	 * to use the cached value.
	 */
	if (info->vginfo && !lvmcache_vgname_is_locked(info->vginfo->vgname))
		return 1;

	if (!(info->status & CACHE_LOCKED))
		return 0;

	return 1;
}

static int _vginfo_is_valid(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_info *info;

	/* Invalid if any info is invalid */
	dm_list_iterate_items(info, &vginfo->infos)
		if (!_info_is_valid(info))
			return 0;

	return 1;
}

/* vginfo is invalid if it does not contain at least one valid info */
static int _vginfo_is_invalid(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_info *info;

	dm_list_iterate_items(info, &vginfo->infos)
		if (_info_is_valid(info))
			return 0;

	return 1;
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

	strncpy(&id[0], pvid, ID_LEN);
	id[ID_LEN] = '\0';

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

	if (valid_only && !_info_is_valid(info))
		return NULL;

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

char *lvmcache_vgname_from_pvid(struct cmd_context *cmd, const char *pvid)
{
	struct lvmcache_info *info;
	char *vgname;

	if (!lvmcache_device_from_pvid(cmd, (const struct id *)pvid, NULL, NULL)) {
		log_error("Couldn't find device with uuid %s.", pvid);
		return NULL;
	}

	info = lvmcache_info_from_pvid(pvid, NULL, 0);
	if (!info)
		return_NULL;

	if (!(vgname = dm_pool_strdup(cmd->mem, info->vginfo->vgname))) {
		log_errno(ENOMEM, "vgname allocation failed");
		return NULL;
	}
	return vgname;
}

static void _rescan_entry(struct lvmcache_info *info)
{
	struct label *label;

	if (info->status & CACHE_INVALID)
		(void) label_read(info->dev, &label, UINT64_C(0));
}

static int _scan_invalid(void)
{
	dm_hash_iter(_pvid_hash, (dm_hash_iterate_fn) _rescan_entry);

	return 1;
}

/*
 * lvmcache_label_scan() remembers that it has already
 * been called, and will not scan labels if it's called
 * again.  (It will rescan "INVALID" devices if called again.)
 *
 * To force lvmcache_label_scan() to rescan labels on all devices,
 * call lvmcache_force_next_label_scan() before calling
 * lvmcache_label_scan().
 */

static int _force_label_scan;

void lvmcache_force_next_label_scan(void)
{
	_force_label_scan = 1;
}

/*
 * Check if any PVs in vg->pvs have the same PVID as any
 * entries in _unused_duplicate_devices.
 */

int vg_has_duplicate_pvs(struct volume_group *vg)
{
	struct pv_list *pvl;
	struct device_list *devl;

	dm_list_iterate_items(pvl, &vg->pvs) {
		dm_list_iterate_items(devl, &_unused_duplicate_devs) {
			if (id_equal(&pvl->pv->id, (const struct id *)devl->dev->pvid))
				return 1;
		}
	}
	return 0;
}

static int _dev_in_device_list(struct device *dev, struct dm_list *head)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, head) {
		if (devl->dev == dev)
			return 1;
	}
	return 0;
}

int lvmcache_dev_is_unchosen_duplicate(struct device *dev)
{
	return _dev_in_device_list(dev, &_unused_duplicate_devs);
}

/*
 * Compare _found_duplicate_devs entries with the corresponding duplicate dev
 * in lvmcache.  There may be multiple duplicates in _found_duplicate_devs for
 * a given pvid.  If a dev from _found_duplicate_devs is preferred over the dev
 * in lvmcache, then drop the dev in lvmcache and rescan the preferred dev to
 * add it to lvmcache.
 *
 * _found_duplicate_devs: duplicate devs found during initial scan.
 * These are compared to lvmcache devs to see if any are preferred.
 *
 * _unused_duplicate_devs: duplicate devs not chosen to be used.
 * These are _found_duplicate_devs entries that were not chosen,
 * or unpreferred lvmcache devs that were dropped.
 *
 * del_cache_devs: devices to drop from lvmcache
 * add_cache_devs: devices to scan to add to lvmcache
 */

static void _choose_preferred_devs(struct cmd_context *cmd,
				   struct dm_list *del_cache_devs,
				   struct dm_list *add_cache_devs)
{
	char uuid[64] __attribute__((aligned(8)));
	const char *reason;
	struct dm_list altdevs;
	struct dm_list new_unused;
	struct dev_types *dt = cmd->dev_types;
	struct device_list *devl, *devl_safe, *alt, *del;
	struct lvmcache_info *info;
	struct device *dev1, *dev2;
	uint32_t dev1_major, dev1_minor, dev2_major, dev2_minor;
	uint64_t info_size, dev1_size, dev2_size;
	int in_subsys1, in_subsys2;
	int is_dm1, is_dm2;
	int has_fs1, has_fs2;
	int has_lv1, has_lv2;
	int same_size1, same_size2;
	int prev_unchosen1, prev_unchosen2;
	int change;

	dm_list_init(&new_unused);

	/*
	 * Create a list of all alternate devs for the same pvid: altdevs.
	 */
next:
	dm_list_init(&altdevs);
	alt = NULL;

	dm_list_iterate_items_safe(devl, devl_safe, &_found_duplicate_devs) {
		if (!alt) {
			dm_list_move(&altdevs, &devl->list);
			alt = devl;
		} else {
			if (!strcmp(alt->dev->pvid, devl->dev->pvid))
				dm_list_move(&altdevs, &devl->list);
		}
	}

	if (!alt) {
		_destroy_duplicate_device_list(&_unused_duplicate_devs);
		dm_list_splice(&_unused_duplicate_devs, &new_unused);
		return;
	}

	/*
	 * Find the device for the pvid that's currently in lvmcache.
	 */

	if (!(info = lvmcache_info_from_pvid(alt->dev->pvid, NULL, 0))) {
		/* This shouldn't happen */
		log_warn("WARNING: PV %s on duplicate device %s not found in cache.",
			 alt->dev->pvid, dev_name(alt->dev));
		goto next;
	}

	/*
	 * Compare devices for the given pvid to find one that's preferred.
	 * "dev1" is the currently preferred device, starting with the device
	 * currently in lvmcache.
	 */

	dev1 = info->dev;

	dm_list_iterate_items(devl, &altdevs) {
		dev2 = devl->dev;

		if (dev1 == dev2) {
			/* This shouldn't happen */
			log_warn("Same duplicate device repeated %s", dev_name(dev1));
			continue;
		}

		prev_unchosen1 = _dev_in_device_list(dev1, &_unused_duplicate_devs);
		prev_unchosen2 = _dev_in_device_list(dev2, &_unused_duplicate_devs);

		if (!prev_unchosen1 && !prev_unchosen2) {
			/*
			 * The cmd list saves the unchosen preference across
			 * lvmcache_destroy.  Sometimes a single command will
			 * fill lvmcache, destroy it, and refill it, and we
			 * want the same duplicate preference to be preserved
			 * in each instance of lvmcache for a single command.
			 */
			prev_unchosen1 = _dev_in_device_list(dev1, &cmd->unused_duplicate_devs);
			prev_unchosen2 = _dev_in_device_list(dev2, &cmd->unused_duplicate_devs);
		}

		dev1_major = MAJOR(dev1->dev);
		dev1_minor = MINOR(dev1->dev);
		dev2_major = MAJOR(dev2->dev);
		dev2_minor = MINOR(dev2->dev);

		if (!dev_get_size(dev1, &dev1_size))
			dev1_size = 0;
		if (!dev_get_size(dev2, &dev2_size))
			dev2_size = 0;

		has_lv1 = (dev1->flags & DEV_USED_FOR_LV) ? 1 : 0;
		has_lv2 = (dev2->flags & DEV_USED_FOR_LV) ? 1 : 0;

		in_subsys1 = dev_subsystem_part_major(dt, dev1);
		in_subsys2 = dev_subsystem_part_major(dt, dev2);

		is_dm1 = dm_is_dm_major(dev1_major);
		is_dm2 = dm_is_dm_major(dev2_major);

		has_fs1 = dm_device_has_mounted_fs(dev1_major, dev1_minor);
		has_fs2 = dm_device_has_mounted_fs(dev2_major, dev2_minor);

		info_size = info->device_size >> SECTOR_SHIFT;
		same_size1 = (dev1_size == info_size);
		same_size2 = (dev2_size == info_size);

		log_debug_cache("PV %s compare duplicates: %s %u:%u. %s %u:%u.",
				devl->dev->pvid,
				dev_name(dev1), dev1_major, dev1_minor,
				dev_name(dev2), dev2_major, dev2_minor);

		log_debug_cache("PV %s: wants size %llu. %s is %llu. %s is %llu.",
				devl->dev->pvid,
				(unsigned long long)info_size,
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

		if (change) {
			dev1 = dev2;
			alt = devl;
		}

		if (!id_write_format((const struct id *)dev1->pvid, uuid, sizeof(uuid)))
			stack;
		log_warn("WARNING: PV %s prefers device %s because %s.", uuid, dev_name(dev1), reason);
	}

	if (dev1 != info->dev) {
		log_debug_cache("PV %s: switching to device %s instead of device %s.",
				 dev1->pvid, dev_name(dev1), dev_name(info->dev));
		/*
		 * Move the preferred device from altdevs to add_cache_devs.
		 * Create a del_cache_devs entry for the current lvmcache
		 * device to drop.
		 */

		dm_list_move(add_cache_devs, &alt->list);

		if ((del = dm_zalloc(sizeof(*del)))) {
			del->dev = info->dev;
			dm_list_add(del_cache_devs, &del->list);
		}

	} else {
		log_debug_cache("PV %s: keeping current device %s.", dev1->pvid, dev_name(info->dev));
	}

	/*
	 * alt devs not chosen are moved to _unused_duplicate_devs.
	 * del_cache_devs being dropped are moved to _unused_duplicate_devs
	 * after being dropped.  So, _unused_duplicate_devs represents all
	 * duplicates not being used in lvmcache.
	 */

	dm_list_splice(&new_unused, &altdevs);

	goto next;
}

int lvmcache_label_scan(struct cmd_context *cmd)
{
	struct dm_list del_cache_devs;
	struct dm_list add_cache_devs;
	struct lvmcache_info *info;
	struct device_list *devl;
	struct label *label;
	struct dev_iter *iter;
	struct device *dev;
	struct format_type *fmt;
	int dev_count = 0;

	int r = 0;

	if (lvmetad_used())
		return 1;

	/* Avoid recursion when a PVID can't be found! */
	if (_scanning_in_progress)
		return 0;

	_scanning_in_progress = 1;

	if (!_vgname_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		goto out;
	}

	if (_has_scanned && !_force_label_scan) {
		r = _scan_invalid();
		goto out;
	}

	if (_force_label_scan && (cmd->full_filter && !cmd->full_filter->use_count) && !refresh_filters(cmd))
		goto_out;

	if (!cmd->full_filter || !(iter = dev_iter_create(cmd->full_filter, _force_label_scan))) {
		log_error("dev_iter creation failed");
		goto out;
	}

	log_very_verbose("Scanning device labels");

	/*
	 * Duplicates found during this label scan are added to _found_duplicate_devs().
	 */
	_destroy_duplicate_device_list(&_found_duplicate_devs);

	while ((dev = dev_iter_get(iter))) {
		(void) label_read(dev, &label, UINT64_C(0));
		dev_count++;
	}

	dev_iter_destroy(iter);

	log_very_verbose("Scanned %d device labels", dev_count);

	/*
	 * _choose_preferred_devs() returns:
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
	 * del_cache_devs onto _unused_duplicate_devs.
	 */

	if (!dm_list_empty(&_found_duplicate_devs)) {
		dm_list_init(&del_cache_devs);
		dm_list_init(&add_cache_devs);

		_choose_preferred_devs(cmd, &del_cache_devs, &add_cache_devs);

		dm_list_iterate_items(devl, &del_cache_devs) {
			log_debug_cache("Drop duplicate device %s in lvmcache", dev_name(devl->dev));
			if ((info = lvmcache_info_from_pvid(devl->dev->pvid, NULL, 0)))
				lvmcache_del(info);
		}

		dm_list_iterate_items(devl, &add_cache_devs) {
			log_debug_cache("Rescan preferred device %s for lvmcache", dev_name(devl->dev));
			(void) label_read(devl->dev, &label, UINT64_C(0));
		}

		dm_list_splice(&_unused_duplicate_devs, &del_cache_devs);
	}

	_has_scanned = 1;

	/* Perform any format-specific scanning e.g. text files */
	if (cmd->independent_metadata_areas)
		dm_list_iterate_items(fmt, &cmd->formats)
			if (fmt->ops->scan && !fmt->ops->scan(fmt, NULL))
				goto out;

	/*
	 * If we are a long-lived process, write out the updated persistent
	 * device cache for the benefit of short-lived processes.
	 */
	if (_force_label_scan && cmd->is_long_lived &&
	    cmd->dump_filter && cmd->full_filter && cmd->full_filter->dump &&
	    !cmd->full_filter->dump(cmd->full_filter, 0))
		stack;

	r = 1;

      out:
	_scanning_in_progress = 0;
	_force_label_scan = 0;

	return r;
}

struct volume_group *lvmcache_get_vg(struct cmd_context *cmd, const char *vgname,
				     const char *vgid, unsigned precommitted)
{
	struct lvmcache_vginfo *vginfo;
	struct volume_group *vg = NULL;
	struct format_instance *fid;
	struct format_instance_ctx fic;

	/*
	 * We currently do not store precommitted metadata in lvmetad at
	 * all. This means that any request for precommitted metadata is served
	 * using the classic scanning mechanics, and read from disk or from
	 * lvmcache.
	 */
	if (lvmetad_used() && !precommitted) {
		/* Still serve the locally cached VG if available */
		if (vgid && (vginfo = lvmcache_vginfo_from_vgid(vgid)) &&
		    vginfo->vgmetadata && (vg = vginfo->cached_vg))
			goto out;
		return lvmetad_vg_lookup(cmd, vgname, vgid);
	}

	if (!vgid || !(vginfo = lvmcache_vginfo_from_vgid(vgid)) || !vginfo->vgmetadata)
		return NULL;

	if (!_vginfo_is_valid(vginfo))
		return NULL;

	/*
	 * Don't return cached data if either:
	 * (i)  precommitted metadata is requested but we don't have it cached
	 *      - caller should read it off disk;
	 * (ii) live metadata is requested but we have precommitted metadata cached
	 *      and no devices are suspended so caller may read it off disk.
	 *
	 * If live metadata is requested but we have precommitted metadata cached
	 * and devices are suspended, we assume this precommitted metadata has
	 * already been preloaded and committed so it's OK to return it as live.
	 * Note that we do not clear the PRECOMMITTED flag.
	 */
	if ((precommitted && !vginfo->precommitted) ||
	    (!precommitted && vginfo->precommitted && !critical_section()))
		return NULL;

	/* Use already-cached VG struct when available */
	if ((vg = vginfo->cached_vg) && !vginfo->cached_vg_invalidated)
		goto out;

	release_vg(vginfo->cached_vg);

	fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = vginfo->vgname;
	fic.context.vg_ref.vg_id = vgid;
	if (!(fid = vginfo->fmt->ops->create_instance(vginfo->fmt, &fic)))
		return_NULL;

	/* Build config tree from vgmetadata, if not yet cached */
	if (!vginfo->cft &&
	    !(vginfo->cft =
	      config_tree_from_string_without_dup_node_check(vginfo->vgmetadata)))
		goto_bad;

	if (!(vg = import_vg_from_config_tree(vginfo->cft, fid)))
		goto_bad;

	/* Cache VG struct for reuse */
	vginfo->cached_vg = vg;
	vginfo->holders = 1;
	vginfo->vg_use_count = 0;
	vginfo->cached_vg_invalidated = 0;
	vg->vginfo = vginfo;

	if (!dm_pool_lock(vg->vgmem, detect_internal_vg_cache_corruption()))
		goto_bad;

out:
	vginfo->holders++;
	vginfo->vg_use_count++;
	log_debug_cache("Using cached %smetadata for VG %s with %u holder(s).",
			vginfo->precommitted ? "pre-committed " : "",
			vginfo->vgname, vginfo->holders);

	return vg;

bad:
	_free_cached_vgmetadata(vginfo);
	return NULL;
}

// #if 0
int lvmcache_vginfo_holders_dec_and_test_for_zero(struct lvmcache_vginfo *vginfo)
{
	log_debug_cache("VG %s decrementing %d holder(s) at %p.",
			vginfo->cached_vg->name, vginfo->holders, vginfo->cached_vg);

	if (--vginfo->holders)
		return 0;

	if (vginfo->vg_use_count > 1)
		log_debug_cache("VG %s reused %d times.",
				vginfo->cached_vg->name, vginfo->vg_use_count);

	/* Debug perform crc check only when it's been used more then once */
	if (!dm_pool_unlock(vginfo->cached_vg->vgmem,
			    detect_internal_vg_cache_corruption() &&
			    (vginfo->vg_use_count > 1)))
		stack;

	vginfo->cached_vg->vginfo = NULL;
	vginfo->cached_vg = NULL;

	return 1;
}
// #endif

int lvmcache_get_vgnameids(struct cmd_context *cmd, int include_internal,
			   struct dm_list *vgnameids)
{
	struct vgnameid_list *vgnl;
	struct lvmcache_vginfo *vginfo;

	lvmcache_label_scan(cmd);

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

struct dm_list *lvmcache_get_vgids(struct cmd_context *cmd,
				   int include_internal)
{
	struct dm_list *vgids;
	struct lvmcache_vginfo *vginfo;

	// TODO plug into lvmetad here automagically?
	lvmcache_label_scan(cmd);

	if (!(vgids = str_list_create(cmd->mem))) {
		log_error("vgids list allocation failed");
		return NULL;
	}

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (!include_internal && is_orphan_vg(vginfo->vgname))
			continue;

		if (!str_list_add(cmd->mem, vgids,
				  dm_pool_strdup(cmd->mem, vginfo->vgid))) {
			log_error("strlist allocation failed");
			return NULL;
		}
	}

	return vgids;
}

struct dm_list *lvmcache_get_vgnames(struct cmd_context *cmd,
				     int include_internal)
{
	struct dm_list *vgnames;
	struct lvmcache_vginfo *vginfo;

	lvmcache_label_scan(cmd);

	if (!(vgnames = str_list_create(cmd->mem))) {
		log_errno(ENOMEM, "vgnames list allocation failed");
		return NULL;
	}

	dm_list_iterate_items(vginfo, &_vginfos) {
		if (!include_internal && is_orphan_vg(vginfo->vgname))
			continue;

		if (!str_list_add(cmd->mem, vgnames,
				  dm_pool_strdup(cmd->mem, vginfo->vgname))) {
			log_errno(ENOMEM, "strlist allocation failed");
			return NULL;
		}
	}

	return vgnames;
}

struct dm_list *lvmcache_get_pvids(struct cmd_context *cmd, const char *vgname,
				const char *vgid)
{
	struct dm_list *pvids;
	struct lvmcache_vginfo *vginfo;
	struct lvmcache_info *info;

	if (!(pvids = str_list_create(cmd->mem))) {
		log_error("pvids list allocation failed");
		return NULL;
	}

	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid)))
		return pvids;

	dm_list_iterate_items(info, &vginfo->infos) {
		if (!str_list_add(cmd->mem, pvids,
				  dm_pool_strdup(cmd->mem, info->dev->pvid))) {
			log_error("strlist allocation failed");
			return NULL;
		}
	}

	return pvids;
}

static struct device *_device_from_pvid(const struct id *pvid,
					uint64_t *label_sector)
{
	struct lvmcache_info *info;
	struct label *label;

	if ((info = lvmcache_info_from_pvid((const char *) pvid, NULL, 0))) {
		if (lvmetad_used()) {
			if (info->label && label_sector)
				*label_sector = info->label->sector;
			return info->dev;
		}

		if (label_read(info->dev, &label, UINT64_C(0))) {
			info = (struct lvmcache_info *) label->info;
			if (id_equal(pvid, (struct id *) &info->dev->pvid)) {
				if (label_sector)
					*label_sector = label->sector;
				return info->dev;
                        }
		}
	}
	return NULL;
}

struct device *lvmcache_device_from_pvid(struct cmd_context *cmd, const struct id *pvid,
				unsigned *scan_done_once, uint64_t *label_sector)
{
	struct device *dev;

	/* Already cached ? */
	dev = _device_from_pvid(pvid, label_sector);
	if (dev)
		return dev;

	lvmcache_label_scan(cmd);

	/* Try again */
	dev = _device_from_pvid(pvid, label_sector);
	if (dev)
		return dev;

	if (critical_section() || (scan_done_once && *scan_done_once))
		return NULL;

	lvmcache_force_next_label_scan();
	lvmcache_label_scan(cmd);
	if (scan_done_once)
		*scan_done_once = 1;

	/* Try again */
	dev = _device_from_pvid(pvid, label_sector);
	if (dev)
		return dev;

	return NULL;
}

const char *lvmcache_pvid_from_devname(struct cmd_context *cmd,
				       const char *devname)
{
	struct device *dev;
	struct label *label;

	if (!(dev = dev_cache_get(devname, cmd->filter))) {
		log_error("%s: Couldn't find device.  Check your filters?",
			  devname);
		return NULL;
	}

	if (!(label_read(dev, &label, UINT64_C(0))))
		return NULL;

	return dev->pvid;
}

int lvmcache_pvid_in_unchosen_duplicates(const char *pvid)
{
	struct device_list *devl;

	dm_list_iterate_items(devl, &_unused_duplicate_devs) {
		if (!strncmp(devl->dev->pvid, pvid, ID_LEN))
			return 1;
	}
	return 0;
}

static int _free_vginfo(struct lvmcache_vginfo *vginfo)
{
	struct lvmcache_vginfo *primary_vginfo, *vginfo2;
	int r = 1;

	_free_cached_vgmetadata(vginfo);

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

	dm_free(vginfo->system_id);
	dm_free(vginfo->vgname);
	dm_free(vginfo->creation_host);

	if (*vginfo->vgid && _vgid_hash &&
	    lvmcache_vginfo_from_vgid(vginfo->vgid) == vginfo)
		dm_hash_remove(_vgid_hash, vginfo->vgid);

	dm_list_del(&vginfo->list);

	dm_free(vginfo);

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
	dm_free(info);
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

	strncpy(vginfo->vgid, vgid, ID_LEN);
	vginfo->vgid[ID_LEN] = '\0';
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
	struct lvmcache_vginfo *vginfo, *primary_vginfo, *orphan_vginfo;
	struct lvmcache_info *info2, *info3;
	char mdabuf[32];
	// struct lvmcache_vginfo  *old_vginfo, *next;

	if (!vgname || (info && info->vginfo && !strcmp(info->vginfo->vgname, vgname)))
		return 1;

	/* Remove existing vginfo entry */
	if (info)
		_drop_vginfo(info, info->vginfo);

	/* Get existing vginfo or create new one */
	if (!(vginfo = lvmcache_vginfo_from_vgname(vgname, vgid))) {
/*** FIXME - vginfo ends up duplicated instead of renamed.
		// Renaming?  This lookup fails.
		if ((vginfo = vginfo_from_vgid(vgid))) {
			next = vginfo->next;
			old_vginfo = vginfo_from_vgname(vginfo->vgname, NULL);
			if (old_vginfo == vginfo) {
				dm_hash_remove(_vgname_hash, old_vginfo->vgname);
				if (old_vginfo->next) {
					if (!dm_hash_insert(_vgname_hash, old_vginfo->vgname, old_vginfo->next)) {
						log_error("vg hash re-insertion failed: %s",
							  old_vginfo->vgname);
						return 0;
					}
				}
			} else do {
				if (old_vginfo->next == vginfo) {
					old_vginfo->next = vginfo->next;
					break;
				}
			} while ((old_vginfo = old_vginfo->next));
			vginfo->next = NULL;

			dm_free(vginfo->vgname);
			if (!(vginfo->vgname = dm_strdup(vgname))) {
				log_error("cache vgname alloc failed for %s", vgname);
				return 0;
			}

			// Rename so can assume new name does not already exist
			if (!dm_hash_insert(_vgname_hash, vginfo->vgname, vginfo->next)) {
				log_error("vg hash re-insertion failed: %s",
					  vginfo->vgname);
		      		return 0;
			}
		} else {
***/
		if (!(vginfo = dm_zalloc(sizeof(*vginfo)))) {
			log_error("lvmcache_update_vgname: list alloc failed");
			return 0;
		}
		if (!(vginfo->vgname = dm_strdup(vgname))) {
			dm_free(vginfo);
			log_error("cache vgname alloc failed for %s", vgname);
			return 0;
		}
		dm_list_init(&vginfo->infos);

		/*
		 * If we're scanning and there's an invalidated entry, remove it.
		 * Otherwise we risk bogus warnings of duplicate VGs.
		 */
		while ((primary_vginfo = lvmcache_vginfo_from_vgname(vgname, NULL)) &&
		       _scanning_in_progress && _vginfo_is_invalid(primary_vginfo)) {
			orphan_vginfo = lvmcache_vginfo_from_vgname(primary_vginfo->fmt->orphan_vg_name, NULL);
			if (!orphan_vginfo) {
				log_error(INTERNAL_ERROR "Orphan vginfo %s lost from cache.",
					  primary_vginfo->fmt->orphan_vg_name);
				dm_free(vginfo->vgname);
				dm_free(vginfo);
				return 0;
			}
			dm_list_iterate_items_safe(info2, info3, &primary_vginfo->infos) {
				_vginfo_detach_info(info2);
				_vginfo_attach_info(orphan_vginfo, info2);
				if (info2->mdas.n)
					sprintf(mdabuf, " with %u mdas",
						dm_list_size(&info2->mdas));
				else
					mdabuf[0] = '\0';
				log_debug_cache("lvmcache: %s: now in VG %s%s%s%s%s",
						dev_name(info2->dev),
						vgname, orphan_vginfo->vgid[0] ? " (" : "",
						orphan_vginfo->vgid[0] ? orphan_vginfo->vgid : "",
						orphan_vginfo->vgid[0] ? ")" : "", mdabuf);
			}

			if (!_drop_vginfo(NULL, primary_vginfo))
				return_0;
		}

		if (!_insert_vginfo(vginfo, vgid, vgstatus, creation_host,
				    primary_vginfo)) {
			dm_free(vginfo->vgname);
			dm_free(vginfo);
			return 0;
		}
		/* Ensure orphans appear last on list_iterate */
		if (is_orphan_vg(vgname))
			dm_list_add(&_vginfos, &vginfo->list);
		else
			dm_list_add_h(&_vginfos, &vginfo->list);
/***
		}
***/
	}

	if (info)
		_vginfo_attach_info(vginfo, info);
	else if (!_lvmcache_update_vgid(NULL, vginfo, vgid)) /* Orphans */
		return_0;

	_update_cache_vginfo_lock_state(vginfo, lvmcache_vgname_is_locked(vgname));

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

	dm_free(info->vginfo->creation_host);

	if (!(info->vginfo->creation_host = dm_strdup(creation_host))) {
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

	dm_free(info->vginfo->lock_type);

	if (!(info->vginfo->lock_type = dm_strdup(lock_type))) {
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

	dm_free(info->vginfo->system_id);

	if (!(info->vginfo->system_id = dm_strdup(system_id))) {
		log_error("cache system_id alloc failed for %s", system_id);
		return 0;
	}

	log_debug_cache("lvmcache %s: VG %s: set system_id to %s.",
			dev_name(info->dev), info->vginfo->vgname, system_id);

out:
	return 1;
}

static int _lvmcache_update_vg_mda_info(struct lvmcache_info *info, uint32_t mda_checksum,
					size_t mda_size)
{
	if (!info || !info->vginfo || !mda_size)
		return 1;

	if (info->vginfo->mda_checksum == mda_checksum || info->vginfo->mda_size == mda_size) 
		return 1;

	info->vginfo->mda_checksum = mda_checksum;
	info->vginfo->mda_size = mda_size;

	/* FIXME Add checksum index */

	log_debug_cache("lvmcache %s: VG %s: stored metadata checksum 0x%08"
			PRIx32 " with size %" PRIsize_t ".",
			dev_name(info->dev), info->vginfo->vgname,
			mda_checksum, mda_size);

	return 1;
}

int lvmcache_add_orphan_vginfo(const char *vgname, struct format_type *fmt)
{
	if (!_lock_hash && !lvmcache_init()) {
		log_error("Internal cache initialisation failed");
		return 0;
	}

	return _lvmcache_update_vgname(NULL, vgname, vgname, 0, "", fmt);
}

int lvmcache_update_vgname_and_id(struct lvmcache_info *info, struct lvmcache_vgsummary *vgsummary)
{
	const char *vgname = vgsummary->vgname;
	const char *vgid = (char *)&vgsummary->vgid;

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

	/* If making a PV into an orphan, any cached VG metadata may become
	 * invalid, incorrectly still referencing device structs.
	 * (Example: pvcreate -ff) */
	if (is_orphan_vg(vgname) && info->vginfo && !is_orphan_vg(info->vginfo->vgname))
		info->vginfo->cached_vg_invalidated = 1;

	/* If moving PV from orphan to real VG, always mark it valid */
	if (!is_orphan_vg(vgname))
		info->status &= ~CACHE_INVALID;

	if (!_lvmcache_update_vgname(info, vgname, vgid, vgsummary->vgstatus,
				     vgsummary->creation_host, info->fmt) ||
	    !_lvmcache_update_vgid(info, info->vginfo, vgid) ||
	    !_lvmcache_update_vgstatus(info, vgsummary->vgstatus, vgsummary->creation_host, vgsummary->lock_type, vgsummary->system_id) ||
	    !_lvmcache_update_vg_mda_info(info, vgsummary->mda_checksum, vgsummary->mda_size))
		return_0;

	return 1;
}

int lvmcache_update_vg(struct volume_group *vg, unsigned precommitted)
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

	pvid_s[sizeof(pvid_s) - 1] = '\0';

	dm_list_iterate_items(pvl, &vg->pvs) {
		strncpy(pvid_s, (char *) &pvl->pv->id, sizeof(pvid_s) - 1);
		/* FIXME Could pvl->pv->dev->pvid ever be different? */
		if ((info = lvmcache_info_from_pvid(pvid_s, pvl->pv->dev, 0)) &&
		    !lvmcache_update_vgname_and_id(info, &vgsummary))
			return_0;
	}

	/* store text representation of vg to cache */
	if (vg->cmd->current_settings.cache_vgmetadata)
		_store_metadata(vg, precommitted);

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

static struct lvmcache_info * _create_info(struct labeller *labeller, struct device *dev)
{
	struct lvmcache_info *info;
	struct label *label;

	if (!(label = label_create(labeller)))
		return_NULL;
	if (!(info = dm_zalloc(sizeof(*info)))) {
		log_error("lvmcache_info allocation failed");
		label_destroy(label);
		return NULL;
	}

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
				   const char *pvid, struct device *dev,
				   const char *vgname, const char *vgid, uint32_t vgstatus)
{
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	char uuid[64] __attribute__((aligned(8)));
	struct lvmcache_vgsummary vgsummary = { 0 };
	struct lvmcache_info *info;
	struct lvmcache_info *info_lookup;
	struct device_list *devl;
	int created = 0;

	strncpy(pvid_s, pvid, sizeof(pvid_s) - 1);
	pvid_s[sizeof(pvid_s) - 1] = '\0';
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
		info = _create_info(labeller, dev);
		created = 1;
	}

	if (!info)
		return_NULL;

	/*
	 * If an existing info struct was found, check if any values are new.
	 */
	if (!created) {
		if (info->dev != dev) {
			log_warn("WARNING: PV %s on %s was already found on %s.",
				  uuid, dev_name(dev), dev_name(info->dev));

			if (!_found_duplicate_pvs && lvmetad_used()) {
				log_warn("WARNING: Disabling lvmetad cache which does not support duplicate PVs."); 
				lvmetad_set_disabled(labeller->fmt->cmd, LVMETAD_DISABLE_REASON_DUPLICATES);
			}
			_found_duplicate_pvs = 1;

			strncpy(dev->pvid, pvid_s, sizeof(dev->pvid));

			/*
			 * Keep the existing PV/dev in lvmcache, and save the
			 * new duplicate in the list of duplicates.  After
			 * scanning is complete, compare the duplicate devs
			 * with those in lvmcache to check if one of the
			 * duplicates is preferred and if so switch lvmcache to
			 * use it.
			 */

			if (!(devl = dm_zalloc(sizeof(*devl))))
				return_NULL;
			devl->dev = dev;

			dm_list_add(&_found_duplicate_devs, &devl->list);
			return NULL;
		}

		if (info->dev->pvid[0] && pvid[0] && strcmp(pvid_s, info->dev->pvid)) {
			/* This happens when running pvcreate on an existing PV. */
			log_verbose("Changing pvid on dev %s from %s to %s",
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

	info->status |= CACHE_INVALID;

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
			dm_free(info->label);
			dm_free(info);
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
	dm_free(info);
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

static void _lvmcache_destroy_lockname(struct dm_hash_node *n)
{
	char *vgname;

	if (!dm_hash_get_data(_lock_hash, n))
		return;

	vgname = dm_hash_get_key(_lock_hash, n);

	if (!strcmp(vgname, VG_GLOBAL))
		_vg_global_lock_held = 1;
	else
		log_error(INTERNAL_ERROR "Volume Group %s was not unlocked",
			  dm_hash_get_key(_lock_hash, n));
}

void lvmcache_destroy(struct cmd_context *cmd, int retain_orphans, int reset)
{
	struct dm_hash_node *n;
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
		if (reset)
			_vg_global_lock_held = 0;
		else
			dm_hash_iterate(n, _lock_hash)
				_lvmcache_destroy_lockname(n);
		dm_hash_destroy(_lock_hash);
		_lock_hash = NULL;
	}

	if (!dm_list_empty(&_vginfos))
		log_error(INTERNAL_ERROR "_vginfos list should be empty");
	dm_list_init(&_vginfos);

	/*
	 * Copy the current _unused_duplicate_devs into a cmd list before
	 * destroying _unused_duplicate_devs.
	 *
	 * One command can init/populate/destroy lvmcache multiple times.  Each
	 * time it will encounter duplicates and choose the preferrred devs.
	 * We want the same preferred devices to be chosen each time, so save
	 * the unpreferred devs here so that _choose_preferred_devs can use
	 * this to make the same choice each time.
	 */
	dm_list_init(&cmd->unused_duplicate_devs);
	lvmcache_get_unused_duplicate_devs(cmd, &cmd->unused_duplicate_devs);
	_destroy_duplicate_device_list(&_unused_duplicate_devs);
	_destroy_duplicate_device_list(&_found_duplicate_devs); /* should be empty anyway */
	_found_duplicate_pvs = 0;

	if (retain_orphans)
		if (!init_lvmcache_orphans(cmd))
			stack;
}

int lvmcache_pvid_is_locked(const char *pvid) {
	struct lvmcache_info *info;
	info = lvmcache_info_from_pvid(pvid, NULL, 0);
	if (!info || !info->vginfo)
		return 0;

	return lvmcache_vgname_is_locked(info->vginfo->vgname);
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

int lvmcache_fid_add_mdas_vg(struct lvmcache_vginfo *vginfo, struct format_instance *fid)
{
	struct lvmcache_info *info;
	dm_list_iterate_items(info, &vginfo->infos) {
		if (!lvmcache_fid_add_mdas_pv(info, fid))
			return_0;
	}
	return 1;
}

static int _get_pv_if_in_vg(struct lvmcache_info *info,
			    struct physical_volume *pv)
{
	char vgname[NAME_LEN + 1];
	char vgid[ID_LEN + 1];

	if (info->vginfo && info->vginfo->vgname &&
	    !is_orphan_vg(info->vginfo->vgname)) {
		/*
		 * get_pv_from_vg_by_id() may call
		 * lvmcache_label_scan() and drop cached
		 * vginfo so make a local copy of string.
		 */
		(void) dm_strncpy(vgname, info->vginfo->vgname, sizeof(vgname));
		memcpy(vgid, info->vginfo->vgid, sizeof(vgid));

		if (get_pv_from_vg_by_id(info->fmt, vgname, vgid,
					 info->dev->pvid, pv))
			return 1;
	}

	return 0;
}

int lvmcache_populate_pv_fields(struct lvmcache_info *info,
				struct physical_volume *pv,
				int scan_label_only)
{
	struct data_area_list *da;

	/* Have we already cached vgname? */
	if (!scan_label_only && _get_pv_if_in_vg(info, pv))
		return 1;

	/* Perform full scan (just the first time) and try again */
	if (!scan_label_only && !critical_section() && !full_scan_done()) {
		lvmcache_force_next_label_scan();
		lvmcache_label_scan(info->fmt->cmd);

		if (_get_pv_if_in_vg(info, pv))
			return 1;
	}

	/* Orphan */
	pv->dev = info->dev;
	pv->fmt = info->fmt;
	pv->size = info->device_size >> SECTOR_SHIFT;
	pv->vg_name = FMT_TEXT_ORPHAN_VG_NAME;
	memcpy(&pv->id, &info->dev->pvid, sizeof(pv->id));

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
		     uint64_t start, uint64_t size, unsigned ignored)
{
	return add_mda(info->fmt, NULL, &info->mdas, dev, start, size, ignored);
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

/*
 * The lifetime of the label returned is tied to the lifetime of the
 * lvmcache_info which is the same as lvmcache itself.
 */
struct label *lvmcache_get_label(struct lvmcache_info *info) {
	return info->label;
}

void lvmcache_make_valid(struct lvmcache_info *info) {
	info->status &= ~CACHE_INVALID;
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

int lvmcache_is_orphan(struct lvmcache_info *info) {
	if (!info->vginfo)
		return 1; /* FIXME? */
	return is_orphan_vg(info->vginfo->vgname);
}

int lvmcache_vgid_is_cached(const char *vgid) {
	struct lvmcache_vginfo *vginfo;

	if (lvmetad_used())
		return 1;

	vginfo = lvmcache_vginfo_from_vgid(vgid);

	if (!vginfo || !vginfo->vgname)
		return 0;

	if (is_orphan_vg(vginfo->vgname))
		return 0;

	return 1;
}

/*
 * Return true iff it is impossible to find out from this info alone whether the
 * PV in question is or is not an orphan.
 */
int lvmcache_uncertain_ownership(struct lvmcache_info *info) {
	return mdas_empty_or_ignored(&info->mdas);
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

	if (lvmetad_used())
		return lvmetad_vg_is_foreign(cmd, vgname, vgid);

	if ((vginfo = lvmcache_vginfo_from_vgid(vgid)))
		ret = !is_system_id_allowed(cmd, vginfo->system_id);

	return ret;
}

