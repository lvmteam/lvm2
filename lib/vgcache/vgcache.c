/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include <stdlib.h>
#include "vgcache.h"
#include "hash.h"
#include "dbg_malloc.h"
#include "log.h"
#include "uuid.h"
#include "toolcontext.h"

static struct hash_table *_vghash;
static struct hash_table *_vgidhash;
static struct hash_table *_pvhash;

const char *all_devices = "\0";

int vgcache_init()
{
	if (!(_vghash = hash_create(128)))
		return 0;

	if (!(_vgidhash = hash_create(128)))
		return 0;

	if (!(_pvhash = hash_create(128)))
		return 0;

	return 1;
}

/* A vg_name of NULL returns all_devices */
struct list *vgcache_find(const char *vg_name)
{
	struct vgname_entry *vgn;

	if (!_vghash)
		return NULL;

	if (!vg_name)
		vg_name = all_devices;

	if (!(vgn = hash_lookup(_vghash, vg_name)))
		return NULL;

	return &vgn->pvdevs;
}

struct list *vgcache_find_by_vgid(const char *vgid)
{
	struct vgname_entry *vgn;
	char vgid_s[ID_LEN + 1];

	if (!_vgidhash || !vgid)
		return NULL;

	memcpy(vgid_s, vgid, ID_LEN);
	vgid_s[ID_LEN] = '\0';

	if (!(vgn = hash_lookup(_vgidhash, vgid_s)))
		return NULL;

	return &vgn->pvdevs;
}

void vgcache_del_orphan(struct device *dev)
{
	struct pvdev_list *pvdev;

	if (_pvhash && ((pvdev = hash_lookup(_pvhash, dev_name(dev))))) {
		list_del(&pvdev->list);
		hash_remove(_pvhash, dev_name(pvdev->dev));
		dbg_free(pvdev);
	}
}

int vgcache_add_entry(const char *vg_name, const char *vgid, struct device *dev)
{
	const char *pv_name;
	struct vgname_entry *vgn;
	struct pvdev_list *pvdev;
	struct list *pvdh, *pvdevs;

	if (!(pvdevs = vgcache_find(vg_name))) {
		if (!(vgn = dbg_malloc(sizeof(struct vgname_entry)))) {
			log_error("struct vgname_entry allocation failed");
			return 0;
		}
		memset(vgn, 0, sizeof(struct vgname_entry));

		pvdevs = &vgn->pvdevs;
		list_init(pvdevs);

		if (!(vgn->vgname = dbg_strdup(vg_name))) {
			log_error("vgcache_add: strdup vg_name failed");
			return 0;
		}

		if (!hash_insert(_vghash, vg_name, vgn)) {
			log_error("vgcache_add: VG hash insertion failed");
			return 0;
		}

		if (vgid) {
			memcpy(vgn->vgid, vgid, ID_LEN);
			vgn->vgid[ID_LEN] = '\0';

			if (!hash_insert(_vgidhash, vgn->vgid, vgn)) {
				log_error("vgcache_add: vgid hash insertion "
					  "failed");
				return 0;
			}
		}
	}

	list_iterate(pvdh, pvdevs) {
		pvdev = list_item(pvdh, struct pvdev_list);
		if (dev == pvdev->dev)
			return 1;
	}

	/* Remove PV from any existing VG unless an all_devices request */
	pvdev = NULL;
	pv_name = dev_name(dev);
	if (*vg_name && _pvhash && ((pvdev = hash_lookup(_pvhash, pv_name)))) {
		list_del(&pvdev->list);
		hash_remove(_pvhash, dev_name(pvdev->dev));
	}

	/* Allocate new pvdev_list if there isn't an existing one to reuse */
	if (!pvdev && !(pvdev = dbg_malloc(sizeof(struct pvdev_list)))) {
		log_error("struct pvdev_list allocation failed");
		return 0;
	}

	pvdev->dev = dev;
	list_add(pvdevs, &pvdev->list);

	if (*vg_name && _pvhash && !hash_insert(_pvhash, pv_name, pvdev)) {
		log_error("vgcache_add: PV hash insertion for %s "
			  "failed", pv_name);
		return 0;
	}

	return 1;
}

/* vg_name of "\0" is an orphan PV; NULL means only add to all_devices */
int vgcache_add(const char *vg_name, const char *vgid, struct device *dev)
{
	if (!_vghash && !vgcache_init())
		return 0;

	/* If orphan PV remove it */
	if (vg_name && !*vg_name)
		vgcache_del_orphan(dev);

	/* Add PV if vg_name supplied */
	if (vg_name && *vg_name && !vgcache_add_entry(vg_name, vgid, dev))
		return 0;

	/* Always add to all_devices */
	return vgcache_add_entry(all_devices, NULL, dev);
}

void vgcache_destroy_entry(struct vgname_entry *vgn)
{
	struct list *pvdh;
	struct pvdev_list *pvdev;

	if (vgn) {
		pvdh = vgn->pvdevs.n;
		while (pvdh != &vgn->pvdevs) {
			pvdev = list_item(pvdh, struct pvdev_list);
			pvdh = pvdh->n;
			if (_pvhash)
				hash_remove(_pvhash, dev_name(pvdev->dev));
			dbg_free(pvdev);
		}
		dbg_free(vgn->vgname);
		if (_vgidhash && vgn->vgid[0])
			hash_remove(_vgidhash, vgn->vgid);
	}
	dbg_free(vgn);
}

void vgcache_del(const char *vg_name)
{
	struct vgname_entry *vgn;

	if (!_vghash)
		return;

	if (!vg_name)
		vg_name = all_devices;

	if (!(vgn = hash_lookup(_vghash, vg_name)))
		return;

	hash_remove(_vghash, vg_name);
	if (vgn->vgid[0])
		hash_remove(_vgidhash, vgn->vgid);

	vgcache_destroy_entry(vgn);
}


void vgcache_del_by_vgid(const char *vgid)
{
	struct vgname_entry *vgn;
	char vgid_s[ID_LEN + 1];

	if (!_vgidhash || !vgid)
		return;

	memcpy(vgid_s, vgid, ID_LEN);
	vgid_s[ID_LEN] = '\0';

	if (!(vgn = hash_lookup(_vghash, vgid_s)))
		return;

	hash_remove(_vgidhash, vgn->vgid);
	if (vgn->vgname[0])
		hash_remove(_vghash, vgn->vgname);

	vgcache_destroy_entry(vgn);
}


void vgcache_destroy()
{
	if (_vghash) {
		hash_iter(_vghash, (iterate_fn)vgcache_destroy_entry);
		hash_destroy(_vghash);
		_vghash = NULL;
	}

	if (_vgidhash) {
		hash_destroy(_vgidhash);
		_vgidhash = NULL;
	}

	if (_pvhash) {
		hash_destroy(_pvhash);
		_pvhash = NULL;
	}
}

char *vgname_from_vgid(struct cmd_context *cmd, struct id *vgid)
{
	struct vgname_entry *vgn;
	char vgid_s[ID_LEN + 1];

	if (!_vgidhash || !vgid)
		return NULL;

	memcpy(vgid_s, vgid->uuid, ID_LEN);
	vgid_s[ID_LEN] = '\0';

	if (!(vgn = hash_lookup(_vgidhash, vgid_s)))
		return NULL;

	return pool_strdup(cmd->mem, vgn->vgname);
}

