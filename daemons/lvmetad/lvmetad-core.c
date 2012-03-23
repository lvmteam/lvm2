/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define _XOPEN_SOURCE 500  /* pthread */

#include "configure.h"
#include "daemon-shared.h"
#include "daemon-server.h"

#include <assert.h>
#include <pthread.h>
#include <malloc.h>
#include <stdint.h>
#include <unistd.h>

typedef struct {
	struct dm_hash_table *pvid_to_pvmeta;
	struct dm_hash_table *device_to_pvid; /* shares locks with above */

	struct dm_hash_table *vgid_to_metadata;
	struct dm_hash_table *vgid_to_vgname;
	struct dm_hash_table *vgname_to_vgid;
	struct dm_hash_table *pvid_to_vgid;
	struct {
		struct dm_hash_table *vg;
		pthread_mutex_t pvid_to_pvmeta;
		pthread_mutex_t vgid_to_metadata;
		pthread_mutex_t pvid_to_vgid;
	} lock;
} lvmetad_state;

__attribute__ ((format(printf, 1, 2)))
static void debug(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[D %lu] ", pthread_self());
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fflush(stderr);
}

static int debug_cft_line(const char *line, void *baton) {
	fprintf(stderr, "| %s\n", line);
	return 0;
}

static void debug_cft(const char *id, struct dm_config_node *n) {
	debug("%s\n", id);
	dm_config_write_node(n, &debug_cft_line, NULL);
}

static void lock_pvid_to_pvmeta(lvmetad_state *s) {
	pthread_mutex_lock(&s->lock.pvid_to_pvmeta); }
static void unlock_pvid_to_pvmeta(lvmetad_state *s) {
	pthread_mutex_unlock(&s->lock.pvid_to_pvmeta); }

static void lock_vgid_to_metadata(lvmetad_state *s) {
	pthread_mutex_lock(&s->lock.vgid_to_metadata); }
static void unlock_vgid_to_metadata(lvmetad_state *s) {
	pthread_mutex_unlock(&s->lock.vgid_to_metadata); }

static void lock_pvid_to_vgid(lvmetad_state *s) {
	pthread_mutex_lock(&s->lock.pvid_to_vgid); }
static void unlock_pvid_to_vgid(lvmetad_state *s) {
	pthread_mutex_unlock(&s->lock.pvid_to_vgid); }

/*
 * TODO: It may be beneficial to clean up the vg lock hash from time to time,
 * since if we have many "rogue" requests for nonexistent things, we will keep
 * allocating memory that we never release. Not good.
 */
static struct dm_config_tree *lock_vg(lvmetad_state *s, const char *id) {
	pthread_mutex_t *vg;
	struct dm_config_tree *cft;

	lock_vgid_to_metadata(s);
	vg = dm_hash_lookup(s->lock.vg, id);
	if (!vg) {
		pthread_mutexattr_t rec;
		pthread_mutexattr_init(&rec);
		pthread_mutexattr_settype(&rec, PTHREAD_MUTEX_RECURSIVE_NP);
		if (!(vg = malloc(sizeof(pthread_mutex_t))))
                        return NULL;
		pthread_mutex_init(vg, &rec);
		if (!dm_hash_insert(s->lock.vg, id, vg)) {
			free(vg);
			return NULL;
		}
	}
	// debug("lock VG %s\n", id);
	pthread_mutex_lock(vg);
	cft = dm_hash_lookup(s->vgid_to_metadata, id);
	unlock_vgid_to_metadata(s);
	return cft;
}

static void unlock_vg(lvmetad_state *s, const char *id) {
	pthread_mutex_t *vg;

	// debug("unlock VG %s\n", id);
	lock_vgid_to_metadata(s); /* someone might be changing the s->lock.vg structure right
				   * now, so avoid stepping on each other's toes */
	if ((vg = dm_hash_lookup(s->lock.vg, id)))
		pthread_mutex_unlock(vg);
	unlock_vgid_to_metadata(s);
}

static struct dm_config_node *pvs(struct dm_config_node *vg)
{
	struct dm_config_node *pv = dm_config_find_node(vg, "metadata/physical_volumes");
	if (pv)
		pv = pv->child;
	return pv;
}

/*
 * TODO: This set_flag function is pretty generic and might make sense in a
 * library here or there.
 */
