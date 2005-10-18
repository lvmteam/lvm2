/*
 * Copyright (C) 2005 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "libdm-targets.h"
#include "libdm-common.h"
#include "list.h"
#include "kdev_t.h"

#include <stdarg.h>
#include <sys/param.h>

#include <linux/dm-ioctl.h>

struct deptree_node {
	struct deptree *deptree;

        const char *name;
        const char *uuid;
        struct dm_info info;

        struct list uses;       	/* Nodes this node uses */
        struct list used_by;    	/* Nodes that use this node */
};

struct deptree {
	struct dm_pool *mem;
	struct dm_hash_table *devs;
	struct deptree_node root;
};

struct deptree_link {
	struct list list;
	struct deptree_node *node;
};

struct deptree *dm_deptree_create(void)
{
	struct deptree *deptree;

	if (!(deptree = dm_malloc(sizeof(*deptree)))) {
		log_error("dm_deptree_create malloc failed");
		return NULL;
	}

	memset(deptree, 0, sizeof(*deptree));
	deptree->root.deptree = deptree;
	list_init(&deptree->root.uses);
	list_init(&deptree->root.used_by);

	if (!(deptree->mem = dm_pool_create("deptree", 1024))) {
		log_error("deptree pool creation failed");
		dm_free(deptree);
		return NULL;
	}

	if (!(deptree->devs = dm_hash_create(8))) {
		log_error("deptree hash creation failed");
		dm_pool_destroy(deptree->mem);
		dm_free(deptree);
		return NULL;
	}

	return deptree;
}

void dm_deptree_free(struct deptree *deptree)
{
	if (!deptree)
		return;

	dm_hash_destroy(deptree->devs);
	dm_pool_destroy(deptree->mem);
	dm_free(deptree);
}

static int _nodes_are_linked(struct deptree_node *parent,
			     struct deptree_node *child)
{
	struct deptree_link *dlink;

	list_iterate_items(dlink, &parent->uses) {
		if (dlink->node == child)
			return 1;
	}

	return 0;
}

static int _link(struct list *list, struct deptree_node *node)
{
	struct deptree_link *dlink;

	if (!(dlink = dm_pool_alloc(node->deptree->mem, sizeof(*dlink)))) {
		log_error("deptree link allocation failed");
		return 0;
	}

	dlink->node = node;
	list_add(list, &dlink->list);

	return 1;
}

static int _link_nodes(struct deptree_node *parent,
		       struct deptree_node *child)
{
	if (_nodes_are_linked(parent, child))
		return 1;

	if (!_link(&parent->uses, child))
		return 0;

	if (!_link(&child->used_by, parent))
		return 0;

	return 1;
}

static void _unlink(struct list *list, struct deptree_node *node)
{
	struct deptree_link *dlink;

	list_iterate_items(dlink, list) {
		if (dlink->node == node) {
			list_del(&dlink->list);
			break;
		}
	}
}

static void _unlink_nodes(struct deptree_node *parent,
			  struct deptree_node *child)
{
	if (!_nodes_are_linked(parent, child))
		return;

	_unlink(&parent->uses, child);
	_unlink(&child->used_by, parent);
}

static void _remove_from_toplevel(struct deptree_node *node)
{
	return _unlink_nodes(&node->deptree->root, node);
}

static int _add_to_bottomlevel(struct deptree_node *node)
{
	return _link_nodes(node, &node->deptree->root);
}

static struct deptree_node *_create_deptree_node(struct deptree *deptree,
						 struct deptree_node *parent,
						 const char *name,
						 const char *uuid,
						 struct dm_info *info)
{
	struct deptree_node *node;
	uint64_t dev;

	if (!(node = dm_pool_zalloc(deptree->mem, sizeof(*node)))) {
		log_error("_create_deptree_node alloc failed");
		return NULL;
	}

	node->deptree = deptree;

	node->name = name;
	node->uuid = uuid;
	node->info = *info;

	list_init(&node->uses);
	list_init(&node->used_by);

	dev = MKDEV(info->major, info->minor);

