/*
 * Copyright (C) 2005-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "dmlib.h"
#include "libdm-targets.h"
#include "libdm-common.h"
#include "kdev_t.h"
#include "dm-ioctl.h"

#include <stdarg.h>
#include <sys/param.h>
#include <sys/utsname.h>

#define MAX_TARGET_PARAMSIZE 500000

#define REPLICATOR_LOCAL_SITE 0

/* Supported segment types */
enum {
	SEG_CRYPT,
	SEG_ERROR,
	SEG_LINEAR,
	SEG_MIRRORED,
	SEG_REPLICATOR,
	SEG_REPLICATOR_DEV,
	SEG_SNAPSHOT,
	SEG_SNAPSHOT_ORIGIN,
	SEG_SNAPSHOT_MERGE,
	SEG_STRIPED,
	SEG_ZERO,
	SEG_THIN_POOL,
	SEG_THIN,
	SEG_RAID1,
	SEG_RAID4,
	SEG_RAID5_LA,
	SEG_RAID5_RA,
	SEG_RAID5_LS,
	SEG_RAID5_RS,
	SEG_RAID6_ZR,
	SEG_RAID6_NR,
	SEG_RAID6_NC,
	SEG_LAST,
};

/* FIXME Add crypt and multipath support */

struct {
	unsigned type;
	const char *target;
} dm_segtypes[] = {
	{ SEG_CRYPT, "crypt" },
	{ SEG_ERROR, "error" },
	{ SEG_LINEAR, "linear" },
	{ SEG_MIRRORED, "mirror" },
	{ SEG_REPLICATOR, "replicator" },
	{ SEG_REPLICATOR_DEV, "replicator-dev" },
	{ SEG_SNAPSHOT, "snapshot" },
	{ SEG_SNAPSHOT_ORIGIN, "snapshot-origin" },
	{ SEG_SNAPSHOT_MERGE, "snapshot-merge" },
	{ SEG_STRIPED, "striped" },
	{ SEG_ZERO, "zero"},
	{ SEG_THIN_POOL, "thin-pool"},
	{ SEG_THIN, "thin"},
	{ SEG_RAID1, "raid1"},
	{ SEG_RAID4, "raid4"},
	{ SEG_RAID5_LA, "raid5_la"},
	{ SEG_RAID5_RA, "raid5_ra"},
	{ SEG_RAID5_LS, "raid5_ls"},
	{ SEG_RAID5_RS, "raid5_rs"},
	{ SEG_RAID6_ZR, "raid6_zr"},
	{ SEG_RAID6_NR, "raid6_nr"},
	{ SEG_RAID6_NC, "raid6_nc"},

	/*
	 *WARNING: Since 'raid' target overloads this 1:1 mapping table
	 * for search do not add new enum elements past them!
	 */
	{ SEG_RAID5_LS, "raid5"}, /* same as "raid5_ls" (default for MD also) */
	{ SEG_RAID6_ZR, "raid6"}, /* same as "raid6_zr" */
	{ SEG_LAST, NULL },
};

/* Some segment types have a list of areas of other devices attached */
struct seg_area {
	struct dm_list list;

	struct dm_tree_node *dev_node;

	uint64_t offset;

	unsigned rsite_index;		/* Replicator site index */
	struct dm_tree_node *slog;	/* Replicator sync log node */
	uint64_t region_size;		/* Replicator sync log size */
	uint32_t flags;			/* Replicator sync log flags */
};

struct dm_thin_message {
	dm_thin_message_t type;
	union {
		struct {
			uint32_t device_id;
			uint32_t origin_id;
		} m_create_snap;
		struct {
			uint32_t device_id;
		} m_create_thin;
		struct {
			uint32_t device_id;
		} m_delete;
		struct {
			uint64_t current_id;
			uint64_t new_id;
		} m_set_transaction_id;
		struct {
			uint32_t device_id;
			uint64_t new_size;
		} m_trim;
	} u;
};

struct thin_message {
	struct dm_list list;
	struct dm_thin_message message;
	int expected_errno;
};

/* Replicator-log has a list of sites */
/* FIXME: maybe move to seg_area too? */
struct replicator_site {
	struct dm_list list;

	unsigned rsite_index;
	dm_replicator_mode_t mode;
	uint32_t async_timeout;
	uint32_t fall_behind_ios;
	uint64_t fall_behind_data;
};

/* Per-segment properties */
struct load_segment {
	struct dm_list list;

	unsigned type;

	uint64_t size;

	unsigned area_count;		/* Linear + Striped + Mirrored + Crypt + Replicator */
	struct dm_list areas;		/* Linear + Striped + Mirrored + Crypt + Replicator */

	uint32_t stripe_size;		/* Striped + raid */

	int persistent;			/* Snapshot */
	uint32_t chunk_size;		/* Snapshot */
	struct dm_tree_node *cow;	/* Snapshot */
	struct dm_tree_node *origin;	/* Snapshot + Snapshot origin */
	struct dm_tree_node *merge;	/* Snapshot */

	struct dm_tree_node *log;	/* Mirror + Replicator */
	uint32_t region_size;		/* Mirror + raid */
	unsigned clustered;		/* Mirror */
	unsigned mirror_area_count;	/* Mirror */
	uint32_t flags;			/* Mirror log */
	char *uuid;			/* Clustered mirror log */

	const char *cipher;		/* Crypt */
	const char *chainmode;		/* Crypt */
	const char *iv;			/* Crypt */
	uint64_t iv_offset;		/* Crypt */
	const char *key;		/* Crypt */

	const char *rlog_type;		/* Replicator */
	struct dm_list rsites;		/* Replicator */
	unsigned rsite_count;		/* Replicator */
	unsigned rdevice_count;		/* Replicator */
	struct dm_tree_node *replicator;/* Replicator-dev */
	uint64_t rdevice_index;		/* Replicator-dev */

	uint64_t rebuilds;	      /* raid */

	struct dm_tree_node *metadata;	/* Thin_pool */
	struct dm_tree_node *pool;	/* Thin_pool, Thin */
	struct dm_list thin_messages;	/* Thin_pool */
	uint64_t transaction_id;	/* Thin_pool */
	uint64_t low_water_mark;	/* Thin_pool */
	uint32_t data_block_size;       /* Thin_pool */
	unsigned skip_block_zeroing;	/* Thin_pool */
	uint32_t device_id;		/* Thin */

};

/* Per-device properties */
struct load_properties {
	int read_only;
	uint32_t major;
	uint32_t minor;

	uint32_t read_ahead;
	uint32_t read_ahead_flags;

	unsigned segment_count;
	unsigned size_changed;
	struct dm_list segs;

	const char *new_name;

	/* If immediate_dev_node is set to 1, try to create the dev node
	 * as soon as possible (e.g. in preload stage even during traversal
	 * and processing of dm tree). This will also flush all stacked dev
	 * node operations, synchronizing with udev.
	 */
	unsigned immediate_dev_node;

	/*
	 * If the device size changed from zero and this is set,
	 * don't resume the device immediately, even if the device
	 * has parents.  This works provided the parents do not
	 * validate the device size and is required by pvmove to
	 * avoid starting the mirror resync operation too early.
	 */
	unsigned delay_resume_if_new;

	/* Send messages for this node in preload */
	unsigned send_messages;
};

/* Two of these used to join two nodes with uses and used_by. */
struct dm_tree_link {
	struct dm_list list;
	struct dm_tree_node *node;
};

struct dm_tree_node {
	struct dm_tree *dtree;

	const char *name;
	const char *uuid;
	struct dm_info info;

	struct dm_list uses;       	/* Nodes this node uses */
	struct dm_list used_by;    	/* Nodes that use this node */

	int activation_priority;	/* 0 gets activated first */

	uint16_t udev_flags;		/* Udev control flags */

	void *context;			/* External supplied context */

	struct load_properties props;	/* For creation/table (re)load */

	/*
	 * If presuspend of child node is needed
	 * Note: only direct child is allowed
	 */
	struct dm_tree_node *presuspend_node;
};

struct dm_tree {
	struct dm_pool *mem;
	struct dm_hash_table *devs;
	struct dm_hash_table *uuids;
	struct dm_tree_node root;
	int skip_lockfs;		/* 1 skips lockfs (for non-snapshots) */
	int no_flush;			/* 1 sets noflush (mirrors/multipath) */
	int retry_remove;		/* 1 retries remove if not successful */
	uint32_t cookie;
};

/*
 * Tree functions.
 */
struct dm_tree *dm_tree_create(void)
{
	struct dm_pool *dmem;
	struct dm_tree *dtree;

	if (!(dmem = dm_pool_create("dtree", 1024)) ||
	    !(dtree = dm_pool_zalloc(dmem, sizeof(*dtree)))) {
		log_error("Failed to allocate dtree.");
		if (dmem)
			dm_pool_destroy(dmem);
		return NULL;
	}

	dtree->root.dtree = dtree;
	dm_list_init(&dtree->root.uses);
	dm_list_init(&dtree->root.used_by);
	dtree->skip_lockfs = 0;
	dtree->no_flush = 0;
	dtree->mem = dmem;

	if (!(dtree->devs = dm_hash_create(8))) {
		log_error("dtree hash creation failed");
		dm_pool_destroy(dtree->mem);
		return NULL;
	}

	if (!(dtree->uuids = dm_hash_create(32))) {
		log_error("dtree uuid hash creation failed");
		dm_hash_destroy(dtree->devs);
		dm_pool_destroy(dtree->mem);
		return NULL;
	}

	return dtree;
}

void dm_tree_free(struct dm_tree *dtree)
{
	if (!dtree)
		return;

	dm_hash_destroy(dtree->uuids);
	dm_hash_destroy(dtree->devs);
	dm_pool_destroy(dtree->mem);
}

void dm_tree_set_cookie(struct dm_tree_node *node, uint32_t cookie)
{
	node->dtree->cookie = cookie;
}

uint32_t dm_tree_get_cookie(struct dm_tree_node *node)
{
	return node->dtree->cookie;
}

void dm_tree_skip_lockfs(struct dm_tree_node *dnode)
{
	dnode->dtree->skip_lockfs = 1;
}

void dm_tree_use_no_flush_suspend(struct dm_tree_node *dnode)
{
	dnode->dtree->no_flush = 1;
}

void dm_tree_retry_remove(struct dm_tree_node *dnode)
{
	dnode->dtree->retry_remove = 1;
}

/*
 * Node functions.
 */
static int _nodes_are_linked(const struct dm_tree_node *parent,
			     const struct dm_tree_node *child)
{
	struct dm_tree_link *dlink;

	dm_list_iterate_items(dlink, &parent->uses)
		if (dlink->node == child)
			return 1;

	return 0;
}

static int _link(struct dm_list *list, struct dm_tree_node *node)
{
	struct dm_tree_link *dlink;

	if (!(dlink = dm_pool_alloc(node->dtree->mem, sizeof(*dlink)))) {
		log_error("dtree link allocation failed");
		return 0;
	}

	dlink->node = node;
	dm_list_add(list, &dlink->list);

	return 1;
}

static int _link_nodes(struct dm_tree_node *parent,
		       struct dm_tree_node *child)
{
	if (_nodes_are_linked(parent, child))
		return 1;

	if (!_link(&parent->uses, child))
		return 0;

	if (!_link(&child->used_by, parent))
		return 0;

	return 1;
}

static void _unlink(struct dm_list *list, struct dm_tree_node *node)
{
	struct dm_tree_link *dlink;

	dm_list_iterate_items(dlink, list)
		if (dlink->node == node) {
			dm_list_del(&dlink->list);
			break;
		}
}

static void _unlink_nodes(struct dm_tree_node *parent,
			  struct dm_tree_node *child)
{
	if (!_nodes_are_linked(parent, child))
		return;

	_unlink(&parent->uses, child);
	_unlink(&child->used_by, parent);
}

static int _add_to_toplevel(struct dm_tree_node *node)
{
	return _link_nodes(&node->dtree->root, node);
}

static void _remove_from_toplevel(struct dm_tree_node *node)
{
	_unlink_nodes(&node->dtree->root, node);
}

static int _add_to_bottomlevel(struct dm_tree_node *node)
{
	return _link_nodes(node, &node->dtree->root);
}