static int set_flag(struct dm_config_tree *cft, struct dm_config_node *parent,
		     const char *field, const char *flag, int want) {
	struct dm_config_value *value = NULL, *pred = NULL;
	struct dm_config_node *node = dm_config_find_node(parent->child, field);
	struct dm_config_value *new;

	if (node)
		value = node->v;

	while (value && value->type != DM_CFG_EMPTY_ARRAY && strcmp(value->v.str, flag)) {
		pred = value;
		value = value->next;
	}

	if (value && want)
		return 1;

	if (!value && !want)
		return 1;

	if (value && !want) {
		if (pred) {
			pred->next = value->next;
		} else if (value == node->v && value->next) {
			node->v = value->next;
		} else {
			node->v->type = DM_CFG_EMPTY_ARRAY;
		}
	}

	if (!value && want) {
		if (!node) {
			if (!(node = dm_config_create_node(cft, field)))
				return 0;
			node->sib = parent->child;
			if (!(node->v = dm_config_create_value(cft)))
				return 0;
			node->v->type = DM_CFG_EMPTY_ARRAY;
			node->parent = parent;
			parent->child = node;
		}
		if (!(new = dm_config_create_value(cft))) {
			/* FIXME error reporting */
			return 0;
		}
		new->type = DM_CFG_STRING;
		new->v.str = flag;
		new->next = node->v;
		node->v = new;
	}

	return 1;
}

static struct dm_config_node *make_config_node(struct dm_config_tree *cft,
					       const char *key,
					       struct dm_config_node *parent,
					       struct dm_config_node *pre_sib)
{
	struct dm_config_node *cn;

	if (!(cn = dm_config_create_node(cft, key)))
		return NULL;

	cn->parent = parent;
	cn->sib = NULL;
	cn->v = NULL;
	cn->child = NULL;

	if (parent && parent->child && !pre_sib) { /* find the last one */
		pre_sib = parent->child;
		while (pre_sib && pre_sib->sib)
			pre_sib = pre_sib->sib;
	}

	if (parent && !parent->child)
		parent->child = cn;
	if (pre_sib)
		pre_sib->sib = cn;

	return cn;
}

static struct dm_config_node *make_text_node(struct dm_config_tree *cft,
					     const char *key,
					     const char *value,
					     struct dm_config_node *parent,
					     struct dm_config_node *pre_sib)
{
	struct dm_config_node *cn;

	if (!(cn = make_config_node(cft, key, parent, pre_sib)) ||
	    !(cn->v = dm_config_create_value(cft)))
		return NULL;

	cn->v->type = DM_CFG_STRING;
	cn->v->v.str = value;
	return cn;
}

#if 0
static struct dm_config_node *make_int_node(struct dm_config_tree *cft,
					    const char *key,
					    int64_t value,
					    struct dm_config_node *parent,
					    struct dm_config_node *pre_sib)
{
	struct dm_config_node *cn;

	if (!(cn = make_config_node(cft, key, parent, pre_sib)) ||
	    !(cn->v = dm_config_create_value(cft)))
		return NULL;

	cn->v->type = DM_CFG_INT;
	cn->v->v.i = value;
	return cn;
}
#endif

static void filter_metadata(struct dm_config_node *vg) {
	struct dm_config_node *pv = pvs(vg);
	while (pv) {
		struct dm_config_node *item = pv->child;
		while (item) {
			/* Remove the advisory device nodes. */
			if (item->sib && !strcmp(item->sib->key, "device"))
				item->sib = item->sib->sib;
			item = item->sib;
		}
		pv = pv->sib;
	}
	vg->sib = NULL; /* Drop any trailing garbage. */
}

static void merge_pvmeta(struct dm_config_node *pv, struct dm_config_node *pvmeta)
{
	struct dm_config_node *tmp;

	if (!pvmeta)
		return;

	tmp = pvmeta;
	while (tmp->sib) {
		/* drop the redundant ID and dev_size nodes */
		if (!strcmp(tmp->sib->key, "id") || !strcmp(tmp->sib->key, "dev_size"))
			tmp->sib = tmp->sib->sib;
		if (!tmp->sib) break;
		tmp = tmp->sib;
		tmp->parent = pv;
	}
	tmp->sib = pv->child;
	pv->child = pvmeta;
	pvmeta->parent = pv;
}

/* Either the "big" vgs lock, or a per-vg lock needs to be held before entering
 * this function. */
static int update_pv_status(lvmetad_state *s,
			    struct dm_config_tree *cft,
			    struct dm_config_node *vg, int act)
{
	struct dm_config_node *pv;
	int complete = 1;
	const char *uuid;
	struct dm_config_tree *pvmeta;

	lock_pvid_to_pvmeta(s);

	for (pv = pvs(vg); pv; pv = pv->sib) {
		if (!(uuid = dm_config_find_str(pv->child, "id", NULL)))
			continue;

		pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, uuid);
		if (act) {
			set_flag(cft, pv, "status", "MISSING", !pvmeta);
			if (pvmeta) {
				struct dm_config_node *pvmeta_cn =
					dm_config_clone_node(cft, pvmeta->root->child, 1);
				merge_pvmeta(pv, pvmeta_cn);
			}
		}
		if (!pvmeta) {
			complete = 0;
			if (!act) { /* optimisation */
				unlock_pvid_to_pvmeta(s);
				return complete;
			}
		}
	}
	unlock_pvid_to_pvmeta(s);

	return complete;
}

