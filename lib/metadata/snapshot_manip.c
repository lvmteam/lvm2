/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.  
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
 */

#include "lib.h"
#include "metadata.h"
#include "toolcontext.h"

int lv_is_origin(const struct logical_volume *lv)
{
	struct list *slh;
	struct snapshot *s;

	list_iterate(slh, &lv->vg->snapshots) {
		s = list_item(slh, struct snapshot_list)->snapshot;
		if (s->origin == lv)
			return 1;
	}

	return 0;
}

int lv_is_cow(const struct logical_volume *lv)
{
	struct list *slh;
	struct snapshot *s;

	list_iterate(slh, &lv->vg->snapshots) {
		s = list_item(slh, struct snapshot_list)->snapshot;
		if (s->cow == lv)
			return 1;
	}

	return 0;
}

struct snapshot *find_origin(const struct logical_volume *lv)
{
	struct list *slh;
	struct snapshot *s;

	list_iterate(slh, &lv->vg->snapshots) {
		s = list_item(slh, struct snapshot_list)->snapshot;
		if (s->origin == lv)
			return s;
	}

	return NULL;
}

struct snapshot *find_cow(const struct logical_volume *lv)
{
	struct list *slh;
	struct snapshot *s;

	list_iterate(slh, &lv->vg->snapshots) {
		s = list_item(slh, struct snapshot_list)->snapshot;
		if (s->cow == lv)
			return s;
	}

	return NULL;
}

struct list *find_snapshots(const struct logical_volume *lv)
{
	struct list *slh;
	struct list *snaplist;
	struct snapshot *s;
	struct snapshot_list *newsl;
	struct pool *mem = lv->vg->cmd->mem;

	if (!(snaplist = pool_alloc(mem, sizeof(*snaplist)))) {
		log_error("snapshot name list allocation failed");
		return NULL;
	}

	list_init(snaplist);

	list_iterate(slh, &lv->vg->snapshots) {
		s = list_item(slh, struct snapshot_list)->snapshot;
		if (!(s->origin == lv))
			continue;
		if (!(newsl = pool_alloc(mem, sizeof(*newsl)))) {
			log_error("snapshot_list structure allocation failed");
			pool_free(mem, snaplist);
			return NULL;
		}
		newsl->snapshot = s;
		list_add(snaplist, &newsl->list);
	}

	return snaplist;
}

int vg_add_snapshot(struct logical_volume *origin, struct logical_volume *cow,
		    int persistent, struct id *id, uint32_t extent_count,
		    uint32_t chunk_size)
{
	struct snapshot *s;
	struct snapshot_list *sl;
	struct pool *mem = origin->vg->cmd->mem;

	/*
	 * Is the cow device already being used ?
	 */
	if (lv_is_cow(cow)) {
		log_err("'%s' is already in use as a snapshot.", cow->name);
		return 0;
	}

	if (!(s = pool_alloc(mem, sizeof(*s)))) {
		stack;
		return 0;
	}

	s->persistent = persistent;
	s->chunk_size = chunk_size;
	s->le_count = extent_count;
	s->origin = origin;
	s->cow = cow;

	if (id)
		s->id = *id;
	else if (!id_create(&s->id)) {
		log_error("Snapshot UUID creation failed");
		return 0;
	}

	if (!(sl = pool_alloc(mem, sizeof(*sl)))) {
		stack;
		pool_free(mem, s);
		return 0;
	}

	cow->status &= ~VISIBLE_LV;
	sl->snapshot = s;
	list_add(&origin->vg->snapshots, &sl->list);
	origin->vg->snapshot_count++;

	return 1;
}

int vg_remove_snapshot(struct volume_group *vg, struct logical_volume *cow)
{
	struct list *slh;
	struct snapshot_list *sl;

	list_iterate(slh, &vg->snapshots) {
		sl = list_item(slh, struct snapshot_list);

		if (sl->snapshot->cow == cow) {
			list_del(slh);
			vg->snapshot_count--;
			return 1;
		}
	}

	/* fail */
	log_err("Asked to remove an unknown snapshot.");
	return 0;
}