static void _remove_from_bottomlevel(struct dm_tree_node *node)
{
	_unlink_nodes(node, &node->dtree->root);
}

static int _link_tree_nodes(struct dm_tree_node *parent, struct dm_tree_node *child)
{
	/* Don't link to root node if child already has a parent */
	if (parent == &parent->dtree->root) {
		if (dm_tree_node_num_children(child, 1))
			return 1;
	} else
		_remove_from_toplevel(child);

	if (child == &child->dtree->root) {
		if (dm_tree_node_num_children(parent, 0))
			return 1;
	} else
		_remove_from_bottomlevel(parent);

	return _link_nodes(parent, child);
}

static struct dm_tree_node *_create_dm_tree_node(struct dm_tree *dtree,
						 const char *name,
						 const char *uuid,
						 struct dm_info *info,
						 void *context,
						 uint16_t udev_flags)
{
	struct dm_tree_node *node;
	uint64_t dev;

	if (!(node = dm_pool_zalloc(dtree->mem, sizeof(*node)))) {
		log_error("_create_dm_tree_node alloc failed");
		return NULL;
	}

	node->dtree = dtree;

	node->name = name;
	node->uuid = uuid;
	node->info = *info;
	node->context = context;
	node->udev_flags = udev_flags;
	node->activation_priority = 0;

	dm_list_init(&node->uses);
	dm_list_init(&node->used_by);
	dm_list_init(&node->props.segs);

	dev = MKDEV(info->major, info->minor);

	if (!dm_hash_insert_binary(dtree->devs, (const char *) &dev,
				sizeof(dev), node)) {
		log_error("dtree node hash insertion failed");
		dm_pool_free(dtree->mem, node);
		return NULL;
	}

	if (uuid && *uuid &&
	    !dm_hash_insert(dtree->uuids, uuid, node)) {
		log_error("dtree uuid hash insertion failed");
		dm_hash_remove_binary(dtree->devs, (const char *) &dev,
				      sizeof(dev));
		dm_pool_free(dtree->mem, node);
		return NULL;
	}

	return node;
}

static struct dm_tree_node *_find_dm_tree_node(struct dm_tree *dtree,
					       uint32_t major, uint32_t minor)
{
	uint64_t dev = MKDEV(major, minor);

	return dm_hash_lookup_binary(dtree->devs, (const char *) &dev,
				  sizeof(dev));
}

static struct dm_tree_node *_find_dm_tree_node_by_uuid(struct dm_tree *dtree,
						       const char *uuid)
{
	struct dm_tree_node *node;
	const char *default_uuid_prefix;
	size_t default_uuid_prefix_len;

	if ((node = dm_hash_lookup(dtree->uuids, uuid)))
		return node;

	default_uuid_prefix = dm_uuid_prefix();
	default_uuid_prefix_len = strlen(default_uuid_prefix);

	if (strncmp(uuid, default_uuid_prefix, default_uuid_prefix_len))
		return NULL;

	return dm_hash_lookup(dtree->uuids, uuid + default_uuid_prefix_len);
}

void dm_tree_node_set_udev_flags(struct dm_tree_node *dnode, uint16_t udev_flags)

{
	struct dm_info *dinfo = &dnode->info;

	if (udev_flags != dnode->udev_flags)
		log_debug("Resetting %s (%" PRIu32 ":%" PRIu32
			  ") udev_flags from 0x%x to 0x%x",
			  dnode->name, dinfo->major, dinfo->minor,
			  dnode->udev_flags, udev_flags);
	dnode->udev_flags = udev_flags;
}

void dm_tree_node_set_read_ahead(struct dm_tree_node *dnode,
				 uint32_t read_ahead,
				 uint32_t read_ahead_flags)
{
	dnode->props.read_ahead = read_ahead;
	dnode->props.read_ahead_flags = read_ahead_flags;
}

void dm_tree_node_set_presuspend_node(struct dm_tree_node *node,
				      struct dm_tree_node *presuspend_node)
{
	node->presuspend_node = presuspend_node;
}

const char *dm_tree_node_get_name(const struct dm_tree_node *node)
{
	return node->info.exists ? node->name : "";
}

const char *dm_tree_node_get_uuid(const struct dm_tree_node *node)
{
	return node->info.exists ? node->uuid : "";
}

const struct dm_info *dm_tree_node_get_info(const struct dm_tree_node *node)
{
	return &node->info;
}

void *dm_tree_node_get_context(const struct dm_tree_node *node)
{
	return node->context;
}

int dm_tree_node_size_changed(const struct dm_tree_node *dnode)
{
	return dnode->props.size_changed;
}

int dm_tree_node_num_children(const struct dm_tree_node *node, uint32_t inverted)
{
	if (inverted) {
		if (_nodes_are_linked(&node->dtree->root, node))
			return 0;
		return dm_list_size(&node->used_by);
	}

	if (_nodes_are_linked(node, &node->dtree->root))
		return 0;

	return dm_list_size(&node->uses);
}

/*
 * Returns 1 if no prefix supplied
 */
static int _uuid_prefix_matches(const char *uuid, const char *uuid_prefix, size_t uuid_prefix_len)
{
	const char *default_uuid_prefix = dm_uuid_prefix();
	size_t default_uuid_prefix_len = strlen(default_uuid_prefix);

	if (!uuid_prefix)
		return 1;

	if (!strncmp(uuid, uuid_prefix, uuid_prefix_len))
		return 1;

	/* Handle transition: active device uuids might be missing the prefix */
	if (uuid_prefix_len <= 4)
		return 0;

	if (!strncmp(uuid, default_uuid_prefix, default_uuid_prefix_len))
		return 0;

	if (strncmp(uuid_prefix, default_uuid_prefix, default_uuid_prefix_len))
		return 0;

	if (!strncmp(uuid, uuid_prefix + default_uuid_prefix_len, uuid_prefix_len - default_uuid_prefix_len))
		return 1;

	return 0;
}

/*
 * Returns 1 if no children.
 */
static int _children_suspended(struct dm_tree_node *node,
			       uint32_t inverted,
			       const char *uuid_prefix,
			       size_t uuid_prefix_len)
{
	struct dm_list *list;
	struct dm_tree_link *dlink;
	const struct dm_info *dinfo;
	const char *uuid;

	if (inverted) {
		if (_nodes_are_linked(&node->dtree->root, node))
			return 1;
		list = &node->used_by;
	} else {
		if (_nodes_are_linked(node, &node->dtree->root))
			return 1;
		list = &node->uses;
	}

	dm_list_iterate_items(dlink, list) {
		if (!(uuid = dm_tree_node_get_uuid(dlink->node))) {
			stack;
			continue;
		}

		/* Ignore if it doesn't belong to this VG */
		if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
			continue;

		/* Ignore if parent node wants to presuspend this node */
		if (dlink->node->presuspend_node == node)
			continue;

		if (!(dinfo = dm_tree_node_get_info(dlink->node))) {
			stack;	/* FIXME Is this normal? */
			return 0;
		}

		if (!dinfo->suspended)
			return 0;
	}

	return 1;
}

/*
 * Set major and minor to zero for root of tree.
 */
struct dm_tree_node *dm_tree_find_node(struct dm_tree *dtree,
					  uint32_t major,
					  uint32_t minor)
{
	if (!major && !minor)
		return &dtree->root;

	return _find_dm_tree_node(dtree, major, minor);
}

/*
 * Set uuid to NULL for root of tree.
 */
struct dm_tree_node *dm_tree_find_node_by_uuid(struct dm_tree *dtree,
						  const char *uuid)
{
	if (!uuid || !*uuid)
		return &dtree->root;

	return _find_dm_tree_node_by_uuid(dtree, uuid);
}

/*
 * First time set *handle to NULL.
 * Set inverted to invert the tree.
 */
struct dm_tree_node *dm_tree_next_child(void **handle,
					const struct dm_tree_node *parent,
					uint32_t inverted)
{
	struct dm_list **dlink = (struct dm_list **) handle;
	const struct dm_list *use_list;

	if (inverted)
		use_list = &parent->used_by;
	else
		use_list = &parent->uses;

	if (!*dlink)
		*dlink = dm_list_first(use_list);
	else
		*dlink = dm_list_next(use_list, *dlink);

	return (*dlink) ? dm_list_item(*dlink, struct dm_tree_link)->node : NULL;
}

static int _deps(struct dm_task **dmt, struct dm_pool *mem, uint32_t major, uint32_t minor,
		 const char **name, const char **uuid, unsigned inactive_table,
		 struct dm_info *info, struct dm_deps **deps)
{
	memset(info, 0, sizeof(*info));

	if (!dm_is_dm_major(major)) {
		if (name)
			*name = "";
		if (uuid)
			*uuid = "";
		*deps = NULL;
		info->major = major;
		info->minor = minor;
		return 1;
	}

	if (!(*dmt = dm_task_create(DM_DEVICE_DEPS))) {
		log_error("deps dm_task creation failed");
		return 0;
	}

	if (!dm_task_set_major(*dmt, major)) {
		log_error("_deps: failed to set major for (%" PRIu32 ":%" PRIu32 ")",
			  major, minor);
		goto failed;
	}

	if (!dm_task_set_minor(*dmt, minor)) {
		log_error("_deps: failed to set minor for (%" PRIu32 ":%" PRIu32 ")",
			  major, minor);
		goto failed;
	}

	if (inactive_table && !dm_task_query_inactive_table(*dmt)) {
		log_error("_deps: failed to set inactive table for (%" PRIu32 ":%" PRIu32 ")",
			  major, minor);
		goto failed;
	}

	if (!dm_task_run(*dmt)) {
		log_error("_deps: task run failed for (%" PRIu32 ":%" PRIu32 ")",
			  major, minor);
		goto failed;
	}

	if (!dm_task_get_info(*dmt, info)) {
		log_error("_deps: failed to get info for (%" PRIu32 ":%" PRIu32 ")",
			  major, minor);
		goto failed;
	}

	if (!info->exists) {
		if (name)
			*name = "";
		if (uuid)
			*uuid = "";
		*deps = NULL;
	} else {
		if (info->major != major) {
			log_error("Inconsistent dtree major number: %u != %u",
				  major, info->major);
			goto failed;
		}
		if (info->minor != minor) {
			log_error("Inconsistent dtree minor number: %u != %u",
				  minor, info->minor);
			goto failed;
		}
		if (name && !(*name = dm_pool_strdup(mem, dm_task_get_name(*dmt)))) {
			log_error("name pool_strdup failed");
			goto failed;
		}
		if (uuid && !(*uuid = dm_pool_strdup(mem, dm_task_get_uuid(*dmt)))) {
			log_error("uuid pool_strdup failed");
			goto failed;
		}
		*deps = dm_task_get_deps(*dmt);
	}

	return 1;

failed:
	dm_task_destroy(*dmt);
	return 0;
}

/*
 * Deactivate a device with its dependencies if the uuid prefix matches.
 */
static int _info_by_dev(uint32_t major, uint32_t minor, int with_open_count,
			struct dm_info *info, struct dm_pool *mem,
			const char **name, const char **uuid)
{
	struct dm_task *dmt;
	int r;

	if (!(dmt = dm_task_create(DM_DEVICE_INFO))) {
		log_error("_info_by_dev: dm_task creation failed");
		return 0;
	}

	if (!dm_task_set_major(dmt, major) || !dm_task_set_minor(dmt, minor)) {
		log_error("_info_by_dev: Failed to set device number");
		dm_task_destroy(dmt);
		return 0;
	}

	if (!with_open_count && !dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!(r = dm_task_run(dmt)))
		goto_out;

	if (!(r = dm_task_get_info(dmt, info)))
		goto_out;

	if (name && !(*name = dm_pool_strdup(mem, dm_task_get_name(dmt)))) {
		log_error("name pool_strdup failed");
		r = 0;
		goto_out;
	}

	if (uuid && !(*uuid = dm_pool_strdup(mem, dm_task_get_uuid(dmt)))) {
		log_error("uuid pool_strdup failed");
		r = 0;
		goto_out;
	}

out:
	dm_task_destroy(dmt);

	return r;
}