static struct dm_config_node *make_pv_node(lvmetad_state *s, const char *pvid,
					   struct dm_config_tree *cft,
					   struct dm_config_node *parent,
					   struct dm_config_node *pre_sib)
{
	struct dm_config_tree *pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, pvid);
	const char *vgid = dm_hash_lookup(s->pvid_to_vgid, pvid), *vgname = NULL;
	struct dm_config_node *pv;
	struct dm_config_node *cn = NULL;

	if (!pvmeta)
		return NULL;

	if (vgid) {
		lock_vgid_to_metadata(s); // XXX
		vgname = dm_hash_lookup(s->vgid_to_vgname, vgid);
		unlock_vgid_to_metadata(s);
	}

	/* Nick the pvmeta config tree. */
	if (!(pv = dm_config_clone_node(cft, pvmeta->root, 0)))
		return 0;

	if (pre_sib)
		pre_sib->sib = pv;
	if (parent && !parent->child)
		parent->child = pv;
	pv->parent = parent;
	pv->key = pvid;

	/* Add the "variable" bits to it. */

	if (vgid && strcmp(vgid, "#orphan"))
		cn = make_text_node(cft, "vgid", vgid, pv, cn);
	if (vgname)
		cn = make_text_node(cft, "vgname", vgname, pv, cn);

	return pv;
}

static response pv_list(lvmetad_state *s, request r)
{
	struct dm_config_node *cn = NULL, *cn_pvs;
	struct dm_hash_node *n;
	const char *id;
	response res = { .buffer = NULL };

	if (!(res.cft = dm_config_create()))
		return res; /* FIXME error reporting */

	/* The response field */
	res.cft->root = make_text_node(res.cft, "response", "OK", NULL, NULL);
	cn_pvs = make_config_node(res.cft, "physical_volumes", NULL, res.cft->root);

	lock_pvid_to_pvmeta(s);

	for (n = dm_hash_get_first(s->pvid_to_pvmeta); n;
	     n = dm_hash_get_next(s->pvid_to_pvmeta, n)) {
		id = dm_hash_get_key(s->pvid_to_pvmeta, n);
		cn = make_pv_node(s, id, res.cft, cn_pvs, cn);
	}

	unlock_pvid_to_pvmeta(s);

	return res;
}

static response pv_lookup(lvmetad_state *s, request r)
{
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	int64_t devt = daemon_request_int(r, "device", 0);
	response res = { .buffer = NULL };
	struct dm_config_node *pv;

	if (!pvid && !devt)
		return daemon_reply_simple("failed", "reason = %s", "need PVID or device", NULL);

	if (!(res.cft = dm_config_create()))
		return daemon_reply_simple("failed", "reason = %s", "out of memory", NULL);

	if (!(res.cft->root = make_text_node(res.cft, "response", "OK", NULL, NULL)))
		return daemon_reply_simple("failed", "reason = %s", "out of memory", NULL);

	lock_pvid_to_pvmeta(s);
	if (!pvid && devt)
		pvid = dm_hash_lookup_binary(s->device_to_pvid, &devt, sizeof(devt));

	if (!pvid) {
		debug("pv_lookup: could not find device %" PRIu64 "\n", devt);
		unlock_pvid_to_pvmeta(s);
		dm_config_destroy(res.cft);
		return daemon_reply_simple("unknown", "reason = %s", "device not found", NULL);
	}

	pv = make_pv_node(s, pvid, res.cft, NULL, res.cft->root);
	if (!pv) {
		unlock_pvid_to_pvmeta(s);
		dm_config_destroy(res.cft);
		return daemon_reply_simple("unknown", "reason = %s", "PV not found", NULL);
	}

	pv->key = "physical_volume";
	unlock_pvid_to_pvmeta(s);

	return res;
}