	if (!dm_hash_insert_binary(deptree->devs, (const char *) &dev,
				sizeof(dev), node)) {
		log_error("deptree node hash insertion failed");
		dm_pool_free(deptree->mem, node);
		return NULL;
	}

	return node;
}

static struct deptree_node *_find_deptree_node(struct deptree *deptree,
					       uint32_t major, uint32_t minor)
{
	uint64_t dev = MKDEV(major, minor);

	return dm_hash_lookup_binary(deptree->devs, (const char *) &dev,
				  sizeof(dev));
}

static int _deps(struct dm_task **dmt, struct dm_pool *mem, uint32_t major, uint32_t minor,
		 const char **name, const char **uuid,
		 struct dm_info *info, struct dm_deps **deps)
{
	memset(info, 0, sizeof(*info));

	if (!dm_is_dm_major(major)) {
		*name = "";
		*uuid = "";
		*deps = NULL;
		info->major = major;
		info->minor = minor;
		info->exists = 0;
		return 1;
	}

	if (!(*dmt = dm_task_create(DM_DEVICE_DEPS))) {
		log_error("deps dm_task creation failed");
		return 0;
	}

	if (!dm_task_set_major(*dmt, major))
		goto failed;

	if (!dm_task_set_minor(*dmt, minor))
		goto failed;

	if (!dm_task_run(*dmt))
		goto failed;

	if (!dm_task_get_info(*dmt, info))
		goto failed;