static int _check_device_not_in_use(const char *name, struct dm_info *info)
{
	if (!info->exists)
		return 1;

	/* If sysfs is not used, use open_count information only. */
	if (!*dm_sysfs_dir()) {
		if (info->open_count) {
			log_error("Device %s (%" PRIu32 ":%" PRIu32 ") in use",
				  name, info->major, info->minor);
			return 0;
		}

		return 1;
	}

	if (dm_device_has_holders(info->major, info->minor)) {
		log_error("Device %s (%" PRIu32 ":%" PRIu32 ") is used "
			  "by another device.", name, info->major, info->minor);
		return 0;
	}

	if (dm_device_has_mounted_fs(info->major, info->minor)) {
		log_error("Device %s (%" PRIu32 ":%" PRIu32 ") contains "
			  "a filesystem in use.", name, info->major, info->minor);
		return 0;
	}

	return 1;
}

/* Check if all parent nodes of given node have open_count == 0 */
static int _node_has_closed_parents(struct dm_tree_node *node,
				    const char *uuid_prefix,
				    size_t uuid_prefix_len)
{
	struct dm_tree_link *dlink;
	const struct dm_info *dinfo;
	struct dm_info info;
	const char *uuid;

	/* Iterate through parents of this node */
	dm_list_iterate_items(dlink, &node->used_by) {
		if (!(uuid = dm_tree_node_get_uuid(dlink->node))) {
			stack;
			continue;
		}

		/* Ignore if it doesn't belong to this VG */
		if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
			continue;

		if (!(dinfo = dm_tree_node_get_info(dlink->node))) {
			stack;	/* FIXME Is this normal? */
			return 0;
		}

		/* Refresh open_count */
		if (!_info_by_dev(dinfo->major, dinfo->minor, 1, &info, NULL, NULL, NULL) ||
		    !info.exists)
			continue;

		if (info.open_count) {
			log_debug("Node %s %d:%d has open_count %d", uuid_prefix,
				  dinfo->major, dinfo->minor, info.open_count);
			return 0;
		}
	}

	return 1;
}

static int _deactivate_node(const char *name, uint32_t major, uint32_t minor,
			    uint32_t *cookie, uint16_t udev_flags, int retry)
{
	struct dm_task *dmt;
	int r = 0;

	log_verbose("Removing %s (%" PRIu32 ":%" PRIu32 ")", name, major, minor);

	if (!(dmt = dm_task_create(DM_DEVICE_REMOVE))) {
		log_error("Deactivation dm_task creation failed for %s", name);
		return 0;
	}

	if (!dm_task_set_major(dmt, major) || !dm_task_set_minor(dmt, minor)) {
		log_error("Failed to set device number for %s deactivation", name);
		goto out;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (cookie)
		if (!dm_task_set_cookie(dmt, cookie, udev_flags))
			goto out;

	if (retry)
		dm_task_retry_remove(dmt);

	r = dm_task_run(dmt);

	/* FIXME Until kernel returns actual name so dm-iface.c can handle it */
	rm_dev_node(name, dmt->cookie_set && !(udev_flags & DM_UDEV_DISABLE_DM_RULES_FLAG),
		    dmt->cookie_set && (udev_flags & DM_UDEV_DISABLE_LIBRARY_FALLBACK));

	/* FIXME Remove node from tree or mark invalid? */

out:
	dm_task_destroy(dmt);

	return r;
}

static int _node_clear_table(struct dm_tree_node *dnode, uint16_t udev_flags)
{
	struct dm_task *dmt = NULL, *deps_dmt = NULL;
	struct dm_info *info, deps_info;
	struct dm_deps *deps = NULL;
	const char *name, *uuid;
	const char *default_uuid_prefix;
	size_t default_uuid_prefix_len;
	uint32_t i;
	int r = 0;

	if (!(info = &dnode->info)) {
		log_error("_node_clear_table failed: missing info");
		return 0;
	}

	if (!(name = dm_tree_node_get_name(dnode))) {
		log_error("_node_clear_table failed: missing name");
		return 0;
	}

	/* Is there a table? */
	if (!info->exists || !info->inactive_table)
		return 1;

	/* Get devices used by inactive table that's about to be deleted. */
	if (!_deps(&deps_dmt, dnode->dtree->mem, info->major, info->minor, NULL, NULL, 1, info, &deps)) {
		log_error("Failed to obtain dependencies for %s before clearing table.", name);
		return 0;
	}

	log_verbose("Clearing inactive table %s (%" PRIu32 ":%" PRIu32 ")",
		    name, info->major, info->minor);

	if (!(dmt = dm_task_create(DM_DEVICE_CLEAR))) {
		log_error("Table clear dm_task creation failed for %s", name);
		goto_out;
	}

	if (!dm_task_set_major(dmt, info->major) ||
	    !dm_task_set_minor(dmt, info->minor)) {
		log_error("Failed to set device number for %s table clear", name);
		goto_out;
	}

	r = dm_task_run(dmt);

	if (!dm_task_get_info(dmt, info)) {
		log_error("_node_clear_table failed: info missing after running task for %s", name);
		r = 0;
	}

	if (!r || !deps)
		goto_out;

	/*
	 * Remove (incomplete) devices that the inactive table referred to but
	 * which are not in the tree, no longer referenced and don't have a live
	 * table.
	 */
	default_uuid_prefix = dm_uuid_prefix();
	default_uuid_prefix_len = strlen(default_uuid_prefix);

	for (i = 0; i < deps->count; i++) {
		/* If already in tree, assume it's under control */
		if (_find_dm_tree_node(dnode->dtree, MAJOR(deps->device[i]), MINOR(deps->device[i])))
			continue;

		if (!_info_by_dev(MAJOR(deps->device[i]), MINOR(deps->device[i]), 1,
				  &deps_info, dnode->dtree->mem, &name, &uuid))
			continue;

		/* Proceed if device is an 'orphan' - unreferenced and without a live table. */
		if (!deps_info.exists || deps_info.live_table || deps_info.open_count)
			continue;

		if (strncmp(uuid, default_uuid_prefix, default_uuid_prefix_len))
			continue;

		/* Remove device. */
		if (!_deactivate_node(name, deps_info.major, deps_info.minor, &dnode->dtree->cookie, udev_flags, 0)) {
			log_error("Failed to deactivate no-longer-used device %s (%"
				  PRIu32 ":%" PRIu32 ")", name, deps_info.major, deps_info.minor);
		} else if (deps_info.suspended)
			dec_suspended();
	}

out:
	if (dmt)
		dm_task_destroy(dmt);

	if (deps_dmt)
		dm_task_destroy(deps_dmt);

	return r;
}

struct dm_tree_node *dm_tree_add_new_dev_with_udev_flags(struct dm_tree *dtree,
							 const char *name,
							 const char *uuid,
							 uint32_t major,
							 uint32_t minor,
							 int read_only,
							 int clear_inactive,
							 void *context,
							 uint16_t udev_flags)
{
	struct dm_tree_node *dnode;
	struct dm_info info;
	const char *name2;
	const char *uuid2;

	if (!name || !uuid) {
		log_error("Cannot add device without name and uuid.");
		return NULL;
	}

	/* Do we need to add node to tree? */
	if (!(dnode = dm_tree_find_node_by_uuid(dtree, uuid))) {
		if (!(name2 = dm_pool_strdup(dtree->mem, name))) {
			log_error("name pool_strdup failed");
			return NULL;
		}
		if (!(uuid2 = dm_pool_strdup(dtree->mem, uuid))) {
			log_error("uuid pool_strdup failed");
			return NULL;
		}

		memset(&info, 0, sizeof(info));

		if (!(dnode = _create_dm_tree_node(dtree, name2, uuid2, &info,
						   context, 0)))
			return_NULL;

		/* Attach to root node until a table is supplied */
		if (!_add_to_toplevel(dnode) || !_add_to_bottomlevel(dnode))
			return_NULL;

		dnode->props.major = major;
		dnode->props.minor = minor;
		dnode->props.new_name = NULL;
		dnode->props.size_changed = 0;
	} else if (strcmp(name, dnode->name)) {
		/* Do we need to rename node? */
		if (!(dnode->props.new_name = dm_pool_strdup(dtree->mem, name))) {
			log_error("name pool_strdup failed");
			return NULL;
		}
	}

	dnode->props.read_only = read_only ? 1 : 0;
	dnode->props.read_ahead = DM_READ_AHEAD_AUTO;
	dnode->props.read_ahead_flags = 0;

	if (clear_inactive && !_node_clear_table(dnode, udev_flags))
		return_NULL;

	dnode->context = context;
	dnode->udev_flags = udev_flags;

	return dnode;
}

struct dm_tree_node *dm_tree_add_new_dev(struct dm_tree *dtree, const char *name,
					 const char *uuid, uint32_t major, uint32_t minor,
					 int read_only, int clear_inactive, void *context)
{
	return dm_tree_add_new_dev_with_udev_flags(dtree, name, uuid, major, minor,
						   read_only, clear_inactive, context, 0);
}

static struct dm_tree_node *_add_dev(struct dm_tree *dtree,
				     struct dm_tree_node *parent,
				     uint32_t major, uint32_t minor,
				     uint16_t udev_flags)
{
	struct dm_task *dmt = NULL;
	struct dm_info info;
	struct dm_deps *deps = NULL;
	const char *name = NULL;
	const char *uuid = NULL;
	struct dm_tree_node *node = NULL;
	uint32_t i;
	int new = 0;

	/* Already in tree? */
	if (!(node = _find_dm_tree_node(dtree, major, minor))) {
		if (!_deps(&dmt, dtree->mem, major, minor, &name, &uuid, 0, &info, &deps))
			return_NULL;

		if (!(node = _create_dm_tree_node(dtree, name, uuid, &info,
						  NULL, udev_flags)))
			goto_out;
		new = 1;
	}

	if (!_link_tree_nodes(parent, node)) {
		node = NULL;
		goto_out;
	}

	/* If node was already in tree, no need to recurse. */
	if (!new)
		goto out;

	/* Can't recurse if not a mapped device or there are no dependencies */
	if (!node->info.exists || !deps || !deps->count) {
		if (!_add_to_bottomlevel(node)) {
			stack;
			node = NULL;
		}
		goto out;
	}

	/* Add dependencies to tree */
	for (i = 0; i < deps->count; i++)
		if (!_add_dev(dtree, node, MAJOR(deps->device[i]),
			      MINOR(deps->device[i]), udev_flags)) {
			node = NULL;
			goto_out;
		}

out:
	if (dmt)
		dm_task_destroy(dmt);

	return node;
}

int dm_tree_add_dev(struct dm_tree *dtree, uint32_t major, uint32_t minor)
{
	return _add_dev(dtree, &dtree->root, major, minor, 0) ? 1 : 0;
}

int dm_tree_add_dev_with_udev_flags(struct dm_tree *dtree, uint32_t major,
				    uint32_t minor, uint16_t udev_flags)
{
	return _add_dev(dtree, &dtree->root, major, minor, udev_flags) ? 1 : 0;
}

static int _rename_node(const char *old_name, const char *new_name, uint32_t major,
			uint32_t minor, uint32_t *cookie, uint16_t udev_flags)
{
	struct dm_task *dmt;
	int r = 0;

	log_verbose("Renaming %s (%" PRIu32 ":%" PRIu32 ") to %s", old_name, major, minor, new_name);

	if (!(dmt = dm_task_create(DM_DEVICE_RENAME))) {
		log_error("Rename dm_task creation failed for %s", old_name);
		return 0;
	}

	if (!dm_task_set_name(dmt, old_name)) {
		log_error("Failed to set name for %s rename.", old_name);
		goto out;
	}

	if (!dm_task_set_newname(dmt, new_name))
		goto_out;

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_set_cookie(dmt, cookie, udev_flags))
		goto out;

	r = dm_task_run(dmt);

out:
	dm_task_destroy(dmt);

	return r;
}