static response vg_list(lvmetad_state *s, request r)
{
	struct dm_config_node *cn, *cn_vgs, *cn_last = NULL;
	struct dm_hash_node *n;
	const char *id;
	const char *name;
	response res = { .buffer = NULL };
	if (!(res.cft = dm_config_create()))
                goto bad; /* FIXME: better error reporting */

	/* The response field */
	res.cft->root = cn = dm_config_create_node(res.cft, "response");
	if (!cn)
                goto bad; /* FIXME */
	cn->parent = res.cft->root;
	if (!(cn->v = dm_config_create_value(res.cft)))
		goto bad; /* FIXME */

	cn->v->type = DM_CFG_STRING;
	cn->v->v.str = "OK";

	cn_vgs = cn = cn->sib = dm_config_create_node(res.cft, "volume_groups");
	if (!cn_vgs)
		goto bad; /* FIXME */

	cn->parent = res.cft->root;
	cn->v = NULL;
	cn->child = NULL;

	lock_vgid_to_metadata(s);

	n = dm_hash_get_first(s->vgid_to_vgname);
	while (n) {
		id = dm_hash_get_key(s->vgid_to_vgname, n),
		name = dm_hash_get_data(s->vgid_to_vgname, n);

		if (!(cn = dm_config_create_node(res.cft, id)))
			goto bad; /* FIXME */

		if (cn_last)
			cn_last->sib = cn;

		cn->parent = cn_vgs;
		cn->sib = NULL;
		cn->v = NULL;

		if (!(cn->child = dm_config_create_node(res.cft, "name")))
			goto bad; /* FIXME */

		cn->child->parent = cn;
		cn->child->sib = 0;
		if (!(cn->child->v = dm_config_create_value(res.cft)))
			goto bad; /* FIXME */

		cn->child->v->type = DM_CFG_STRING;
		cn->child->v->v.str = name;

		if (!cn_vgs->child)
			cn_vgs->child = cn;
		cn_last = cn;

		n = dm_hash_get_next(s->vgid_to_vgname, n);
	}

	unlock_vgid_to_metadata(s);
bad:
	return res;
}

static response vg_lookup(lvmetad_state *s, request r)
{
	struct dm_config_tree *cft;
	struct dm_config_node *metadata, *n;
	response res = { .buffer = NULL };

	const char *uuid = daemon_request_str(r, "uuid", NULL);
	const char *name = daemon_request_str(r, "name", NULL);

	debug("vg_lookup: uuid = %s, name = %s\n", uuid, name);

	if (!uuid || !name) {
		lock_vgid_to_metadata(s);
		if (name && !uuid)
			uuid = dm_hash_lookup(s->vgname_to_vgid, name);
		if (uuid && !name)
			name = dm_hash_lookup(s->vgid_to_vgname, uuid);
		unlock_vgid_to_metadata(s);
	}

	debug("vg_lookup: updated uuid = %s, name = %s\n", uuid, name);

	if (!uuid)
		return daemon_reply_simple("unknown", "reason = %s", "VG not found", NULL);

	cft = lock_vg(s, uuid);
	if (!cft || !cft->root) {
		unlock_vg(s, uuid);
		return daemon_reply_simple("unknown", "reason = %s", "UUID not found", NULL);
	}

	metadata = cft->root;
	if (!(res.cft = dm_config_create()))
		goto bad;

	/* The response field */
	if (!(res.cft->root = n = dm_config_create_node(res.cft, "response")))
		goto bad;

	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = "OK";

	if (!(n = n->sib = dm_config_create_node(res.cft, "name")))
		goto bad;
	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = name;

	/* The metadata section */
	if (!(n = n->sib = dm_config_clone_node(res.cft, metadata, 1)))
		goto bad;
	n->parent = res.cft->root;
	res.error = 0;
	unlock_vg(s, uuid);

	update_pv_status(s, res.cft, n, 1); /* FIXME report errors */

	return res;
bad:
	unlock_vg(s, uuid);
	return daemon_reply_simple("failed", "reason = %s", "Out of memory", NULL);
}

static int compare_value(struct dm_config_value *a, struct dm_config_value *b)
{
	int r = 0;

	if (a->type > b->type)
		return 1;
	if (a->type < b->type)
		return -1;

	switch (a->type) {
	case DM_CFG_STRING: r = strcmp(a->v.str, b->v.str); break;
	case DM_CFG_FLOAT: r = (a->v.f == b->v.f) ? 0 : (a->v.f > b->v.f) ? 1 : -1; break;
	case DM_CFG_INT: r = (a->v.i == b->v.i) ? 0 : (a->v.i > b->v.i) ? 1 : -1; break;
	case DM_CFG_EMPTY_ARRAY: return 0;
	}

	if (r == 0 && a->next && b->next)
		r = compare_value(a->next, b->next);
	return r;
}

static int compare_config(struct dm_config_node *a, struct dm_config_node *b)
{
	int result = 0;
	if (a->v && b->v)
		result = compare_value(a->v, b->v);
	if (a->v && !b->v)
		result = 1;
	if (!a->v && b->v)
		result = -1;
	if (a->child && b->child)
		result = compare_config(a->child, b->child);

	if (result) {
		debug("config inequality at %s / %s\n", a->key, b->key);
		return result;
	}

	if (a->sib && b->sib)
		result = compare_config(a->sib, b->sib);
	if (a->sib && !b->sib)
		result = 1;
	if (!a->sib && b->sib)
		result = -1;

	return result;
}