	if (!info->exists) {
		*name = "";
		*uuid = "";
		*deps = NULL;
	} else {
		if (info->major != major) {
			log_error("Inconsistent deptree major number: %u != %u",
				  major, info->major);
			goto failed;
		}
		if (info->minor != minor) {
			log_error("Inconsistent deptree minor number: %u != %u",
				  minor, info->minor);
			goto failed;
		}
		if (!(*name = dm_pool_strdup(mem, dm_task_get_name(*dmt)))) {
			log_error("name pool_strdup failed");
			goto failed;
		}
		if (!(*uuid = dm_pool_strdup(mem, dm_task_get_uuid(*dmt)))) {
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

static int _add_dev(struct deptree *deptree, struct deptree_node *parent,
		    uint32_t major, uint32_t minor)
{
	struct dm_task *dmt = NULL;
	struct dm_info info;
	struct dm_deps *deps = NULL;
	const char *name = NULL;
	const char *uuid = NULL;
	struct deptree_node *node;
	uint32_t i;
	int r = 0;
	int new = 0;

	/* Already in tree? */
	if (!(node = _find_deptree_node(deptree, major, minor))) {
		if (!_deps(&dmt, deptree->mem, major, minor, &name, &uuid, &info, &deps))
			return 0;

		if (!(node = _create_deptree_node(deptree, node, name, uuid,
						  &info)))
			goto out;
		new = 1;
	}

	/* If new parent not root node, remove any existing root node parent */
	if (parent != &deptree->root)
		_remove_from_toplevel(node);

	/* Create link to parent.  Use root node only if no other parents. */
	if ((parent != &deptree->root) || !dm_deptree_node_num_children(node, 1))
		if (!_link_nodes(parent, node))
			goto out;

	/* If node was already in tree, no need to recurse. */
	if (!new)
		return 1;

	/* Can't recurse if not a mapped device or there are no dependencies */
	if (!node->info.exists || !deps->count) {
		if (!_add_to_bottomlevel(node))
			goto out;
		return 1;
	}

	/* Add dependencies to tree */
	for (i = 0; i < deps->count; i++)
		if (!_add_dev(deptree, node, MAJOR(deps->device[i]),
			      MINOR(deps->device[i])))
			goto out;

	r = 1;
out:
	if (dmt)
		dm_task_destroy(dmt);

	return r;
}

int dm_deptree_add_dev(struct deptree *deptree, uint32_t major, uint32_t minor)
{
	return _add_dev(deptree, &deptree->root, major, minor);
}

const char *dm_deptree_node_get_name(struct deptree_node *node)
{
	return node->info.exists ? node->name : "";
}

const char *dm_deptree_node_get_uuid(struct deptree_node *node)
{
	return node->info.exists ? node->uuid : "";
}

const struct dm_info *dm_deptree_node_get_info(struct deptree_node *node)
{
	return &node->info;
}

int dm_deptree_node_num_children(struct deptree_node *node, uint32_t inverted)
{
	if (inverted) {
		if (_nodes_are_linked(&node->deptree->root, node))
			return 0;
		return list_size(&node->used_by);
	}

	if (_nodes_are_linked(node, &node->deptree->root))
		return 0;

	return list_size(&node->uses);
}

/*
 * Set major and minor to zero for root of tree.
 */
struct deptree_node *dm_deptree_find_node(struct deptree *deptree,
					  uint32_t major,
					  uint32_t minor)
{
	if (!major && !minor)
		return &deptree->root;

	return _find_deptree_node(deptree, major, minor);
}

/*
 * First time set *handle to NULL.
 * Set inverted to invert the tree.
 */
struct deptree_node *dm_deptree_next_child(void **handle,
					   struct deptree_node *parent,
					   uint32_t inverted)
{
	struct list **dlink = (struct list **) handle;
	struct list *use_list;

	if (inverted)
		use_list = &parent->used_by;
	else
		use_list = &parent->uses;

	if (!*dlink)
		*dlink = list_first(use_list);
	else
		*dlink = list_next(use_list, *dlink);

	return (*dlink) ? list_item(*dlink, struct deptree_link)->node : NULL;
}

/*
 * Deactivate a device with its dependencies if the uuid prefix matches
 */
static int _info_by_dev(uint32_t major, uint32_t minor, struct dm_info *info)
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

	if ((r = dm_task_run(dmt)))
		r = dm_task_get_info(dmt, info);

	dm_task_destroy(dmt);

	return r;
}

static int _deactivate_node(const char *name, uint32_t major, uint32_t minor)
{
	struct dm_task *dmt;
	int r;

	log_verbose("Removing %s (%" PRIu32 ":%" PRIu32 ")", name, major, minor);

	if (!(dmt = dm_task_create(DM_DEVICE_REMOVE))) {
		log_error("Deactivation dm_task creation failed for %s", name);
		return 0;
	}

	if (!dm_task_set_major(dmt, major) || !dm_task_set_minor(dmt, minor)) {
		log_error("Failed to set device number for %s deactivation", name);
		dm_task_destroy(dmt);
		return 0;
	}

	if (!dm_task_no_open_count(dmt))
		log_error("Failed to disable open_count");

	r = dm_task_run(dmt);

	dm_task_destroy(dmt);

	return r;
}

int dm_deptree_deactivate_children(struct deptree_node *dnode, const char *uuid_prefix, size_t uuid_prefix_len)
{
	void *handle = NULL;
	struct deptree_node *child = dnode;
	struct dm_info info;
	const struct dm_info *dinfo;
	const char *name;
	const char *uuid;

	while ((child = dm_deptree_next_child(&handle, dnode, 0))) {
		if (!(dinfo = dm_deptree_node_get_info(child))) {
			stack;
			continue;
		}

		if (!(name = dm_deptree_node_get_name(child))) {
			stack;
			continue;
		}

		if (!(uuid = dm_deptree_node_get_uuid(child))) {
			stack;
			continue;
		}

		/* Ignore if it doesn't belong to this VG */
		if (strncmp(uuid, uuid_prefix, uuid_prefix_len))
			continue;

		/* Refresh open_count */
		if (!_info_by_dev(dinfo->major, dinfo->minor, &info) ||
		    !info.exists || info.open_count)
			continue;

		if (!_deactivate_node(name, info.major, info.minor)) {
			log_error("Unable to deactivate %s (%" PRIu32
				  ":%" PRIu32 ")", name, info.major,
				  info.minor);
			continue;
		}

		if (dm_deptree_node_num_children(child, 0))
			dm_deptree_deactivate_children(child, uuid_prefix, uuid_prefix_len);
	}

	return 1;
}