/* FIXME Merge with _suspend_node? */
static int _resume_node(const char *name, uint32_t major, uint32_t minor,
			uint32_t read_ahead, uint32_t read_ahead_flags,
			struct dm_info *newinfo, uint32_t *cookie,
			uint16_t udev_flags, int already_suspended)
{
	struct dm_task *dmt;
	int r = 0;

	log_verbose("Resuming %s (%" PRIu32 ":%" PRIu32 ")", name, major, minor);

	if (!(dmt = dm_task_create(DM_DEVICE_RESUME))) {
		log_debug("Suspend dm_task creation failed for %s.", name);
		return 0;
	}

	/* FIXME Kernel should fill in name on return instead */
	if (!dm_task_set_name(dmt, name)) {
		log_debug("Failed to set device name for %s resumption.", name);
		goto out;
	}

	if (!dm_task_set_major(dmt, major) || !dm_task_set_minor(dmt, minor)) {
		log_error("Failed to set device number for %s resumption.", name);
		goto out;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (!dm_task_set_read_ahead(dmt, read_ahead, read_ahead_flags))
		log_error("Failed to set read ahead");

	if (!dm_task_set_cookie(dmt, cookie, udev_flags))
		goto_out;

	if (!(r = dm_task_run(dmt)))
		goto_out;

	if (already_suspended)
		dec_suspended();

	if (!(r = dm_task_get_info(dmt, newinfo)))
		stack;

out:
	dm_task_destroy(dmt);

	return r;
}

static int _suspend_node(const char *name, uint32_t major, uint32_t minor,
			 int skip_lockfs, int no_flush, struct dm_info *newinfo)
{
	struct dm_task *dmt;
	int r;

	log_verbose("Suspending %s (%" PRIu32 ":%" PRIu32 ")%s%s",
		    name, major, minor,
		    skip_lockfs ? "" : " with filesystem sync",
		    no_flush ? "" : " with device flush");

	if (!(dmt = dm_task_create(DM_DEVICE_SUSPEND))) {
		log_error("Suspend dm_task creation failed for %s", name);
		return 0;
	}

	if (!dm_task_set_major(dmt, major) || !dm_task_set_minor(dmt, minor)) {
		log_error("Failed to set device number for %s suspension.", name);
		dm_task_destroy(dmt);
		return 0;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if (skip_lockfs && !dm_task_skip_lockfs(dmt))
		log_error("Failed to set skip_lockfs flag.");

	if (no_flush && !dm_task_no_flush(dmt))
		log_error("Failed to set no_flush flag.");

	if ((r = dm_task_run(dmt))) {
		inc_suspended();
		r = dm_task_get_info(dmt, newinfo);
	}

	dm_task_destroy(dmt);

	return r;
}

static int _thin_pool_status_transaction_id(struct dm_tree_node *dnode, uint64_t *transaction_id)
{
	struct dm_task *dmt;
	int r = 0;
	uint64_t start, length;
	char *type = NULL;
	char *params = NULL;

	if (!(dmt = dm_task_create(DM_DEVICE_STATUS)))
		return_0;

	if (!dm_task_set_major(dmt, dnode->info.major) ||
	    !dm_task_set_minor(dmt, dnode->info.minor)) {
		log_error("Failed to set major minor.");
		goto out;
	}

	if (!dm_task_run(dmt))
		goto_out;

	dm_get_next_target(dmt, NULL, &start, &length, &type, &params);

	if (type && (strcmp(type, "thin-pool") != 0)) {
		log_error("Expected thin-pool target for %d:%d and got %s.",
			  dnode->info.major, dnode->info.minor, type);
		goto out;
	}

	if (!params || (sscanf(params, "%" PRIu64, transaction_id) != 1)) {
		log_error("Failed to parse transaction_id from %s.", params);
		goto out;
	}

	log_debug("Thin pool transaction id: %" PRIu64 " status: %s.", *transaction_id, params);

	r = 1;
out:
	dm_task_destroy(dmt);

	return r;
}

static int _thin_pool_node_message(struct dm_tree_node *dnode, struct thin_message *tm)
{
	struct dm_task *dmt;
	struct dm_thin_message *m = &tm->message;
	char buf[64];
	int r;

	switch (m->type) {
	case DM_THIN_MESSAGE_CREATE_SNAP:
		r = dm_snprintf(buf, sizeof(buf), "create_snap %u %u",
				m->u.m_create_snap.device_id,
				m->u.m_create_snap.origin_id);
		break;
	case DM_THIN_MESSAGE_CREATE_THIN:
		r = dm_snprintf(buf, sizeof(buf), "create_thin %u",
				m->u.m_create_thin.device_id);
		break;
	case DM_THIN_MESSAGE_DELETE:
		r = dm_snprintf(buf, sizeof(buf), "delete %u",
				m->u.m_delete.device_id);
		break;
	case DM_THIN_MESSAGE_TRIM:
		r = dm_snprintf(buf, sizeof(buf), "trim %u %" PRIu64,
				m->u.m_trim.device_id,
				m->u.m_trim.new_size);
		break;
	case DM_THIN_MESSAGE_SET_TRANSACTION_ID:
		r = dm_snprintf(buf, sizeof(buf),
				"set_transaction_id %" PRIu64 " %" PRIu64,
				m->u.m_set_transaction_id.current_id,
				m->u.m_set_transaction_id.new_id);
		break;
	default:
		r = -1;
	}

	if (r < 0) {
		log_error("Failed to prepare message.");
		return 0;
	}

	r = 0;

	if (!(dmt = dm_task_create(DM_DEVICE_TARGET_MSG)))
		return_0;

	if (!dm_task_set_major(dmt, dnode->info.major) ||
	    !dm_task_set_minor(dmt, dnode->info.minor)) {
		log_error("Failed to set message major minor.");
		goto out;
	}

	if (!dm_task_set_message(dmt, buf))
		goto_out;

        /* Internal functionality of dm_task */
	dmt->expected_errno = tm->expected_errno;

	if (!dm_task_run(dmt))
		goto_out;

	r = 1;
out:
	dm_task_destroy(dmt);

	return r;
}

static int _node_send_messages(struct dm_tree_node *dnode,
			       const char *uuid_prefix,
			       size_t uuid_prefix_len)
{
	struct load_segment *seg;
	struct thin_message *tmsg;
	uint64_t trans_id;
	const char *uuid;

	if (!dnode->info.exists || (dm_list_size(&dnode->props.segs) != 1))
		return 1;

	seg = dm_list_item(dm_list_last(&dnode->props.segs), struct load_segment);
	if (seg->type != SEG_THIN_POOL)
		return 1;

	if (!(uuid = dm_tree_node_get_uuid(dnode)))
		return_0;

	if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len)) {
		log_debug("UUID \"%s\" does not match.", uuid);
		return 1;
	}

	if (!_thin_pool_status_transaction_id(dnode, &trans_id))
		goto_bad;

	if (trans_id == seg->transaction_id)
		return 1; /* In sync - skip messages */

	if (trans_id != (seg->transaction_id - 1)) {
		log_error("Thin pool transaction_id=%" PRIu64 ", while expected: %" PRIu64 ".",
			  trans_id, seg->transaction_id - 1);
		goto bad; /* Nothing to send */
	}

	dm_list_iterate_items(tmsg, &seg->thin_messages)
		if (!(_thin_pool_node_message(dnode, tmsg)))
			goto_bad;

	return 1;
bad:
	/* Try to deactivate */
	if (!(dm_tree_deactivate_children(dnode, uuid_prefix, uuid_prefix_len)))
		log_error("Failed to deactivate %s", dnode->name);

	return 0;
}

/*
 * FIXME Don't attempt to deactivate known internal dependencies.
 */
static int _dm_tree_deactivate_children(struct dm_tree_node *dnode,
					const char *uuid_prefix,
					size_t uuid_prefix_len,
					unsigned level)
{
	int r = 1;
	void *handle = NULL;
	struct dm_tree_node *child = dnode;
	struct dm_info info;
	const struct dm_info *dinfo;
	const char *name;
	const char *uuid;

	while ((child = dm_tree_next_child(&handle, dnode, 0))) {
		if (!(dinfo = dm_tree_node_get_info(child))) {
			stack;
			continue;
		}

		if (!(name = dm_tree_node_get_name(child))) {
			stack;
			continue;
		}

		if (!(uuid = dm_tree_node_get_uuid(child))) {
			stack;
			continue;
		}

		/* Ignore if it doesn't belong to this VG */
		if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
			continue;

		/* Refresh open_count */
		if (!_info_by_dev(dinfo->major, dinfo->minor, 1, &info, NULL, NULL, NULL) ||
		    !info.exists)
			continue;

		if (info.open_count) {
			/* Skip internal non-toplevel opened nodes */
			if (level)
				continue;

			/* When retry is not allowed, error */
			if (!child->dtree->retry_remove) {
				log_error("Unable to deactivate open %s (%" PRIu32
					  ":%" PRIu32 ")", name, info.major, info.minor);
				r = 0;
				continue;
			}

			/* Check toplevel node for holders/mounted fs */
			if (!_check_device_not_in_use(name, &info)) {
				stack;
				r = 0;
				continue;
			}
			/* Go on with retry */
		}

		/* Also checking open_count in parent nodes of presuspend_node */
		if ((child->presuspend_node &&
		     !_node_has_closed_parents(child->presuspend_node,
					       uuid_prefix, uuid_prefix_len))) {
			/* Only report error from (likely non-internal) dependency at top level */
			if (!level) {
				log_error("Unable to deactivate open %s (%" PRIu32
					  ":%" PRIu32 ")", name, info.major,
				  	info.minor);
				r = 0;
			}
			continue;
		}

		/* Suspend child node first if requested */
		if (child->presuspend_node &&
		    !dm_tree_suspend_children(child, uuid_prefix, uuid_prefix_len))
			continue;

		if (!_deactivate_node(name, info.major, info.minor,
				      &child->dtree->cookie, child->udev_flags,
				      (level == 0) ? child->dtree->retry_remove : 0)) {
			log_error("Unable to deactivate %s (%" PRIu32
				  ":%" PRIu32 ")", name, info.major,
				  info.minor);
			r = 0;
			continue;
		} else if (info.suspended)
			dec_suspended();

		if (dm_tree_node_num_children(child, 0)) {
			if (!_dm_tree_deactivate_children(child, uuid_prefix, uuid_prefix_len, level + 1))
				return_0;
		}
	}

	return r;
}

int dm_tree_deactivate_children(struct dm_tree_node *dnode,
				   const char *uuid_prefix,
				   size_t uuid_prefix_len)
{
	return _dm_tree_deactivate_children(dnode, uuid_prefix, uuid_prefix_len, 0);
}

int dm_tree_suspend_children(struct dm_tree_node *dnode,
			     const char *uuid_prefix,
			     size_t uuid_prefix_len)
{
	int r = 1;
	void *handle = NULL;
	struct dm_tree_node *child = dnode;
	struct dm_info info, newinfo;
	const struct dm_info *dinfo;
	const char *name;
	const char *uuid;

	/* Suspend nodes at this level of the tree */
	while ((child = dm_tree_next_child(&handle, dnode, 0))) {
		if (!(dinfo = dm_tree_node_get_info(child))) {
			stack;
			continue;
		}

		if (!(name = dm_tree_node_get_name(child))) {
			stack;
			continue;
		}

		if (!(uuid = dm_tree_node_get_uuid(child))) {
			stack;
			continue;
		}

		/* Ignore if it doesn't belong to this VG */
		if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
			continue;

		/* Ensure immediate parents are already suspended */
		if (!_children_suspended(child, 1, uuid_prefix, uuid_prefix_len))
			continue;

		if (!_info_by_dev(dinfo->major, dinfo->minor, 0, &info, NULL, NULL, NULL) ||
		    !info.exists || info.suspended)
			continue;

		if (!_suspend_node(name, info.major, info.minor,
				   child->dtree->skip_lockfs,
				   child->dtree->no_flush, &newinfo)) {
			log_error("Unable to suspend %s (%" PRIu32
				  ":%" PRIu32 ")", name, info.major,
				  info.minor);
			r = 0;
			continue;
		}

		/* Update cached info */
		child->info = newinfo;
	}

	/* Then suspend any child nodes */
	handle = NULL;

	while ((child = dm_tree_next_child(&handle, dnode, 0))) {
		if (!(uuid = dm_tree_node_get_uuid(child))) {
			stack;
			continue;
		}

		/* Ignore if it doesn't belong to this VG */
		if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
			continue;

		if (dm_tree_node_num_children(child, 0))
			if (!dm_tree_suspend_children(child, uuid_prefix, uuid_prefix_len))
				return_0;
	}

	return r;
}