static int vg_remove_if_missing(lvmetad_state *s, const char *vgid);

/* You need to be holding the pvid_to_vgid lock already to call this. */
static int update_pvid_to_vgid(lvmetad_state *s, struct dm_config_tree *vg,
			       const char *vgid, int nuke_empty)
{
	struct dm_config_node *pv;
	struct dm_hash_table *to_check;
	struct dm_hash_node *n;
	const char *pvid;
	const char *vgid_old;
	const char *check_vgid;
	int r = 0;

	if (!vgid)
		return 0;

	if (!(to_check = dm_hash_create(32)))
		return 0;

	for (pv = pvs(vg->root); pv; pv = pv->sib) {
		if (!(pvid = dm_config_find_str(pv->child, "id", NULL)))
			continue;

		if (nuke_empty &&
		    (vgid_old = dm_hash_lookup(s->pvid_to_vgid, pvid)) &&
		    !dm_hash_insert(to_check, vgid_old, (void*) 1))
			goto out;

		if (!dm_hash_insert(s->pvid_to_vgid, pvid, (void*) vgid))
			goto out;

		debug("remap PV %s to VG %s\n", pvid, vgid);
	}

	for (n = dm_hash_get_first(to_check); n;
	     n = dm_hash_get_next(to_check, n)) {
		check_vgid = dm_hash_get_key(to_check, n);
		lock_vg(s, check_vgid);
		vg_remove_if_missing(s, check_vgid);
		unlock_vg(s, check_vgid);
	}

	r = 1;
    out:
	dm_hash_destroy(to_check);

	return r;
}

/* A pvid map lock needs to be held if update_pvids = 1. */
static int remove_metadata(lvmetad_state *s, const char *vgid, int update_pvids)
{
	struct dm_config_tree *old;
	const char *oldname;
	lock_vgid_to_metadata(s);
	old = dm_hash_lookup(s->vgid_to_metadata, vgid);
	oldname = dm_hash_lookup(s->vgid_to_vgname, vgid);
	unlock_vgid_to_metadata(s);

	if (!old)
		return 0;
	assert(oldname);

	if (update_pvids)
		/* FIXME: What should happen when update fails */
		update_pvid_to_vgid(s, old, "#orphan", 0);
	/* need to update what we have since we found a newer version */
	dm_hash_remove(s->vgid_to_metadata, vgid);
	dm_hash_remove(s->vgid_to_vgname, vgid);
	dm_hash_remove(s->vgname_to_vgid, oldname);
	dm_config_destroy(old);
	return 1;
}

/* The VG must be locked. */
static int vg_remove_if_missing(lvmetad_state *s, const char *vgid)
{
	struct dm_config_tree *vg;
	struct dm_config_node *pv;
	const char *vgid_check;
	const char *pvid;
	int missing = 1;

	if (!vgid)
		return 0;

	if (!(vg = dm_hash_lookup(s->vgid_to_metadata, vgid)))
		return 1;

	lock_pvid_to_pvmeta(s);
	for (pv = pvs(vg->root); pv; pv = pv->sib) {
		if (!(pvid = dm_config_find_str(pv->child, "id", NULL)))
			continue;

		if ((vgid_check = dm_hash_lookup(s->pvid_to_vgid, pvid)) &&
		    dm_hash_lookup(s->pvid_to_pvmeta, pvid) &&
		    !strcmp(vgid, vgid_check))
			missing = 0; /* at least one PV is around */
	}

	if (missing) {
		debug("nuking VG %s\n", vgid);
		remove_metadata(s, vgid, 0);
	}

	unlock_pvid_to_pvmeta(s);

	return 1;
}

/* No locks need to be held. The pointers are never used outside of the scope of
 * this function, so they can be safely destroyed after update_metadata returns
 * (anything that might have been retained is copied). */
static int update_metadata(lvmetad_state *s, const char *name, const char *_vgid,
			   struct dm_config_node *metadata)
{
	struct dm_config_tree *cft;
	struct dm_config_tree *old;
	int retval = 0;
	int seq;
	int haveseq = -1;
	const char *oldname = NULL;
	const char *vgid;
	char *cfgname;

	lock_vgid_to_metadata(s);
	old = dm_hash_lookup(s->vgid_to_metadata, _vgid);
	lock_vg(s, _vgid);
	unlock_vgid_to_metadata(s);

	seq = dm_config_find_int(metadata, "metadata/seqno", -1);

	if (old) {
		haveseq = dm_config_find_int(old->root, "metadata/seqno", -1);
		oldname = dm_hash_lookup(s->vgid_to_vgname, _vgid);
		assert(oldname);
	}

	if (seq < 0)
		goto out;

	filter_metadata(metadata); /* sanitize */

	if (seq == haveseq) {
		retval = 1;
		if (compare_config(metadata, old->root))
			retval = 0;
		debug("Not updating metadata for %s at %d (%s)\n", _vgid, haveseq,
		      retval ? "ok" : "MISMATCH");
		if (!retval) {
			debug_cft("OLD: ", old->root);
			debug_cft("NEW: ", metadata);
		}
		goto out;
	}

	if (seq < haveseq) {
		debug("Refusing to update metadata for %s at %d to %d\n", _vgid, haveseq, seq);
		/* TODO: notify the client that their metadata is out of date? */
		retval = 1;
		goto out;
	}

	if (!(cft = dm_config_create()) ||
	    !(cft->root = dm_config_clone_node(cft, metadata, 0))) {
		debug("Out of memory\n");
		goto out;
	}

	vgid = dm_config_find_str(cft->root, "metadata/id", NULL);

	if (!vgid || !name) {
		debug("Name '%s' or uuid '%s' missing!\n", name, vgid);
		goto out;
	}

	lock_pvid_to_vgid(s);

	if (haveseq >= 0 && haveseq < seq) {
		debug("Updating metadata for %s at %d to %d\n", _vgid, haveseq, seq);
		/* temporarily orphan all of our PVs */
		remove_metadata(s, vgid, 1);
	}

	lock_vgid_to_metadata(s);
	debug("Mapping %s to %s\n", vgid, name);

	retval = ((cfgname = dm_pool_strdup(dm_config_memory(cft), name)) &&
		  dm_hash_insert(s->vgid_to_metadata, vgid, cft) &&
		  dm_hash_insert(s->vgid_to_vgname, vgid, cfgname) &&
		  dm_hash_insert(s->vgname_to_vgid, name, (void*) vgid)) ? 1 : 0;
	unlock_vgid_to_metadata(s);

	if (retval)
		/* FIXME: What should happen when update fails */
		retval = update_pvid_to_vgid(s, cft, vgid, 1);

	unlock_pvid_to_vgid(s);
out:
	unlock_vg(s, _vgid);
	return retval;
}

static response pv_gone(lvmetad_state *s, request r)
{
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	int64_t device = daemon_request_int(r, "device", 0);
	struct dm_config_tree *pvmeta;

	debug("pv_gone: %s / %" PRIu64 "\n", pvid, device);

	lock_pvid_to_pvmeta(s);
	if (!pvid && device > 0)
		pvid = dm_hash_lookup_binary(s->device_to_pvid, &device, sizeof(device));
	if (!pvid) {
		unlock_pvid_to_pvmeta(s);
		return daemon_reply_simple("unknown", "reason = %s", "device not in cache", NULL);
	}

	debug("pv_gone (updated): %s / %" PRIu64 "\n", pvid, device);

	pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, pvid);
	dm_hash_remove_binary(s->device_to_pvid, &device, sizeof(device));
	dm_hash_remove(s->pvid_to_pvmeta, pvid);
	vg_remove_if_missing(s, dm_hash_lookup(s->pvid_to_vgid, pvid));
	unlock_pvid_to_pvmeta(s);

	if (pvmeta) {
		dm_config_destroy(pvmeta);
		return daemon_reply_simple("OK", NULL);
	} else
		return daemon_reply_simple("unknown", "reason = %s", "PVID does not exist", NULL);
}