int dm_tree_activate_children(struct dm_tree_node *dnode,
				 const char *uuid_prefix,
				 size_t uuid_prefix_len)
{
	int r = 1;
	void *handle = NULL;
	struct dm_tree_node *child = dnode;
	struct dm_info newinfo;
	const char *name;
	const char *uuid;
	int priority;

	/* Activate children first */
	while ((child = dm_tree_next_child(&handle, dnode, 0))) {
		if (!(uuid = dm_tree_node_get_uuid(child))) {
			stack;
			continue;
		}

		if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
			continue;

		if (dm_tree_node_num_children(child, 0))
			if (!dm_tree_activate_children(child, uuid_prefix, uuid_prefix_len))
				return_0;
	}

	handle = NULL;

	for (priority = 0; priority < 3; priority++) {
		while ((child = dm_tree_next_child(&handle, dnode, 0))) {
			if (priority != child->activation_priority)
				continue;

			if (!(uuid = dm_tree_node_get_uuid(child))) {
				stack;
				continue;
			}

			if (!_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
				continue;

			if (!(name = dm_tree_node_get_name(child))) {
				stack;
				continue;
			}

			/* Rename? */
			if (child->props.new_name) {
				if (!_rename_node(name, child->props.new_name, child->info.major,
						  child->info.minor, &child->dtree->cookie,
						  child->udev_flags)) {
					log_error("Failed to rename %s (%" PRIu32
						  ":%" PRIu32 ") to %s", name, child->info.major,
						  child->info.minor, child->props.new_name);
					return 0;
				}
				child->name = child->props.new_name;
				child->props.new_name = NULL;
			}

			if (!child->info.inactive_table && !child->info.suspended)
				continue;

			if (!_resume_node(child->name, child->info.major, child->info.minor,
					  child->props.read_ahead, child->props.read_ahead_flags,
					  &newinfo, &child->dtree->cookie, child->udev_flags, child->info.suspended)) {
				log_error("Unable to resume %s (%" PRIu32
					  ":%" PRIu32 ")", child->name, child->info.major,
					  child->info.minor);
				r = 0;
				continue;
			}

			/* Update cached info */
			child->info = newinfo;
		}
	}

	/*
	 * FIXME: Implement delayed error reporting
	 * activation should be stopped only in the case,
	 * the submission of transation_id message fails,
	 * resume should continue further, just whole command
	 * has to report failure.
	 */
	if (r && dnode->props.send_messages &&
	    !(r = _node_send_messages(dnode, uuid_prefix, uuid_prefix_len)))
		stack;

	handle = NULL;

	return r;
}

static int _create_node(struct dm_tree_node *dnode)
{
	int r = 0;
	struct dm_task *dmt;

	log_verbose("Creating %s", dnode->name);

	if (!(dmt = dm_task_create(DM_DEVICE_CREATE))) {
		log_error("Create dm_task creation failed for %s", dnode->name);
		return 0;
	}

	if (!dm_task_set_name(dmt, dnode->name)) {
		log_error("Failed to set device name for %s", dnode->name);
		goto out;
	}

	if (!dm_task_set_uuid(dmt, dnode->uuid)) {
		log_error("Failed to set uuid for %s", dnode->name);
		goto out;
	}

	if (dnode->props.major &&
	    (!dm_task_set_major(dmt, dnode->props.major) ||
	     !dm_task_set_minor(dmt, dnode->props.minor))) {
		log_error("Failed to set device number for %s creation.", dnode->name);
		goto out;
	}

	if (dnode->props.read_only && !dm_task_set_ro(dmt)) {
		log_error("Failed to set read only flag for %s", dnode->name);
		goto out;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	if ((r = dm_task_run(dmt)))
		r = dm_task_get_info(dmt, &dnode->info);

out:
	dm_task_destroy(dmt);

	return r;
}


static int _build_dev_string(char *devbuf, size_t bufsize, struct dm_tree_node *node)
{
	if (!dm_format_dev(devbuf, bufsize, node->info.major, node->info.minor)) {
		log_error("Failed to format %s device number for %s as dm "
			  "target (%u,%u)",
			  node->name, node->uuid, node->info.major, node->info.minor);
		return 0;
	}

	return 1;
}

/* simplify string emiting code */
#define EMIT_PARAMS(p, str...)\
do {\
	int w;\
	if ((w = dm_snprintf(params + p, paramsize - (size_t) p, str)) < 0) {\
		stack; /* Out of space */\
		return -1;\
	}\
	p += w;\
} while (0)

/*
 * _emit_areas_line
 *
 * Returns: 1 on success, 0 on failure
 */
static int _emit_areas_line(struct dm_task *dmt __attribute__((unused)),
			    struct load_segment *seg, char *params,
			    size_t paramsize, int *pos)
{
	struct seg_area *area;
	char devbuf[DM_FORMAT_DEV_BUFSIZE];
	unsigned first_time = 1;
	const char *logtype, *synctype;
	unsigned log_parm_count;

	dm_list_iterate_items(area, &seg->areas) {
		switch (seg->type) {
		case SEG_REPLICATOR_DEV:
			if (!_build_dev_string(devbuf, sizeof(devbuf), area->dev_node))
				return_0;

			EMIT_PARAMS(*pos, " %d 1 %s", area->rsite_index, devbuf);
			if (first_time)
				EMIT_PARAMS(*pos, " nolog 0");
			else {
				/* Remote devices */
				log_parm_count = (area->flags &
						  (DM_NOSYNC | DM_FORCESYNC)) ? 2 : 1;

				if (!area->slog) {
					devbuf[0] = 0;		/* Only core log parameters */
					logtype = "core";
				} else {
					devbuf[0] = ' ';	/* Extra space before device name */
					if (!_build_dev_string(devbuf + 1,
							       sizeof(devbuf) - 1,
							       area->slog))
						return_0;
					logtype = "disk";
					log_parm_count++;	/* Extra sync log device name parameter */
				}

				EMIT_PARAMS(*pos, " %s %u%s %" PRIu64, logtype,
					    log_parm_count, devbuf, area->region_size);

				synctype = (area->flags & DM_NOSYNC) ?
						" nosync" : (area->flags & DM_FORCESYNC) ?
								" sync" : NULL;

				if (synctype)
					EMIT_PARAMS(*pos, "%s", synctype);
			}
			break;
		case SEG_RAID1:
		case SEG_RAID4:
		case SEG_RAID5_LA:
		case SEG_RAID5_RA:
		case SEG_RAID5_LS:
		case SEG_RAID5_RS:
		case SEG_RAID6_ZR:
		case SEG_RAID6_NR:
		case SEG_RAID6_NC:
			if (!area->dev_node) {
				EMIT_PARAMS(*pos, " -");
				break;
			}
			if (!_build_dev_string(devbuf, sizeof(devbuf), area->dev_node))
				return_0;

			EMIT_PARAMS(*pos, " %s", devbuf);
			break;
		default:
			if (!_build_dev_string(devbuf, sizeof(devbuf), area->dev_node))
				return_0;

			EMIT_PARAMS(*pos, "%s%s %" PRIu64, first_time ? "" : " ",
				    devbuf, area->offset);
		}

		first_time = 0;
	}

	return 1;
}

static int _replicator_emit_segment_line(const struct load_segment *seg, char *params,
					 size_t paramsize, int *pos)
{
	const struct load_segment *rlog_seg;
	struct replicator_site *rsite;
	char rlogbuf[DM_FORMAT_DEV_BUFSIZE];
	unsigned parm_count;

	if (!seg->log || !_build_dev_string(rlogbuf, sizeof(rlogbuf), seg->log))
		return_0;

	rlog_seg = dm_list_item(dm_list_last(&seg->log->props.segs),
				struct load_segment);

	EMIT_PARAMS(*pos, "%s 4 %s 0 auto %" PRIu64,
		    seg->rlog_type, rlogbuf, rlog_seg->size);

	dm_list_iterate_items(rsite, &seg->rsites) {
		parm_count = (rsite->fall_behind_data
			      || rsite->fall_behind_ios
			      || rsite->async_timeout) ? 4 : 2;

		EMIT_PARAMS(*pos, " blockdev %u %u %s", parm_count, rsite->rsite_index,
			    (rsite->mode == DM_REPLICATOR_SYNC) ? "synchronous" : "asynchronous");

		if (rsite->fall_behind_data)
			EMIT_PARAMS(*pos, " data %" PRIu64, rsite->fall_behind_data);
		else if (rsite->fall_behind_ios)
			EMIT_PARAMS(*pos, " ios %" PRIu32, rsite->fall_behind_ios);
		else if (rsite->async_timeout)
			EMIT_PARAMS(*pos, " timeout %" PRIu32, rsite->async_timeout);
	}

	return 1;
}

/*
 * Returns: 1 on success, 0 on failure
 */
static int _mirror_emit_segment_line(struct dm_task *dmt, struct load_segment *seg,
				     char *params, size_t paramsize)
{
	int block_on_error = 0;
	int handle_errors = 0;
	int dm_log_userspace = 0;
	struct utsname uts;
	unsigned log_parm_count;
	int pos = 0, parts;
	char logbuf[DM_FORMAT_DEV_BUFSIZE];
	const char *logtype;
	unsigned kmaj = 0, kmin = 0, krel = 0;

	if (uname(&uts) == -1) {
		log_error("Cannot read kernel release version.");
		return 0;
	}

	/* Kernels with a major number of 2 always had 3 parts. */
	parts = sscanf(uts.release, "%u.%u.%u", &kmaj, &kmin, &krel);
	if (parts < 1 || (kmaj < 3 && parts < 3)) {
		log_error("Wrong kernel release version %s.", uts.release);
		return 0;
	}

	if ((seg->flags & DM_BLOCK_ON_ERROR)) {
		/*
		 * Originally, block_on_error was an argument to the log
		 * portion of the mirror CTR table.  It was renamed to
		 * "handle_errors" and now resides in the 'features'
		 * section of the mirror CTR table (i.e. at the end).
		 *
		 * We can identify whether to use "block_on_error" or
		 * "handle_errors" by the dm-mirror module's version
		 * number (>= 1.12) or by the kernel version (>= 2.6.22).
		 */
		if (KERNEL_VERSION(kmaj, kmin, krel) >= KERNEL_VERSION(2, 6, 22))
			handle_errors = 1;
		else
			block_on_error = 1;
	}

	if (seg->clustered) {
		/* Cluster mirrors require a UUID */
		if (!seg->uuid)
			return_0;

		/*
		 * Cluster mirrors used to have their own log
		 * types.  Now they are accessed through the
		 * userspace log type.
		 *
		 * The dm-log-userspace module was added to the
		 * 2.6.31 kernel.
		 */
		if (KERNEL_VERSION(kmaj, kmin, krel) >= KERNEL_VERSION(2, 6, 31))
			dm_log_userspace = 1;
	}

	/* Region size */
	log_parm_count = 1;

	/* [no]sync, block_on_error etc. */
	log_parm_count += hweight32(seg->flags);

	/* "handle_errors" is a feature arg now */
	if (handle_errors)
		log_parm_count--;

	/* DM_CORELOG does not count in the param list */
	if (seg->flags & DM_CORELOG)
		log_parm_count--;

	if (seg->clustered) {
		log_parm_count++; /* For UUID */

		if (!dm_log_userspace)
			EMIT_PARAMS(pos, "clustered-");
		else
			/* For clustered-* type field inserted later */
			log_parm_count++;
	}

	if (!seg->log)
		logtype = "core";
	else {
		logtype = "disk";
		log_parm_count++;
		if (!_build_dev_string(logbuf, sizeof(logbuf), seg->log))
			return_0;
	}

	if (dm_log_userspace)
		EMIT_PARAMS(pos, "userspace %u %s clustered-%s",
			    log_parm_count, seg->uuid, logtype);
	else
		EMIT_PARAMS(pos, "%s %u", logtype, log_parm_count);

	if (seg->log)
		EMIT_PARAMS(pos, " %s", logbuf);

	EMIT_PARAMS(pos, " %u", seg->region_size);

	if (seg->clustered && !dm_log_userspace)
		EMIT_PARAMS(pos, " %s", seg->uuid);

	if ((seg->flags & DM_NOSYNC))
		EMIT_PARAMS(pos, " nosync");
	else if ((seg->flags & DM_FORCESYNC))
		EMIT_PARAMS(pos, " sync");

	if (block_on_error)
		EMIT_PARAMS(pos, " block_on_error");

	EMIT_PARAMS(pos, " %u ", seg->mirror_area_count);

	if (_emit_areas_line(dmt, seg, params, paramsize, &pos) <= 0)
		return_0;

	if (handle_errors)
		EMIT_PARAMS(pos, " 1 handle_errors");

	return 1;
}

static int _raid_emit_segment_line(struct dm_task *dmt, uint32_t major,
				   uint32_t minor, struct load_segment *seg,
				   uint64_t *seg_start, char *params,
				   size_t paramsize)
{
	uint32_t i;
	int param_count = 1; /* mandatory 'chunk size'/'stripe size' arg */
	int pos = 0;

	if ((seg->flags & DM_NOSYNC) || (seg->flags & DM_FORCESYNC))
		param_count++;

	if (seg->region_size)
		param_count += 2;

	/* rebuilds is 64-bit */
	param_count += 2 * hweight32(seg->rebuilds & 0xFFFFFFFF);
	param_count += 2 * hweight32(seg->rebuilds >> 32);

	if ((seg->type == SEG_RAID1) && seg->stripe_size)
		log_error("WARNING: Ignoring RAID1 stripe size");

	EMIT_PARAMS(pos, "%s %d %u", dm_segtypes[seg->type].target,
		    param_count, seg->stripe_size);

	if (seg->flags & DM_NOSYNC)
		EMIT_PARAMS(pos, " nosync");
	else if (seg->flags & DM_FORCESYNC)
		EMIT_PARAMS(pos, " sync");

	if (seg->region_size)
		EMIT_PARAMS(pos, " region_size %u", seg->region_size);

	for (i = 0; i < (seg->area_count / 2); i++)
		if (seg->rebuilds & (1 << i))
			EMIT_PARAMS(pos, " rebuild %u", i);

	/* Print number of metadata/data device pairs */
	EMIT_PARAMS(pos, " %u", seg->area_count/2);

	if (_emit_areas_line(dmt, seg, params, paramsize, &pos) <= 0)
		return_0;

	return 1;
}

static int _emit_segment_line(struct dm_task *dmt, uint32_t major,
			      uint32_t minor, struct load_segment *seg,
			      uint64_t *seg_start, char *params,
			      size_t paramsize)
{
	int pos = 0;
	int r;
	int target_type_is_raid = 0;
	char originbuf[DM_FORMAT_DEV_BUFSIZE], cowbuf[DM_FORMAT_DEV_BUFSIZE];
	char pool[DM_FORMAT_DEV_BUFSIZE], metadata[DM_FORMAT_DEV_BUFSIZE];

	switch(seg->type) {
	case SEG_ERROR:
	case SEG_ZERO:
	case SEG_LINEAR:
		break;
	case SEG_MIRRORED:
		/* Mirrors are pretty complicated - now in separate function */
		r = _mirror_emit_segment_line(dmt, seg, params, paramsize);
		if (!r)
			return_0;
		break;
	case SEG_REPLICATOR:
		if ((r = _replicator_emit_segment_line(seg, params, paramsize,
						       &pos)) <= 0) {
			stack;
			return r;
		}
		break;
	case SEG_REPLICATOR_DEV:
		if (!seg->replicator || !_build_dev_string(originbuf,
							   sizeof(originbuf),
							   seg->replicator))
			return_0;

		EMIT_PARAMS(pos, "%s %" PRIu64, originbuf, seg->rdevice_index);
		break;
	case SEG_SNAPSHOT:
	case SEG_SNAPSHOT_MERGE:
		if (!_build_dev_string(originbuf, sizeof(originbuf), seg->origin))
			return_0;
		if (!_build_dev_string(cowbuf, sizeof(cowbuf), seg->cow))
			return_0;
		EMIT_PARAMS(pos, "%s %s %c %d", originbuf, cowbuf,
			    seg->persistent ? 'P' : 'N', seg->chunk_size);
		break;
	case SEG_SNAPSHOT_ORIGIN:
		if (!_build_dev_string(originbuf, sizeof(originbuf), seg->origin))
			return_0;
		EMIT_PARAMS(pos, "%s", originbuf);
		break;
	case SEG_STRIPED:
		EMIT_PARAMS(pos, "%u %u ", seg->area_count, seg->stripe_size);
		break;
	case SEG_CRYPT:
		EMIT_PARAMS(pos, "%s%s%s%s%s %s %" PRIu64 " ", seg->cipher,
			    seg->chainmode ? "-" : "", seg->chainmode ?: "",
			    seg->iv ? "-" : "", seg->iv ?: "", seg->key,
			    seg->iv_offset != DM_CRYPT_IV_DEFAULT ?
			    seg->iv_offset : *seg_start);
		break;
	case SEG_RAID1:
	case SEG_RAID4:
	case SEG_RAID5_LA:
	case SEG_RAID5_RA:
	case SEG_RAID5_LS:
	case SEG_RAID5_RS:
	case SEG_RAID6_ZR:
	case SEG_RAID6_NR:
	case SEG_RAID6_NC:
		target_type_is_raid = 1;
		r = _raid_emit_segment_line(dmt, major, minor, seg, seg_start,
					    params, paramsize);
		if (!r)
			return_0;

		break;
	case SEG_THIN_POOL:
		if (!_build_dev_string(metadata, sizeof(metadata), seg->metadata))
			return_0;
		if (!_build_dev_string(pool, sizeof(pool), seg->pool))
			return_0;
		EMIT_PARAMS(pos, "%s %s %d %" PRIu64 " %s", metadata, pool,
			    seg->data_block_size, seg->low_water_mark,
			    seg->skip_block_zeroing ? "1 skip_block_zeroing" : "0");
		break;
	case SEG_THIN:
		if (!_build_dev_string(pool, sizeof(pool), seg->pool))
			return_0;
		EMIT_PARAMS(pos, "%s %d", pool, seg->device_id);
		break;
	}

	switch(seg->type) {
	case SEG_ERROR:
	case SEG_REPLICATOR:
	case SEG_SNAPSHOT:
	case SEG_SNAPSHOT_ORIGIN:
	case SEG_SNAPSHOT_MERGE:
	case SEG_ZERO:
	case SEG_THIN_POOL:
	case SEG_THIN:
		break;
	case SEG_CRYPT:
	case SEG_LINEAR:
	case SEG_REPLICATOR_DEV:
	case SEG_STRIPED:
		if ((r = _emit_areas_line(dmt, seg, params, paramsize, &pos)) <= 0) {
			stack;
			return r;
		}
		if (!params[0]) {
			log_error("No parameters supplied for %s target "
				  "%u:%u.", dm_segtypes[seg->type].target,
				  major, minor);
			return 0;
		}
		break;
	}

	log_debug("Adding target to (%" PRIu32 ":%" PRIu32 "): %" PRIu64
		  " %" PRIu64 " %s %s", major, minor,
		  *seg_start, seg->size, target_type_is_raid ? "raid" :
		  dm_segtypes[seg->type].target, params);

	if (!dm_task_add_target(dmt, *seg_start, seg->size,
				target_type_is_raid ? "raid" :
				dm_segtypes[seg->type].target, params))
		return_0;

	*seg_start += seg->size;

	return 1;
}

#undef EMIT_PARAMS

static int _emit_segment(struct dm_task *dmt, uint32_t major, uint32_t minor,
			 struct load_segment *seg, uint64_t *seg_start)
{
	char *params;
	size_t paramsize = 4096;
	int ret;

	do {
		if (!(params = dm_malloc(paramsize))) {
			log_error("Insufficient space for target parameters.");
			return 0;
		}

		params[0] = '\0';
		ret = _emit_segment_line(dmt, major, minor, seg, seg_start,
					 params, paramsize);
		dm_free(params);

		if (!ret)
			stack;

		if (ret >= 0)
			return ret;

		log_debug("Insufficient space in params[%" PRIsize_t
			  "] for target parameters.", paramsize);

		paramsize *= 2;
	} while (paramsize < MAX_TARGET_PARAMSIZE);

	log_error("Target parameter size too big. Aborting.");
	return 0;
}

static int _load_node(struct dm_tree_node *dnode)
{
	int r = 0;
	struct dm_task *dmt;
	struct load_segment *seg;
	uint64_t seg_start = 0, existing_table_size;

	log_verbose("Loading %s table (%" PRIu32 ":%" PRIu32 ")", dnode->name,
		    dnode->info.major, dnode->info.minor);

	if (!(dmt = dm_task_create(DM_DEVICE_RELOAD))) {
		log_error("Reload dm_task creation failed for %s", dnode->name);
		return 0;
	}

	if (!dm_task_set_major(dmt, dnode->info.major) ||
	    !dm_task_set_minor(dmt, dnode->info.minor)) {
		log_error("Failed to set device number for %s reload.", dnode->name);
		goto out;
	}

	if (dnode->props.read_only && !dm_task_set_ro(dmt)) {
		log_error("Failed to set read only flag for %s", dnode->name);
		goto out;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	dm_list_iterate_items(seg, &dnode->props.segs)
		if (!_emit_segment(dmt, dnode->info.major, dnode->info.minor,
				   seg, &seg_start))
			goto_out;

	if (!dm_task_suppress_identical_reload(dmt))
		log_error("Failed to suppress reload of identical tables.");

	if ((r = dm_task_run(dmt))) {
		r = dm_task_get_info(dmt, &dnode->info);
		if (r && !dnode->info.inactive_table)
			log_verbose("Suppressed %s identical table reload.",
				    dnode->name);

		existing_table_size = dm_task_get_existing_table_size(dmt);
		if ((dnode->props.size_changed =
		     (existing_table_size == seg_start) ? 0 : 1)) {
			log_debug("Table size changed from %" PRIu64 " to %"
				  PRIu64 " for %s", existing_table_size,
				  seg_start, dnode->name);
			/*
			 * Kernel usually skips size validation on zero-length devices
			 * now so no need to preload them.
			 */
			/* FIXME In which kernel version did this begin? */
			if (!existing_table_size && dnode->props.delay_resume_if_new)
				dnode->props.size_changed = 0;
		}
	}

	dnode->props.segment_count = 0;

out:
	dm_task_destroy(dmt);

	return r;
}

int dm_tree_preload_children(struct dm_tree_node *dnode,
			     const char *uuid_prefix,
			     size_t uuid_prefix_len)
{
	int r = 1;
	void *handle = NULL;
	struct dm_tree_node *child;
	struct dm_info newinfo;
	int update_devs_flag = 0;

	/* Preload children first */
	while ((child = dm_tree_next_child(&handle, dnode, 0))) {
		/* Skip existing non-device-mapper devices */
		if (!child->info.exists && child->info.major)
			continue;

		/* Ignore if it doesn't belong to this VG */
		if (child->info.exists &&
		    !_uuid_prefix_matches(child->uuid, uuid_prefix, uuid_prefix_len))
			continue;

		if (dm_tree_node_num_children(child, 0))
			if (!dm_tree_preload_children(child, uuid_prefix, uuid_prefix_len))
				return_0;

		/* FIXME Cope if name exists with no uuid? */
		if (!child->info.exists && !_create_node(child))
			return_0;

		if (!child->info.inactive_table &&
		    child->props.segment_count &&
		    !_load_node(child))
			return_0;

		/* Propagate device size change change */
		if (child->props.size_changed)
			dnode->props.size_changed = 1;

		/* Resume device immediately if it has parents and its size changed */
		if (!dm_tree_node_num_children(child, 1) || !child->props.size_changed)
			continue;

		if (!child->info.inactive_table && !child->info.suspended)
			continue;

		if (!_resume_node(child->name, child->info.major, child->info.minor,
				  child->props.read_ahead, child->props.read_ahead_flags,
				  &newinfo, &child->dtree->cookie, child->udev_flags,
				  child->info.suspended)) {
			log_error("Unable to resume %s (%" PRIu32
				  ":%" PRIu32 ")", child->name, child->info.major,
				  child->info.minor);
			r = 0;
			continue;
		}

		/* Update cached info */
		child->info = newinfo;
		/*
		 * Prepare for immediate synchronization with udev and flush all stacked
		 * dev node operations if requested by immediate_dev_node property. But
		 * finish processing current level in the tree first.
		 */
		if (child->props.immediate_dev_node)
			update_devs_flag = 1;
	}

	if (update_devs_flag) {
		if (!dm_udev_wait(dm_tree_get_cookie(dnode)))
			stack;
		dm_tree_set_cookie(dnode, 0);
	}

	return r;
}

/*
 * Returns 1 if unsure.
 */
int dm_tree_children_use_uuid(struct dm_tree_node *dnode,
				 const char *uuid_prefix,
				 size_t uuid_prefix_len)
{
	void *handle = NULL;
	struct dm_tree_node *child = dnode;
	const char *uuid;

	while ((child = dm_tree_next_child(&handle, dnode, 0))) {
		if (!(uuid = dm_tree_node_get_uuid(child))) {
			log_error("Failed to get uuid for dtree node.");
			return 1;
		}

		if (_uuid_prefix_matches(uuid, uuid_prefix, uuid_prefix_len))
			return 1;

		if (dm_tree_node_num_children(child, 0))
			dm_tree_children_use_uuid(child, uuid_prefix, uuid_prefix_len);
	}

	return 0;
}

/*
 * Target functions
 */
static struct load_segment *_add_segment(struct dm_tree_node *dnode, unsigned type, uint64_t size)
{
	struct load_segment *seg;

	if (!(seg = dm_pool_zalloc(dnode->dtree->mem, sizeof(*seg)))) {
		log_error("dtree node segment allocation failed");
		return NULL;
	}

	seg->type = type;
	seg->size = size;
	seg->area_count = 0;
	dm_list_init(&seg->areas);
	seg->stripe_size = 0;
	seg->persistent = 0;
	seg->chunk_size = 0;
	seg->cow = NULL;
	seg->origin = NULL;
	seg->merge = NULL;

	dm_list_add(&dnode->props.segs, &seg->list);
	dnode->props.segment_count++;

	return seg;
}

int dm_tree_node_add_snapshot_origin_target(struct dm_tree_node *dnode,
					       uint64_t size,
					       const char *origin_uuid)
{
	struct load_segment *seg;
	struct dm_tree_node *origin_node;

	if (!(seg = _add_segment(dnode, SEG_SNAPSHOT_ORIGIN, size)))
		return_0;

	if (!(origin_node = dm_tree_find_node_by_uuid(dnode->dtree, origin_uuid))) {
		log_error("Couldn't find snapshot origin uuid %s.", origin_uuid);
		return 0;
	}

	seg->origin = origin_node;
	if (!_link_tree_nodes(dnode, origin_node))
		return_0;

	/* Resume snapshot origins after new snapshots */
	dnode->activation_priority = 1;

	return 1;
}

static int _add_snapshot_target(struct dm_tree_node *node,
				   uint64_t size,
				   const char *origin_uuid,
				   const char *cow_uuid,
				   const char *merge_uuid,
				   int persistent,
				   uint32_t chunk_size)
{
	struct load_segment *seg;
	struct dm_tree_node *origin_node, *cow_node, *merge_node;
	unsigned seg_type;

	seg_type = !merge_uuid ? SEG_SNAPSHOT : SEG_SNAPSHOT_MERGE;

	if (!(seg = _add_segment(node, seg_type, size)))
		return_0;

	if (!(origin_node = dm_tree_find_node_by_uuid(node->dtree, origin_uuid))) {
		log_error("Couldn't find snapshot origin uuid %s.", origin_uuid);
		return 0;
	}

	seg->origin = origin_node;
	if (!_link_tree_nodes(node, origin_node))
		return_0;

	if (!(cow_node = dm_tree_find_node_by_uuid(node->dtree, cow_uuid))) {
		log_error("Couldn't find snapshot COW device uuid %s.", cow_uuid);
		return 0;
	}

	seg->cow = cow_node;
	if (!_link_tree_nodes(node, cow_node))
		return_0;

	seg->persistent = persistent ? 1 : 0;
	seg->chunk_size = chunk_size;

	if (merge_uuid) {
		if (!(merge_node = dm_tree_find_node_by_uuid(node->dtree, merge_uuid))) {
			/* not a pure error, merging snapshot may have been deactivated */
			log_verbose("Couldn't find merging snapshot uuid %s.", merge_uuid);
		} else {
			seg->merge = merge_node;
			/* must not link merging snapshot, would undermine activation_priority below */
		}

		/* Resume snapshot-merge (acting origin) after other snapshots */
		node->activation_priority = 1;
		if (seg->merge) {
			/* Resume merging snapshot after snapshot-merge */
			seg->merge->activation_priority = 2;
		}
	}

	return 1;
}


int dm_tree_node_add_snapshot_target(struct dm_tree_node *node,
				     uint64_t size,
				     const char *origin_uuid,
				     const char *cow_uuid,
				     int persistent,
				     uint32_t chunk_size)
{
	return _add_snapshot_target(node, size, origin_uuid, cow_uuid,
				    NULL, persistent, chunk_size);
}

int dm_tree_node_add_snapshot_merge_target(struct dm_tree_node *node,
					   uint64_t size,
					   const char *origin_uuid,
					   const char *cow_uuid,
					   const char *merge_uuid,
					   uint32_t chunk_size)
{
	return _add_snapshot_target(node, size, origin_uuid, cow_uuid,
				    merge_uuid, 1, chunk_size);
}

int dm_tree_node_add_error_target(struct dm_tree_node *node,
				     uint64_t size)
{
	if (!_add_segment(node, SEG_ERROR, size))
		return_0;

	return 1;
}

int dm_tree_node_add_zero_target(struct dm_tree_node *node,
				    uint64_t size)
{
	if (!_add_segment(node, SEG_ZERO, size))
		return_0;

	return 1;
}

int dm_tree_node_add_linear_target(struct dm_tree_node *node,
				      uint64_t size)
{
	if (!_add_segment(node, SEG_LINEAR, size))
		return_0;

	return 1;
}

int dm_tree_node_add_striped_target(struct dm_tree_node *node,
				       uint64_t size,
				       uint32_t stripe_size)
{
	struct load_segment *seg;

	if (!(seg = _add_segment(node, SEG_STRIPED, size)))
		return_0;

	seg->stripe_size = stripe_size;

	return 1;
}

int dm_tree_node_add_crypt_target(struct dm_tree_node *node,
				  uint64_t size,
				  const char *cipher,
				  const char *chainmode,
				  const char *iv,
				  uint64_t iv_offset,
				  const char *key)
{
	struct load_segment *seg;

	if (!(seg = _add_segment(node, SEG_CRYPT, size)))
		return_0;

	seg->cipher = cipher;
	seg->chainmode = chainmode;
	seg->iv = iv;
	seg->iv_offset = iv_offset;
	seg->key = key;

	return 1;
}

int dm_tree_node_add_mirror_target_log(struct dm_tree_node *node,
					  uint32_t region_size,
					  unsigned clustered,
					  const char *log_uuid,
					  unsigned area_count,
					  uint32_t flags)
{
	struct dm_tree_node *log_node = NULL;
	struct load_segment *seg;

	if (!node->props.segment_count) {
		log_error(INTERNAL_ERROR "Attempt to add target area to missing segment.");
		return 0;
	}

	seg = dm_list_item(dm_list_last(&node->props.segs), struct load_segment);

	if (log_uuid) {
		if (!(seg->uuid = dm_pool_strdup(node->dtree->mem, log_uuid))) {
			log_error("log uuid pool_strdup failed");
			return 0;
		}
		if ((flags & DM_CORELOG))
			/* For pvmove: immediate resume (for size validation) isn't needed. */
			node->props.delay_resume_if_new = 1;
		else {
			if (!(log_node = dm_tree_find_node_by_uuid(node->dtree, log_uuid))) {
				log_error("Couldn't find mirror log uuid %s.", log_uuid);
				return 0;
			}

			if (clustered)
				log_node->props.immediate_dev_node = 1;

			/* The kernel validates the size of disk logs. */
			/* FIXME Propagate to any devices below */
			log_node->props.delay_resume_if_new = 0;

			if (!_link_tree_nodes(node, log_node))
				return_0;
		}
	}

	seg->log = log_node;
	seg->region_size = region_size;
	seg->clustered = clustered;
	seg->mirror_area_count = area_count;
	seg->flags = flags;

	return 1;
}

int dm_tree_node_add_mirror_target(struct dm_tree_node *node,
				      uint64_t size)
{
	if (!_add_segment(node, SEG_MIRRORED, size))
		return_0;

	return 1;
}

int dm_tree_node_add_raid_target(struct dm_tree_node *node,
				 uint64_t size,
				 const char *raid_type,
				 uint32_t region_size,
				 uint32_t stripe_size,
				 uint64_t rebuilds,
				 uint64_t reserved2)
{
	int i;
	struct load_segment *seg = NULL;

	for (i = 0; dm_segtypes[i].target && !seg; i++)
		if (!strcmp(raid_type, dm_segtypes[i].target))
			if (!(seg = _add_segment(node,
						 dm_segtypes[i].type, size)))
				return_0;

	if (!seg)
		return_0;

	seg->region_size = region_size;
	seg->stripe_size = stripe_size;
	seg->area_count = 0;
	seg->rebuilds = rebuilds;

	return 1;
}

int dm_tree_node_add_replicator_target(struct dm_tree_node *node,
				       uint64_t size,
				       const char *rlog_uuid,
				       const char *rlog_type,
				       unsigned rsite_index,
				       dm_replicator_mode_t mode,
				       uint32_t async_timeout,
				       uint64_t fall_behind_data,
				       uint32_t fall_behind_ios)
{
	struct load_segment *rseg;
	struct replicator_site *rsite;

	/* Local site0 - adds replicator segment and links rlog device */
	if (rsite_index == REPLICATOR_LOCAL_SITE) {
		if (node->props.segment_count) {
			log_error(INTERNAL_ERROR "Attempt to add replicator segment to already used node.");
			return 0;
		}

		if (!(rseg = _add_segment(node, SEG_REPLICATOR, size)))
			return_0;

		if (!(rseg->log = dm_tree_find_node_by_uuid(node->dtree, rlog_uuid))) {
			log_error("Missing replicator log uuid %s.", rlog_uuid);
			return 0;
		}

		if (!_link_tree_nodes(node, rseg->log))
			return_0;

		if (strcmp(rlog_type, "ringbuffer") != 0) {
			log_error("Unsupported replicator log type %s.", rlog_type);
			return 0;
		}

		if (!(rseg->rlog_type = dm_pool_strdup(node->dtree->mem, rlog_type)))
			return_0;

		dm_list_init(&rseg->rsites);
		rseg->rdevice_count = 0;
		node->activation_priority = 1;
	}

	/* Add site to segment */
	if (mode == DM_REPLICATOR_SYNC
	    && (async_timeout || fall_behind_ios || fall_behind_data)) {
		log_error("Async parameters passed for synchronnous replicator.");
		return 0;
	}

	if (node->props.segment_count != 1) {
		log_error(INTERNAL_ERROR "Attempt to add remote site area before setting replicator log.");
		return 0;
	}

	rseg = dm_list_item(dm_list_last(&node->props.segs), struct load_segment);
	if (rseg->type != SEG_REPLICATOR) {
		log_error(INTERNAL_ERROR "Attempt to use non replicator segment %s.",
			  dm_segtypes[rseg->type].target);
		return 0;
	}

	if (!(rsite = dm_pool_zalloc(node->dtree->mem, sizeof(*rsite)))) {
		log_error("Failed to allocate remote site segment.");
		return 0;
	}

	dm_list_add(&rseg->rsites, &rsite->list);
	rseg->rsite_count++;

	rsite->mode = mode;
	rsite->async_timeout = async_timeout;
	rsite->fall_behind_data = fall_behind_data;
	rsite->fall_behind_ios = fall_behind_ios;
	rsite->rsite_index = rsite_index;

	return 1;
}

/* Appends device node to Replicator */
int dm_tree_node_add_replicator_dev_target(struct dm_tree_node *node,
					   uint64_t size,
					   const char *replicator_uuid,
					   uint64_t rdevice_index,
					   const char *rdev_uuid,
					   unsigned rsite_index,
					   const char *slog_uuid,
					   uint32_t slog_flags,
					   uint32_t slog_region_size)
{
	struct seg_area *area;
	struct load_segment *rseg;
	struct load_segment *rep_seg;

	if (rsite_index == REPLICATOR_LOCAL_SITE) {
		/* Site index for local target */
		if (!(rseg = _add_segment(node, SEG_REPLICATOR_DEV, size)))
			return_0;

		if (!(rseg->replicator = dm_tree_find_node_by_uuid(node->dtree, replicator_uuid))) {
			log_error("Missing replicator uuid %s.", replicator_uuid);
			return 0;
		}

		/* Local slink0 for replicator must be always initialized first */
		if (rseg->replicator->props.segment_count != 1) {
			log_error(INTERNAL_ERROR "Attempt to use non replicator segment.");
			return 0;
		}

		rep_seg = dm_list_item(dm_list_last(&rseg->replicator->props.segs), struct load_segment);
		if (rep_seg->type != SEG_REPLICATOR) {
			log_error(INTERNAL_ERROR "Attempt to use non replicator segment %s.",
				  dm_segtypes[rep_seg->type].target);
			return 0;
		}
		rep_seg->rdevice_count++;

		if (!_link_tree_nodes(node, rseg->replicator))
			return_0;

		rseg->rdevice_index = rdevice_index;
	} else {
		/* Local slink0 for replicator must be always initialized first */
		if (node->props.segment_count != 1) {
			log_error(INTERNAL_ERROR "Attempt to use non replicator-dev segment.");
			return 0;
		}

		rseg = dm_list_item(dm_list_last(&node->props.segs), struct load_segment);
		if (rseg->type != SEG_REPLICATOR_DEV) {
			log_error(INTERNAL_ERROR "Attempt to use non replicator-dev segment %s.",
				  dm_segtypes[rseg->type].target);
			return 0;
		}
	}

	if (!(slog_flags & DM_CORELOG) && !slog_uuid) {
		log_error("Unspecified sync log uuid.");
		return 0;
	}

	if (!dm_tree_node_add_target_area(node, NULL, rdev_uuid, 0))
		return_0;

	area = dm_list_item(dm_list_last(&rseg->areas), struct seg_area);

	if (!(slog_flags & DM_CORELOG)) {
		if (!(area->slog = dm_tree_find_node_by_uuid(node->dtree, slog_uuid))) {
			log_error("Couldn't find sync log uuid %s.", slog_uuid);
			return 0;
		}

		if (!_link_tree_nodes(node, area->slog))
			return_0;
	}

	area->flags = slog_flags;
	area->region_size = slog_region_size;
	area->rsite_index = rsite_index;

	return 1;
}

static int _thin_validate_device_id(uint32_t device_id)
{
	if (device_id > DM_THIN_MAX_DEVICE_ID) {
		log_error("Device id %u is higher then %u.",
			  device_id, DM_THIN_MAX_DEVICE_ID);
		return 0;
	}

	return 1;
}

int dm_tree_node_add_thin_pool_target(struct dm_tree_node *node,
				      uint64_t size,
				      uint64_t transaction_id,
				      const char *metadata_uuid,
				      const char *pool_uuid,
				      uint32_t data_block_size,
				      uint64_t low_water_mark,
				      unsigned skip_block_zeroing)
{
	struct load_segment *seg;

	if (data_block_size < DM_THIN_MIN_DATA_BLOCK_SIZE) {
		log_error("Data block size %u is lower then %u sectors.",
			  data_block_size, DM_THIN_MIN_DATA_BLOCK_SIZE);
		return 0;
	}

	if (data_block_size > DM_THIN_MAX_DATA_BLOCK_SIZE) {
		log_error("Data block size %u is higher then %u sectors.",
			  data_block_size, DM_THIN_MAX_DATA_BLOCK_SIZE);
		return 0;
	}

	if (!(seg = _add_segment(node, SEG_THIN_POOL, size)))
		return_0;

	if (!(seg->metadata = dm_tree_find_node_by_uuid(node->dtree, metadata_uuid))) {
		log_error("Missing metadata uuid %s.", metadata_uuid);
		return 0;
	}

	if (!_link_tree_nodes(node, seg->metadata))
		return_0;

	if (!(seg->pool = dm_tree_find_node_by_uuid(node->dtree, pool_uuid))) {
		log_error("Missing pool uuid %s.", pool_uuid);
		return 0;
	}

	if (!_link_tree_nodes(node, seg->pool))
		return_0;

	node->props.send_messages = 1;
	seg->transaction_id = transaction_id;
	seg->low_water_mark = low_water_mark;
	seg->data_block_size = data_block_size;
	seg->skip_block_zeroing = skip_block_zeroing;
	dm_list_init(&seg->thin_messages);

	return 1;
}

int dm_tree_node_add_thin_pool_message(struct dm_tree_node *node,
				       dm_thin_message_t type,
				       uint64_t id1, uint64_t id2)
{
	struct load_segment *seg;
	struct thin_message *tm;

	if (node->props.segment_count != 1) {
		log_error("Thin pool node must have only one segment.");
		return 0;
	}

	seg = dm_list_item(dm_list_last(&node->props.segs), struct load_segment);
	if (seg->type != SEG_THIN_POOL) {
		log_error("Thin pool node has segment type %s.",
			  dm_segtypes[seg->type].target);
		return 0;
	}

	if (!(tm = dm_pool_zalloc(node->dtree->mem, sizeof (*tm)))) {
		log_error("Failed to allocate thin message.");
		return 0;
	}

	switch (type) {
	case DM_THIN_MESSAGE_CREATE_SNAP:
		/* If the thin origin is active, it must be suspend first! */
		if (id1 == id2) {
			log_error("Cannot use same device id for origin and its snapshot.");
			return 0;
		}
		if (!_thin_validate_device_id(id1) ||
		    !_thin_validate_device_id(id2))
			return_0;
		tm->message.u.m_create_snap.device_id = id1;
		tm->message.u.m_create_snap.origin_id = id2;
		break;
	case DM_THIN_MESSAGE_CREATE_THIN:
		if (!_thin_validate_device_id(id1))
			return_0;
		tm->message.u.m_create_thin.device_id = id1;
		tm->expected_errno = EEXIST;
		break;
	case DM_THIN_MESSAGE_DELETE:
		if (!_thin_validate_device_id(id1))
			return_0;
		tm->message.u.m_delete.device_id = id1;
		tm->expected_errno = ENODATA;
		break;
	case DM_THIN_MESSAGE_TRIM:
		if (!_thin_validate_device_id(id1))
			return_0;
		tm->message.u.m_trim.device_id = id1;
		tm->message.u.m_trim.new_size = id2;
		break;
	case DM_THIN_MESSAGE_SET_TRANSACTION_ID:
		if ((id1 + 1) != id2) {
			log_error("New transaction id must be sequential.");
			return 0; /* FIXME: Maybe too strict here? */
		}
		if (id2 != seg->transaction_id) {
			log_error("Current transaction id is different from thin pool.");
			return 0; /* FIXME: Maybe too strict here? */
		}
		tm->message.u.m_set_transaction_id.current_id = id1;
		tm->message.u.m_set_transaction_id.new_id = id2;
		break;
	default:
		log_error("Unsupported message type %d.", (int) type);
		return 0;
	}

	tm->message.type = type;
	dm_list_add(&seg->thin_messages, &tm->list);

	return 1;
}

int dm_tree_node_add_thin_target(struct dm_tree_node *node,
				 uint64_t size,
				 const char *pool_uuid,
				 uint32_t device_id)
{
	struct dm_tree_node *pool;
	struct load_segment *seg;

	if (!(pool = dm_tree_find_node_by_uuid(node->dtree, pool_uuid))) {
		log_error("Missing thin pool uuid %s.", pool_uuid);
		return 0;
	}

	if (!_link_tree_nodes(node, pool))
		return_0;

	if (!_thin_validate_device_id(device_id))
		return_0;

	if (!(seg = _add_segment(node, SEG_THIN, size)))
		return_0;

	seg->pool = pool;
	seg->device_id = device_id;

	return 1;
}


int dm_get_status_thin_pool(struct dm_pool *mem, const char *params,
			    struct dm_status_thin_pool **status)
{
	struct dm_status_thin_pool *s;

	if (!(s = dm_pool_zalloc(mem, sizeof(struct dm_status_thin_pool)))) {
		log_error("Failed to allocate thin_pool status structure.");
		return 0;
	}

	/* FIXME: add support for held metadata root */
	if (sscanf(params, "%" PRIu64 " %" PRIu64 "/%" PRIu64 " %" PRIu64 "/%" PRIu64,
		   &s->transaction_id,
		   &s->used_metadata_blocks,
		   &s->total_metadata_blocks,
		   &s->used_data_blocks,
		   &s->total_data_blocks) != 5) {
		log_error("Failed to parse thin pool params: %s.", params);
		return 0;
	}

	*status = s;

	return 1;
}

int dm_get_status_thin(struct dm_pool *mem, const char *params,
		       struct dm_status_thin **status)
{
	struct dm_status_thin *s;

	if (!(s = dm_pool_zalloc(mem, sizeof(struct dm_status_thin)))) {
		log_error("Failed to allocate thin status structure.");
		return 0;
	}

	if (strchr(params, '-')) {
		s->mapped_sectors = 0;
		s->highest_mapped_sector = 0;
	} else if (sscanf(params, "%" PRIu64 " %" PRIu64,
		   &s->mapped_sectors,
		   &s->highest_mapped_sector) != 2) {
		log_error("Failed to parse thin params: %s.", params);
		return 0;
	}

	*status = s;

	return 1;
}

static int _add_area(struct dm_tree_node *node, struct load_segment *seg, struct dm_tree_node *dev_node, uint64_t offset)
{
	struct seg_area *area;

	if (!(area = dm_pool_zalloc(node->dtree->mem, sizeof (*area)))) {
		log_error("Failed to allocate target segment area.");
		return 0;
	}

	area->dev_node = dev_node;
	area->offset = offset;

	dm_list_add(&seg->areas, &area->list);
	seg->area_count++;

	return 1;
}

int dm_tree_node_add_target_area(struct dm_tree_node *node,
				    const char *dev_name,
				    const char *uuid,
				    uint64_t offset)
{
	struct load_segment *seg;
	struct stat info;
	struct dm_tree_node *dev_node;

	if ((!dev_name || !*dev_name) && (!uuid || !*uuid)) {
		log_error("dm_tree_node_add_target_area called without device");
		return 0;
	}

	if (uuid) {
		if (!(dev_node = dm_tree_find_node_by_uuid(node->dtree, uuid))) {
			log_error("Couldn't find area uuid %s.", uuid);
			return 0;
		}
		if (!_link_tree_nodes(node, dev_node))
			return_0;
	} else {
		if (stat(dev_name, &info) < 0) {
			log_error("Device %s not found.", dev_name);
			return 0;
		}

		if (!S_ISBLK(info.st_mode)) {
			log_error("Device %s is not a block device.", dev_name);
			return 0;
		}

		/* FIXME Check correct macro use */
		if (!(dev_node = _add_dev(node->dtree, node, MAJOR(info.st_rdev),
					  MINOR(info.st_rdev), 0)))
			return_0;
	}

	if (!node->props.segment_count) {
		log_error(INTERNAL_ERROR "Attempt to add target area to missing segment.");
		return 0;
	}

	seg = dm_list_item(dm_list_last(&node->props.segs), struct load_segment);

	if (!_add_area(node, seg, dev_node, offset))
		return_0;

	return 1;
}

int dm_tree_node_add_null_area(struct dm_tree_node *node, uint64_t offset)
{
	struct load_segment *seg;

	seg = dm_list_item(dm_list_last(&node->props.segs), struct load_segment);

	switch (seg->type) {
	case SEG_RAID1:
	case SEG_RAID4:
	case SEG_RAID5_LA:
	case SEG_RAID5_RA:
	case SEG_RAID5_LS:
	case SEG_RAID5_RS:
	case SEG_RAID6_ZR:
	case SEG_RAID6_NR:
	case SEG_RAID6_NC:
		break;
	default:
		log_error("dm_tree_node_add_null_area() called on an unsupported segment type");
		return 0;
	}

	if (!_add_area(node, seg, NULL, offset))
		return_0;

	return 1;
}