static response pv_found(lvmetad_state *s, request r)
{
	struct dm_config_node *metadata = dm_config_find_node(r.cft->root, "metadata");
	const char *pvid = daemon_request_str(r, "pvmeta/id", NULL);
	const char *vgname = daemon_request_str(r, "vgname", NULL);
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);
	struct dm_config_node *pvmeta = dm_config_find_node(r.cft->root, "pvmeta");
	uint64_t device;
	struct dm_config_tree *cft, *pvmeta_old = NULL;
	const char *old;
	const char *pvid_dup;
	int complete = 0, orphan = 0;

	if (!pvid)
		return daemon_reply_simple("failed", "reason = %s", "need PV UUID", NULL);
	if (!pvmeta)
		return daemon_reply_simple("failed", "reason = %s", "need PV metadata", NULL);

	if (!dm_config_get_uint64(pvmeta, "pvmeta/device", &device))
		return daemon_reply_simple("failed", "reason = %s", "need PV device number", NULL);

	debug("pv_found %s, vgid = %s, device = %" PRIu64 "\n", pvid, vgid, device);

	lock_pvid_to_pvmeta(s);

	if ((old = dm_hash_lookup_binary(s->device_to_pvid, &device, sizeof(device)))) {
		pvmeta_old = dm_hash_lookup(s->pvid_to_pvmeta, old);
		dm_hash_remove(s->pvid_to_pvmeta, old);
	}

	if (!(cft = dm_config_create()) ||
	    !(cft->root = dm_config_clone_node(cft, pvmeta, 0))) {
		unlock_pvid_to_pvmeta(s);
		return daemon_reply_simple("failed", "reason = %s", "out of memory", NULL);
	}

	pvid_dup = dm_config_find_str(cft->root, "pvmeta/id", NULL);
	if (!dm_hash_insert(s->pvid_to_pvmeta, pvid, cft) ||
	    !dm_hash_insert_binary(s->device_to_pvid, &device, sizeof(device), (void*)pvid_dup)) {
		unlock_pvid_to_pvmeta(s);
		return daemon_reply_simple("failed", "reason = %s", "out of memory", NULL);
	}
	if (pvmeta_old)
		dm_config_destroy(pvmeta_old);

	unlock_pvid_to_pvmeta(s);

	if (metadata) {
		if (!vgid)
			return daemon_reply_simple("failed", "reason = %s", "need VG UUID", NULL);
		debug("obtained vgid = %s, vgname = %s\n", vgid, vgname);
		if (!vgname)
			return daemon_reply_simple("failed", "reason = %s", "need VG name", NULL);
		if (daemon_request_int(r, "metadata/seqno", -1) < 0)
			return daemon_reply_simple("failed", "reason = %s", "need VG seqno", NULL);

		if (!update_metadata(s, vgname, vgid, metadata))
			return daemon_reply_simple("failed", "reason = %s",
						   "metadata update failed", NULL);
	} else {
		lock_pvid_to_vgid(s);
		vgid = dm_hash_lookup(s->pvid_to_vgid, pvid);
		unlock_pvid_to_vgid(s);
	}

	if (vgid) {
		if ((cft = lock_vg(s, vgid)))
			complete = update_pv_status(s, cft, cft->root, 0);
		else if (!strcmp(vgid, "#orphan"))
			orphan = 1;
		else {
			unlock_vg(s, vgid);
			return daemon_reply_simple("failed", "reason = %s",
// FIXME provide meaningful-to-user error message
						   "internal treason!", NULL);
		}
		unlock_vg(s, vgid);
	}

	return daemon_reply_simple("OK",
				   "status = %s", orphan ? "orphan" :
				                     (complete ? "complete" : "partial"),
				   "vgid = %s", vgid ? vgid : "#orphan",
				   NULL);
}

static response vg_update(lvmetad_state *s, request r)
{
	struct dm_config_node *metadata = dm_config_find_node(r.cft->root, "metadata");
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);
	const char *vgname = daemon_request_str(r, "vgname", NULL);
	if (metadata) {
		if (!vgid)
			return daemon_reply_simple("failed", "reason = %s", "need VG UUID", NULL);
		if (!vgname)
			return daemon_reply_simple("failed", "reason = %s", "need VG name", NULL);
		if (daemon_request_int(r, "metadata/seqno", -1) < 0)
			return daemon_reply_simple("failed", "reason = %s", "need VG seqno", NULL);

		/* TODO defer metadata update here; add a separate vg_commit
		 * call; if client does not commit, die */
		if (!update_metadata(s, vgname, vgid, metadata))
			return daemon_reply_simple("failed", "reason = %s",
						   "metadata update failed", NULL);
	}
	return daemon_reply_simple("OK", NULL);
}

static response vg_remove(lvmetad_state *s, request r)
{
	const char *vgid = daemon_request_str(r, "uuid", NULL);

	if (!vgid)
		return daemon_reply_simple("failed", "reason = %s", "need VG UUID", NULL);

	fprintf(stderr, "vg_remove: %s\n", vgid);

	lock_pvid_to_vgid(s);
	remove_metadata(s, vgid, 1);
	unlock_pvid_to_vgid(s);

	return daemon_reply_simple("OK", NULL);
}

static response handler(daemon_state s, client_handle h, request r)
{
	lvmetad_state *state = s.private;
	const char *rq = daemon_request_str(r, "request", "NONE");

	/*
	 * TODO Add a stats call, with transaction count/rate, time since last
	 * update &c.
	 */
	if (!strcmp(rq, "pv_found"))
		return pv_found(state, r);

	if (!strcmp(rq, "pv_gone"))
		return pv_gone(state, r);

	if (!strcmp(rq, "pv_lookup"))
		return pv_lookup(state, r);

	if (!strcmp(rq, "vg_update"))
		return vg_update(state, r);

	if (!strcmp(rq, "vg_remove"))
		return vg_remove(state, r);

	if (!strcmp(rq, "vg_lookup"))
		return vg_lookup(state, r);

	if (!strcmp(rq, "pv_list")) {
		return pv_list(state, r);
	}

	if (!strcmp(rq, "vg_list"))
		return vg_list(state, r);

	return daemon_reply_simple("failed", "reason = %s", "no such request", NULL);
}

static int init(daemon_state *s)
{
	pthread_mutexattr_t rec;
	lvmetad_state *ls = s->private;

	ls->pvid_to_pvmeta = dm_hash_create(32);
	ls->device_to_pvid = dm_hash_create(32);
	ls->vgid_to_metadata = dm_hash_create(32);
	ls->vgid_to_vgname = dm_hash_create(32);
	ls->pvid_to_vgid = dm_hash_create(32);
	ls->vgname_to_vgid = dm_hash_create(32);
	ls->lock.vg = dm_hash_create(32);
	pthread_mutexattr_init(&rec);
	pthread_mutexattr_settype(&rec, PTHREAD_MUTEX_RECURSIVE_NP);
	pthread_mutex_init(&ls->lock.pvid_to_pvmeta, &rec);
	pthread_mutex_init(&ls->lock.vgid_to_metadata, &rec);
	pthread_mutex_init(&ls->lock.pvid_to_vgid, NULL);

	debug("initialised state: vgid_to_metadata = %p\n", ls->vgid_to_metadata);
	if (!ls->pvid_to_vgid || !ls->vgid_to_metadata)
		return 0;

	/* if (ls->initial_registrations)
	   _process_initial_registrations(ds->initial_registrations); */

	return 1;
}

static int fini(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	struct dm_hash_node *n = dm_hash_get_first(ls->vgid_to_metadata);

	debug("fini\n");
	while (n) {
		dm_config_destroy(dm_hash_get_data(ls->vgid_to_metadata, n));
		n = dm_hash_get_next(ls->vgid_to_metadata, n);
	}

	n = dm_hash_get_first(ls->pvid_to_pvmeta);
	while (n) {
		dm_config_destroy(dm_hash_get_data(ls->pvid_to_pvmeta, n));
		n = dm_hash_get_next(ls->pvid_to_pvmeta, n);
	}

	n = dm_hash_get_first(ls->lock.vg);
	while (n) {
		pthread_mutex_destroy(dm_hash_get_data(ls->lock.vg, n));
		free(dm_hash_get_data(ls->lock.vg, n));
		n = dm_hash_get_next(ls->lock.vg, n);
	}

	dm_hash_destroy(ls->lock.vg);
	dm_hash_destroy(ls->pvid_to_pvmeta);
	dm_hash_destroy(ls->device_to_pvid);
	dm_hash_destroy(ls->vgid_to_metadata);
	dm_hash_destroy(ls->vgid_to_vgname);
	dm_hash_destroy(ls->vgname_to_vgid);
	dm_hash_destroy(ls->pvid_to_vgid);
	return 1;
}

static void usage(char *prog, FILE *file)
{
	fprintf(file, "Usage:\n"
		"%s [-V] [-h] [-d] [-d] [-d] [-f]\n\n"
		"   -V       Show version of lvmetad\n"
		"   -h       Show this help information\n"
		"   -d       Log debug messages to syslog (-d, -dd, -ddd)\n"
		"   -R       Replace a running lvmetad instance, loading its data\n"
		"   -f       Don't fork, run in the foreground\n\n", prog);
}

int main(int argc, char *argv[])
{
	signed char opt;
	daemon_state s = { .private = NULL };
	lvmetad_state ls;
	int _restart = 0;

	s.name = "lvmetad";
	s.private = &ls;
	s.daemon_init = init;
	s.daemon_fini = fini;
	s.handler = handler;
	s.socket_path = getenv("LVM_LVMETAD_SOCKET");
	if (!s.socket_path)
		s.socket_path = DEFAULT_RUN_DIR "/lvmetad.socket";
	s.pidfile = DEFAULT_RUN_DIR "/lvmetad.pid";
        s.log_level = 0;
	s.protocol = "lvmetad";
	s.protocol_version = 1;

	// use getopt_long
	while ((opt = getopt(argc, argv, "?fhVdRs:")) != EOF) {
		switch (opt) {
		case 'h':
			usage(argv[0], stdout);
			exit(0);
		case '?':
			usage(argv[0], stderr);
			exit(0);
		case 'R':
			_restart++;
			break;
		case 'f':
			s.foreground = 1;
			break;
		case 'd':
			s.log_level++;
			break;
		case 's': // --socket
			s.socket_path = optarg;
			break;
		case 'V':
			printf("lvmetad version 0\n");
			exit(1);
		}
	}

	daemon_start(s);
	return 0;
}
