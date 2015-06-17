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
#include "daemon-io.h"
#include "config-util.h"
#include "daemon-server.h"
#include "daemon-log.h"
#include "lvm-version.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#define LVMETAD_SOCKET DEFAULT_RUN_DIR "/lvmetad.socket"

typedef struct {
	log_state *log; /* convenience */
	const char *log_config;

	struct dm_hash_table *pvid_to_pvmeta;
	struct dm_hash_table *device_to_pvid; /* shares locks with above */

	struct dm_hash_table *vgid_to_metadata;
	struct dm_hash_table *vgid_to_vgname;
	struct dm_hash_table *vgid_to_outdated_pvs;
	struct dm_hash_table *vgname_to_vgid;
	struct dm_hash_table *pvid_to_vgid;
	struct {
		struct dm_hash_table *vg;
		pthread_mutex_t vg_lock_map;
		pthread_mutex_t pvid_to_pvmeta;
		pthread_mutex_t vgid_to_metadata;
		pthread_mutex_t pvid_to_vgid;
	} lock;
	char token[128];
	pthread_mutex_t token_lock;
} lvmetad_state;

static void destroy_metadata_hashes(lvmetad_state *s)
{
	struct dm_hash_node *n = NULL;

	dm_hash_iterate(n, s->vgid_to_metadata)
		dm_config_destroy(dm_hash_get_data(s->vgid_to_metadata, n));

	dm_hash_iterate(n, s->vgid_to_outdated_pvs)
		dm_config_destroy(dm_hash_get_data(s->vgid_to_outdated_pvs, n));

	dm_hash_iterate(n, s->pvid_to_pvmeta)
		dm_config_destroy(dm_hash_get_data(s->pvid_to_pvmeta, n));

	dm_hash_destroy(s->pvid_to_pvmeta);
	dm_hash_destroy(s->vgid_to_metadata);
	dm_hash_destroy(s->vgid_to_vgname);
	dm_hash_destroy(s->vgid_to_outdated_pvs);
	dm_hash_destroy(s->vgname_to_vgid);

	dm_hash_destroy(s->device_to_pvid);
	dm_hash_destroy(s->pvid_to_vgid);
}

static void create_metadata_hashes(lvmetad_state *s)
{
	s->pvid_to_pvmeta = dm_hash_create(32);
	s->device_to_pvid = dm_hash_create(32);
	s->vgid_to_metadata = dm_hash_create(32);
	s->vgid_to_vgname = dm_hash_create(32);
	s->vgid_to_outdated_pvs = dm_hash_create(32);
	s->pvid_to_vgid = dm_hash_create(32);
	s->vgname_to_vgid = dm_hash_create(32);
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

static response reply_fail(const char *reason)
{
	return daemon_reply_simple("failed", "reason = %s", reason, NULL);
}

static response reply_unknown(const char *reason)
{
	return daemon_reply_simple("unknown", "reason = %s", reason, NULL);
}

/*
 * TODO: It may be beneficial to clean up the vg lock hash from time to time,
 * since if we have many "rogue" requests for nonexistent things, we will keep
 * allocating memory that we never release. Not good.
 */
static struct dm_config_tree *lock_vg(lvmetad_state *s, const char *id) {
	pthread_mutex_t *vg;
	struct dm_config_tree *cft;
	pthread_mutexattr_t rec;

	pthread_mutex_lock(&s->lock.vg_lock_map);
	if (!(vg = dm_hash_lookup(s->lock.vg, id))) {
		if (!(vg = malloc(sizeof(pthread_mutex_t))) ||
		    pthread_mutexattr_init(&rec) ||
		    pthread_mutexattr_settype(&rec, PTHREAD_MUTEX_RECURSIVE_NP) ||
		    pthread_mutex_init(vg, &rec))
			goto bad;
		if (!dm_hash_insert(s->lock.vg, id, vg)) {
			pthread_mutex_destroy(vg);
			goto bad;
		}
	}
	/* We never remove items from s->lock.vg => the pointer remains valid. */
	pthread_mutex_unlock(&s->lock.vg_lock_map);

	DEBUGLOG(s, "locking VG %s", id);
	pthread_mutex_lock(vg);

	/* Protect against structure changes of the vgid_to_metadata hash. */
	lock_vgid_to_metadata(s);
	cft = dm_hash_lookup(s->vgid_to_metadata, id);
	unlock_vgid_to_metadata(s);
	return cft;
bad:
	pthread_mutex_unlock(&s->lock.vg_lock_map);
	free(vg);
	ERROR(s, "Out of memory");
	return NULL;
}

static void unlock_vg(lvmetad_state *s, const char *id) {
	pthread_mutex_t *vg;

	DEBUGLOG(s, "unlocking VG %s", id);
	/* Protect the s->lock.vg structure from concurrent access. */
	pthread_mutex_lock(&s->lock.vg_lock_map);
	if ((vg = dm_hash_lookup(s->lock.vg, id)))
		pthread_mutex_unlock(vg);
	pthread_mutex_unlock(&s->lock.vg_lock_map);
}

static struct dm_config_node *pvs(struct dm_config_node *vg)
{
	struct dm_config_node *pv = dm_config_find_node(vg, "metadata/physical_volumes");
	if (pv)
		pv = pv->child;
	return pv;
}

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
	response res = { 0 };

	buffer_init( &res.buffer );

	if (!(res.cft = dm_config_create()))
		return res; /* FIXME error reporting */

	/* The response field */
	res.cft->root = make_text_node(res.cft, "response", "OK", NULL, NULL);
	cn_pvs = make_config_node(res.cft, "physical_volumes", NULL, res.cft->root);

	lock_pvid_to_pvmeta(s);

	dm_hash_iterate(n, s->pvid_to_pvmeta) {
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
	response res = { 0 };
	struct dm_config_node *pv;

	buffer_init( &res.buffer );

	if (!pvid && !devt)
		return reply_fail("need PVID or device");

	if (!(res.cft = dm_config_create()))
		return reply_fail("out of memory");

	if (!(res.cft->root = make_text_node(res.cft, "response", "OK", NULL, NULL)))
		return reply_fail("out of memory");

	lock_pvid_to_pvmeta(s);
	if (!pvid && devt)
		pvid = dm_hash_lookup_binary(s->device_to_pvid, &devt, sizeof(devt));

	if (!pvid) {
		unlock_pvid_to_pvmeta(s);
		WARN(s, "pv_lookup: could not find device %" PRIu64, devt);
		dm_config_destroy(res.cft);
		return reply_unknown("device not found");
	}

	pv = make_pv_node(s, pvid, res.cft, NULL, res.cft->root);
	if (!pv) {
		unlock_pvid_to_pvmeta(s);
		dm_config_destroy(res.cft);
		return reply_unknown("PV not found");
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
	response res = { 0 };

	buffer_init( &res.buffer );

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

	dm_hash_iterate(n, s->vgid_to_vgname) {
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
	}

	unlock_vgid_to_metadata(s);
bad:
	return res;
}

static void mark_outdated_pv(lvmetad_state *s, const char *vgid, const char *pvid)
{
	struct dm_config_tree *pvmeta, *outdated_pvs;
	struct dm_config_node *list, *cft_vgid;
	struct dm_config_value *v;

	lock_pvid_to_pvmeta(s);
	pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, pvid);
	unlock_pvid_to_pvmeta(s);

	/* if the MDA exists and is used, it will have ignore=0 set */
	if (!pvmeta ||
	    (dm_config_find_int64(pvmeta->root, "pvmeta/mda0/ignore", 1) &&
	     dm_config_find_int64(pvmeta->root, "pvmeta/mda1/ignore", 1)))
		return;

	WARN(s, "PV %s has outdated metadata", pvid);

	outdated_pvs = dm_hash_lookup(s->vgid_to_outdated_pvs, vgid);
	if (!outdated_pvs) {
		if (!(outdated_pvs = dm_config_from_string("outdated_pvs/pv_list = []")) ||
		    !(cft_vgid = make_text_node(outdated_pvs, "vgid", dm_pool_strdup(outdated_pvs->mem, vgid),
						outdated_pvs->root, NULL)))
			abort();
		if(!dm_hash_insert(s->vgid_to_outdated_pvs, cft_vgid->v->v.str, outdated_pvs))
			abort();
		DEBUGLOG(s, "created outdated_pvs list for VG %s", vgid);
	}

	list = dm_config_find_node(outdated_pvs->root, "outdated_pvs/pv_list");
	v = list->v;
	while (v) {
		if (v->type != DM_CFG_EMPTY_ARRAY && !strcmp(v->v.str, pvid))
			return;
		v = v->next;
	}
	if (!(v = dm_config_create_value(outdated_pvs)))
		abort();
	v->type = DM_CFG_STRING;
	v->v.str = dm_pool_strdup(outdated_pvs->mem, pvid);
	v->next = list->v;
	list->v = v;
}

static void chain_outdated_pvs(lvmetad_state *s, const char *vgid, struct dm_config_tree *metadata_cft, struct dm_config_node *metadata)
{
	struct dm_config_tree *cft = dm_hash_lookup(s->vgid_to_outdated_pvs, vgid), *pvmeta;
	struct dm_config_node *pv, *res, *out_pvs = cft ? dm_config_find_node(cft->root, "outdated_pvs/pv_list") : NULL;
	struct dm_config_value *pvs_v = out_pvs ? out_pvs->v : NULL;
	if (!pvs_v)
		return;
	if (!(res = make_config_node(metadata_cft, "outdated_pvs", metadata_cft->root, 0)))
		return; /* oops */
	res->sib = metadata->child;
	metadata->child = res;
	for (; pvs_v && pvs_v->type != DM_CFG_EMPTY_ARRAY; pvs_v = pvs_v->next) {
		pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, pvs_v->v.str);
		if (!pvmeta) {
			WARN(s, "metadata for PV %s not found", pvs_v->v.str);
			continue;
		}
		if (!(pv = dm_config_clone_node(metadata_cft, pvmeta->root, 0)))
			continue;
		pv->key = dm_config_find_str(pv, "pvmeta/id", NULL);
		pv->sib = res->child;
		res->child = pv;
	}
}

static response vg_lookup(lvmetad_state *s, request r)
{
	struct dm_config_tree *cft;
	struct dm_config_node *metadata, *n;
	response res = { 0 };

	const char *uuid = daemon_request_str(r, "uuid", NULL);
	const char *name = daemon_request_str(r, "name", NULL);

	buffer_init( &res.buffer );

	DEBUGLOG(s, "vg_lookup: uuid = %s, name = %s", uuid, name);

	if (!uuid || !name) {
		lock_vgid_to_metadata(s);
		if (name && !uuid)
			uuid = dm_hash_lookup(s->vgname_to_vgid, name);
		if (uuid && !name)
			name = dm_hash_lookup(s->vgid_to_vgname, uuid);
		unlock_vgid_to_metadata(s);
	}

	DEBUGLOG(s, "vg_lookup: updated uuid = %s, name = %s", uuid, name);

	/* Check the name here. */
	if (!uuid || !name)
		return reply_unknown("VG not found");

	cft = lock_vg(s, uuid);
	if (!cft || !cft->root) {
		unlock_vg(s, uuid);
		return reply_unknown("UUID not found");
	}

	metadata = cft->root;
	if (!(res.cft = dm_config_create()))
		goto bad;

	/* The response field */
	if (!(res.cft->root = n = dm_config_create_node(res.cft, "response")))
		goto bad;

	if (!(n->v = dm_config_create_value(cft)))
		goto bad;

	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = "OK";

	if (!(n = n->sib = dm_config_create_node(res.cft, "name")))
		goto bad;

	if (!(n->v = dm_config_create_value(res.cft)))
		goto bad;

	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = name;

	/* The metadata section */
	if (!(n = n->sib = dm_config_clone_node(res.cft, metadata, 1)))
		goto bad;
	n->parent = res.cft->root;
	unlock_vg(s, uuid);

	update_pv_status(s, res.cft, n, 1); /* FIXME report errors */
	chain_outdated_pvs(s, uuid, res.cft, n);

	return res;
bad:
	unlock_vg(s, uuid);
	return reply_fail("out of memory");
}

static int vg_remove_if_missing(lvmetad_state *s, const char *vgid, int update_pvids);

enum update_pvid_mode { UPDATE_ONLY, REMOVE_EMPTY, MARK_OUTDATED };

/* You need to be holding the pvid_to_vgid lock already to call this. */
static int update_pvid_to_vgid(lvmetad_state *s, struct dm_config_tree *vg,
			       const char *vgid, int mode)
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

		if (mode == REMOVE_EMPTY &&
		    (vgid_old = dm_hash_lookup(s->pvid_to_vgid, pvid)) &&
		    !dm_hash_insert(to_check, vgid_old, (void*) 1))
			goto out;

		if (mode == MARK_OUTDATED)
			mark_outdated_pv(s, vgid, pvid);

		if (!dm_hash_insert(s->pvid_to_vgid, pvid, (void*) vgid))
			goto out;

		DEBUGLOG(s, "moving PV %s to VG %s", pvid, vgid);
	}

	dm_hash_iterate(n, to_check) {
		check_vgid = dm_hash_get_key(to_check, n);
		lock_vg(s, check_vgid);
		vg_remove_if_missing(s, check_vgid, 0);
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
	struct dm_config_tree *old, *outdated_pvs;
	const char *oldname;
	lock_vgid_to_metadata(s);
	old = dm_hash_lookup(s->vgid_to_metadata, vgid);
	outdated_pvs = dm_hash_lookup(s->vgid_to_outdated_pvs, vgid);
	oldname = dm_hash_lookup(s->vgid_to_vgname, vgid);

	if (!old) {
		unlock_vgid_to_metadata(s);
		return 0;
	}

	assert(oldname);

	/* need to update what we have since we found a newer version */
	dm_hash_remove(s->vgid_to_metadata, vgid);
	dm_hash_remove(s->vgid_to_vgname, vgid);
	dm_hash_remove(s->vgname_to_vgid, oldname);
	dm_hash_remove(s->vgid_to_outdated_pvs, vgid);
	unlock_vgid_to_metadata(s);

	if (update_pvids)
		/* FIXME: What should happen when update fails */
		update_pvid_to_vgid(s, old, "#orphan", 0);
	dm_config_destroy(old);
	if (outdated_pvs)
		dm_config_destroy(outdated_pvs);
	return 1;
}

/* The VG must be locked. */
static int vg_remove_if_missing(lvmetad_state *s, const char *vgid, int update_pvids)
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
		DEBUGLOG(s, "removing empty VG %s", vgid);
		remove_metadata(s, vgid, update_pvids);
	}

	unlock_pvid_to_pvmeta(s);

	return 1;
}

/* No locks need to be held. The pointers are never used outside of the scope of
 * this function, so they can be safely destroyed after update_metadata returns
 * (anything that might have been retained is copied). */
static int update_metadata(lvmetad_state *s, const char *name, const char *_vgid,
			   struct dm_config_node *metadata, int64_t *oldseq, const char *pvid)
{
	struct dm_config_tree *cft = NULL;
	struct dm_config_tree *old;
	int retval = 0;
	int seq;
	int haveseq = -1;
	const char *oldname = NULL;
	const char *vgid;
	char *cfgname;

	lock_vgid_to_metadata(s);
	old = dm_hash_lookup(s->vgid_to_metadata, _vgid);
	oldname = dm_hash_lookup(s->vgid_to_vgname, _vgid);
	unlock_vgid_to_metadata(s);
	lock_vg(s, _vgid);

	seq = dm_config_find_int(metadata, "metadata/seqno", -1);

	if (old)
		haveseq = dm_config_find_int(old->root, "metadata/seqno", -1);

	if (seq < 0)
		goto out;

	filter_metadata(metadata); /* sanitize */

	if (oldseq) {
		if (old)
			*oldseq = haveseq;
		else
			*oldseq = seq;
	}

	if (seq == haveseq) {
		retval = 1;
		if (compare_config(metadata, old->root))
			retval = 0;
		DEBUGLOG(s, "Not updating metadata for %s at %d (%s)", _vgid, haveseq,
		      retval ? "ok" : "MISMATCH");
		if (!retval) {
			DEBUGLOG_cft(s, "OLD: ", old->root);
			DEBUGLOG_cft(s, "NEW: ", metadata);
		}
		goto out;
	}

	if (seq < haveseq) {
		DEBUGLOG(s, "Refusing to update metadata for %s (at %d) to %d", _vgid, haveseq, seq);

		if (pvid)
			mark_outdated_pv(s, dm_config_find_str(old->root, "metadata/id", NULL), pvid);

		/* TODO: notify the client that their metadata is out of date? */
		retval = 1;
		goto out;
	}

	if (!(cft = dm_config_create()) ||
	    !(cft->root = dm_config_clone_node(cft, metadata, 0))) {
		ERROR(s, "Out of memory");
		goto out;
	}

	vgid = dm_config_find_str(cft->root, "metadata/id", NULL);

	if (!vgid || !name) {
		DEBUGLOG(s, "Name '%s' or uuid '%s' missing!", name, vgid);
		goto out;
	}

	lock_pvid_to_vgid(s);

	if (haveseq >= 0 && haveseq < seq) {
		INFO(s, "Updating metadata for %s at %d to %d", _vgid, haveseq, seq);
		if (oldseq)
			update_pvid_to_vgid(s, old, vgid, MARK_OUTDATED);
		/* temporarily orphan all of our PVs */
		update_pvid_to_vgid(s, old, "#orphan", 0);
	}

	lock_vgid_to_metadata(s);
	DEBUGLOG(s, "Mapping %s to %s", vgid, name);

	retval = ((cfgname = dm_pool_strdup(dm_config_memory(cft), name)) &&
		  dm_hash_insert(s->vgid_to_metadata, vgid, cft) &&
		  dm_hash_insert(s->vgid_to_vgname, vgid, cfgname) &&
		  dm_hash_insert(s->vgname_to_vgid, name, (void*) vgid)) ? 1 : 0;

	if (retval && oldname && strcmp(name, oldname)) {
		const char *vgid_prev = dm_hash_lookup(s->vgname_to_vgid, oldname);
		if (vgid_prev && !strcmp(vgid_prev, vgid))
			dm_hash_remove(s->vgname_to_vgid, oldname);
	}

	if (haveseq >= 0 && haveseq < seq)
		dm_config_destroy(old);

	unlock_vgid_to_metadata(s);

	if (retval)
		retval = update_pvid_to_vgid(s, cft, vgid, 1);

	unlock_pvid_to_vgid(s);
out: /* FIXME: We should probably abort() on partial failures. */
	if (!retval && cft)
		dm_config_destroy(cft);
	unlock_vg(s, _vgid);
	return retval;
}

static dev_t device_remove(lvmetad_state *s, struct dm_config_tree *pvmeta, dev_t device)
{
	struct dm_config_node *pvmeta_tmp;
	struct dm_config_value *v = NULL;
	dev_t alt_device = 0, prim_device = 0;

	if ((pvmeta_tmp = dm_config_find_node(pvmeta->root, "pvmeta/devices_alternate")))
		v = pvmeta_tmp->v;

	prim_device = dm_config_find_int64(pvmeta->root, "pvmeta/device", 0);

	/* it is the primary device */
	if (device > 0 && device == prim_device && pvmeta_tmp && pvmeta_tmp->v)
	{
		alt_device = pvmeta_tmp->v->v.i;
		pvmeta_tmp->v = pvmeta_tmp->v->next;
		pvmeta_tmp = dm_config_find_node(pvmeta->root, "pvmeta/device");
		pvmeta_tmp->v->v.i = alt_device;
	} else if (device != prim_device)
		alt_device = prim_device;

	/* it is an alternate device */
	if (device > 0 && v && v->v.i == device)
		pvmeta_tmp->v = v->next;
	else while (device > 0 && pvmeta_tmp && v) {
		if (v->next && v->next->v.i == device)
			v->next = v->next->next;
		v = v->next;
	}

	return alt_device;
}

static response pv_gone(lvmetad_state *s, request r)
{
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	int64_t device = daemon_request_int(r, "device", 0);
	int64_t alt_device = 0;
	struct dm_config_tree *pvmeta;
	char *vgid;

	DEBUGLOG(s, "pv_gone: %s / %" PRIu64, pvid, device);

	lock_pvid_to_pvmeta(s);
	if (!pvid && device > 0)
		pvid = dm_hash_lookup_binary(s->device_to_pvid, &device, sizeof(device));
	if (!pvid) {
		unlock_pvid_to_pvmeta(s);
		return reply_unknown("device not in cache");
	}

	DEBUGLOG(s, "pv_gone (updated): %s / %" PRIu64, pvid, device);

	pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, pvid);
	vgid = dm_hash_lookup(s->pvid_to_vgid, pvid);

	dm_hash_remove_binary(s->device_to_pvid, &device, sizeof(device));

	if (!(alt_device = device_remove(s, pvmeta, device)))
		dm_hash_remove(s->pvid_to_pvmeta, pvid);

	DEBUGLOG(s, "pv_gone alt_device = %" PRIu64, alt_device);

	unlock_pvid_to_pvmeta(s);

	if (vgid) {
		if (!(vgid = dm_strdup(vgid)))
			return reply_fail("out of memory");

		lock_vg(s, vgid);
		vg_remove_if_missing(s, vgid, 1);
		unlock_vg(s, vgid);
		dm_free(vgid);
	}

	if (!pvmeta)
		return reply_unknown("PVID does not exist");

	if (!alt_device)
		dm_config_destroy(pvmeta);

	if (alt_device) {
		return daemon_reply_simple("OK",
					   "device = %"PRId64, alt_device,
					   NULL);
	} else
		return daemon_reply_simple("OK", NULL );
}

static response pv_clear_all(lvmetad_state *s, request r)
{
	DEBUGLOG(s, "pv_clear_all");

	lock_pvid_to_pvmeta(s);
	lock_pvid_to_vgid(s);
	lock_vgid_to_metadata(s);

	destroy_metadata_hashes(s);
	create_metadata_hashes(s);

	unlock_pvid_to_vgid(s);
	unlock_vgid_to_metadata(s);
	unlock_pvid_to_pvmeta(s);

	return daemon_reply_simple("OK", NULL);
}

static response pv_found(lvmetad_state *s, request r)
{
	struct dm_config_node *metadata = dm_config_find_node(r.cft->root, "metadata");
	const char *pvid = daemon_request_str(r, "pvmeta/id", NULL);
	const char *vgname = daemon_request_str(r, "vgname", NULL);
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);
	const char *vgid_old = NULL;
	struct dm_config_node *pvmeta = dm_config_find_node(r.cft->root, "pvmeta"), *altdev = NULL;
	struct dm_config_value *altdev_v;
	uint64_t device, device_old_pvid = 0;
	struct dm_config_tree *cft, *pvmeta_old_dev = NULL, *pvmeta_old_pvid = NULL;
	char *old;
	int complete = 0, orphan = 0;
	int64_t seqno = -1, seqno_old = -1, changed = 0;

	if (!pvid)
		return reply_fail("need PV UUID");
	if (!pvmeta)
		return reply_fail("need PV metadata");

	if (!dm_config_get_uint64(pvmeta, "pvmeta/device", &device))
		return reply_fail("need PV device number");

	if (!(cft = dm_config_create()))
		return reply_fail("out of memory");

	lock_pvid_to_pvmeta(s);

	if ((pvmeta_old_pvid = dm_hash_lookup(s->pvid_to_pvmeta, pvid)))
		dm_config_get_uint64(pvmeta_old_pvid->root, "pvmeta/device", &device_old_pvid);

	if ((old = dm_hash_lookup_binary(s->device_to_pvid, &device, sizeof(device)))) {
		pvmeta_old_dev = dm_hash_lookup(s->pvid_to_pvmeta, old);
		vgid_old = dm_hash_lookup(s->pvid_to_vgid, old);
	}

	DEBUGLOG(s, "pv_found %s, vgid = %s, device = %" PRIu64 " (previously %" PRIu64 "), old = %s",
		 pvid, vgid, device, device_old_pvid, old);

	if (!(cft->root = dm_config_clone_node(cft, pvmeta, 0)))
                goto out_of_mem;

	pvid = dm_config_find_str(cft->root, "pvmeta/id", NULL);

	if (!pvmeta_old_pvid || compare_config(pvmeta_old_pvid->root, cft->root))
		changed |= 1;

	if (pvmeta_old_pvid && device != device_old_pvid) {
		DEBUGLOG(s, "PV %s duplicated on device %" PRIu64, pvid, device_old_pvid);
		dm_hash_remove_binary(s->device_to_pvid, &device_old_pvid, sizeof(device_old_pvid));
		if (!dm_hash_insert_binary(s->device_to_pvid, &device_old_pvid,
					   sizeof(device_old_pvid), (void*)pvid))
			goto out_of_mem;
		if ((altdev = dm_config_find_node(pvmeta_old_pvid->root, "pvmeta/devices_alternate"))) {
			altdev = dm_config_clone_node(cft, altdev, 0);
			chain_node(altdev, cft->root, 0);
		} else
			if (!(altdev = make_config_node(cft, "devices_alternate", cft->root, 0)))
				goto out_of_mem;
                altdev_v = altdev->v;
                while (1) {
			if (altdev_v && altdev_v->v.i == device_old_pvid)
				break;
			if (altdev_v)
				altdev_v = altdev_v->next;
			if (!altdev_v) {
				if (!(altdev_v = dm_config_create_value(cft)))
					goto out_of_mem;
				altdev_v->next = altdev->v;
				altdev->v = altdev_v;
				altdev->v->v.i = device_old_pvid;
				break;
			}
		};
		altdev_v = altdev->v;
		while (altdev_v) {
			if (altdev_v->next && altdev_v->next->v.i == device)
				altdev_v->next = altdev_v->next->next;
			altdev_v = altdev_v->next;
		}
		changed |= 1;
	}

	if (!dm_hash_insert(s->pvid_to_pvmeta, pvid, cft) ||
	    !dm_hash_insert_binary(s->device_to_pvid, &device, sizeof(device), (void*)pvid)) {
		dm_hash_remove(s->pvid_to_pvmeta, pvid);
out_of_mem:
		unlock_pvid_to_pvmeta(s);
		dm_config_destroy(cft);
		dm_free(old);
		return reply_fail("out of memory");
	}

	unlock_pvid_to_pvmeta(s);

	if (pvmeta_old_pvid)
		dm_config_destroy(pvmeta_old_pvid);
	if (pvmeta_old_dev && pvmeta_old_dev != pvmeta_old_pvid) {
		dev_t d = dm_config_find_int64(pvmeta_old_dev->root, "pvmeta/device", 0);
		WARN(s, "pv_found: stray device %"PRId64, d);
		if (!device_remove(s, pvmeta_old_dev, device)) {
			dm_hash_remove(s->pvid_to_pvmeta, old);
			dm_config_destroy(pvmeta_old_dev);
		}
	}

	if (metadata) {
		if (!vgid)
			return reply_fail("need VG UUID");
		DEBUGLOG(s, "obtained vgid = %s, vgname = %s", vgid, vgname);
		if (!vgname)
			return reply_fail("need VG name");
		if (daemon_request_int(r, "metadata/seqno", -1) < 0)
			return reply_fail("need VG seqno");

		if (!update_metadata(s, vgname, vgid, metadata, &seqno_old, pvid))
			return reply_fail("metadata update failed");
		changed |= (seqno_old != dm_config_find_int(metadata, "metadata/seqno", -1));
	} else {
		lock_pvid_to_vgid(s);
		vgid = dm_hash_lookup(s->pvid_to_vgid, pvid);
		unlock_pvid_to_vgid(s);
	}

	if (vgid) {
		if ((cft = lock_vg(s, vgid))) {
			complete = update_pv_status(s, cft, cft->root, 0);
			seqno = dm_config_find_int(cft->root, "metadata/seqno", -1);
		} else if (!strcmp(vgid, "#orphan"))
			orphan = 1;
		else {
			unlock_vg(s, vgid);
			return reply_fail("non-orphan VG without metadata encountered");
		}
		unlock_vg(s, vgid);

		// TODO: separate vgid->vgname lock
		lock_vgid_to_metadata(s);
		vgname = dm_hash_lookup(s->vgid_to_vgname, vgid);
		unlock_vgid_to_metadata(s);
	}

	if (vgid_old && (!vgid || strcmp(vgid, vgid_old))) {
		/* make a copy, because vg_remove_if_missing will deallocate the
		 * storage behind vgid_old */
		vgid_old = dm_strdup(vgid_old);
		lock_vg(s, vgid_old);
		vg_remove_if_missing(s, vgid_old, 1);
		unlock_vg(s, vgid_old);
		dm_free((char*)vgid_old);
	}

	return daemon_reply_simple("OK",
				   "status = %s", orphan ? "orphan" :
				                     (complete ? "complete" : "partial"),
				   "changed = %d", changed,
				   "vgid = %s", vgid ? vgid : "#orphan",
				   "vgname = %s", vgname ? vgname : "#orphan",
				   "seqno_before = %"PRId64, seqno_old,
				   "seqno_after = %"PRId64, seqno,
				   NULL);
}

static response vg_clear_outdated_pvs(lvmetad_state *s, request r)
{
	struct dm_config_tree *outdated_pvs;
	const char *vgid = daemon_request_str(r, "vgid", NULL);

	if (!vgid)
		return reply_fail("need VG UUID");

	if ((outdated_pvs = dm_hash_lookup(s->vgid_to_outdated_pvs, vgid))) {
		dm_config_destroy(outdated_pvs);
		dm_hash_remove(s->vgid_to_outdated_pvs, vgid);
	}
	return daemon_reply_simple("OK", NULL);
}

static response vg_update(lvmetad_state *s, request r)
{
	struct dm_config_node *metadata = dm_config_find_node(r.cft->root, "metadata");
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);
	const char *vgname = daemon_request_str(r, "vgname", NULL);
	if (metadata) {
		if (!vgid)
			return reply_fail("need VG UUID");
		if (!vgname)
			return reply_fail("need VG name");
		if (daemon_request_int(r, "metadata/seqno", -1) < 0)
			return reply_fail("need VG seqno");

		/* TODO defer metadata update here; add a separate vg_commit
		 * call; if client does not commit, die */
		if (!update_metadata(s, vgname, vgid, metadata, NULL, NULL))
			return reply_fail("metadata update failed");
	}
	return daemon_reply_simple("OK", NULL);
}

static response vg_remove(lvmetad_state *s, request r)
{
	const char *vgid = daemon_request_str(r, "uuid", NULL);

	if (!vgid)
		return reply_fail("need VG UUID");

	DEBUGLOG(s, "vg_remove: %s", vgid);

	lock_pvid_to_vgid(s);
	remove_metadata(s, vgid, 1);
	unlock_pvid_to_vgid(s);

	return daemon_reply_simple("OK", NULL);
}

static void _dump_cft(struct buffer *buf, struct dm_hash_table *ht, const char *key_addr)
{
	struct dm_hash_node *n;

	dm_hash_iterate(n, ht) {
		struct dm_config_tree *cft = dm_hash_get_data(ht, n);
		const char *key_backup = cft->root->key;
		cft->root->key = dm_config_find_str(cft->root, key_addr, "unknown");
		(void) dm_config_write_node(cft->root, buffer_line, buf);
		cft->root->key = key_backup;
	}
}

static void _dump_pairs(struct buffer *buf, struct dm_hash_table *ht, const char *name, int int_key)
{
	char *append;
	struct dm_hash_node *n;

	buffer_append(buf, name);
	buffer_append(buf, " {\n");

	dm_hash_iterate(n, ht) {
		const char *key = dm_hash_get_key(ht, n),
			   *val = dm_hash_get_data(ht, n);
		buffer_append(buf, "    ");
		if (int_key)
			(void) dm_asprintf(&append, "%d = \"%s\"", *(const int*)key, val);
		else
			(void) dm_asprintf(&append, "%s = \"%s\"", key, val);
		if (append)
			buffer_append(buf, append);
		buffer_append(buf, "\n");
		dm_free(append);
	}
	buffer_append(buf, "}\n");
}

static response dump(lvmetad_state *s)
{
	response res = { 0 };
	struct buffer *b = &res.buffer;

	buffer_init(b);

	/* Lock everything so that we get a consistent dump. */

	lock_vgid_to_metadata(s);
	lock_pvid_to_pvmeta(s);
	lock_pvid_to_vgid(s);

	buffer_append(b, "# VG METADATA\n\n");
	_dump_cft(b, s->vgid_to_metadata, "metadata/id");

	buffer_append(b, "\n# PV METADATA\n\n");
	_dump_cft(b, s->pvid_to_pvmeta, "pvmeta/id");

	buffer_append(b, "\n# VGID to VGNAME mapping\n\n");
	_dump_pairs(b, s->vgid_to_vgname, "vgid_to_vgname", 0);

	buffer_append(b, "\n# VGID to outdated PVs mapping\n\n");
	_dump_cft(b, s->vgid_to_outdated_pvs, "outdated_pvs/vgid");

	buffer_append(b, "\n# VGNAME to VGID mapping\n\n");
	_dump_pairs(b, s->vgname_to_vgid, "vgname_to_vgid", 0);

	buffer_append(b, "\n# PVID to VGID mapping\n\n");
	_dump_pairs(b, s->pvid_to_vgid, "pvid_to_vgid", 0);

	buffer_append(b, "\n# DEVICE to PVID mapping\n\n");
	_dump_pairs(b, s->device_to_pvid, "device_to_pvid", 1);

	unlock_pvid_to_vgid(s);
	unlock_pvid_to_pvmeta(s);
	unlock_vgid_to_metadata(s);

	return res;
}

static response handler(daemon_state s, client_handle h, request r)
{
	lvmetad_state *state = s.private;
	const char *rq = daemon_request_str(r, "request", "NONE");
	const char *token = daemon_request_str(r, "token", "NONE");

	pthread_mutex_lock(&state->token_lock);
	if (!strcmp(rq, "token_update")) {
		strncpy(state->token, token, 128);
		state->token[127] = 0;
		pthread_mutex_unlock(&state->token_lock);
		return daemon_reply_simple("OK", NULL);
	}

	if (strcmp(token, state->token) && strcmp(rq, "dump")) {
		pthread_mutex_unlock(&state->token_lock);
		return daemon_reply_simple("token_mismatch",
					   "expected = %s", state->token,
					   "received = %s", token,
					   "reason = %s",
					   "lvmetad cache is invalid due to a global_filter change or due to a running rescan", NULL);
	}
	pthread_mutex_unlock(&state->token_lock);

	/*
	 * TODO Add a stats call, with transaction count/rate, time since last
	 * update &c.
	 */
	if (!strcmp(rq, "pv_found"))
		return pv_found(state, r);

	if (!strcmp(rq, "pv_gone"))
		return pv_gone(state, r);

	if (!strcmp(rq, "pv_clear_all"))
		return pv_clear_all(state, r);

	if (!strcmp(rq, "pv_lookup"))
		return pv_lookup(state, r);

	if (!strcmp(rq, "vg_update"))
		return vg_update(state, r);

	if (!strcmp(rq, "vg_clear_outdated_pvs"))
		return vg_clear_outdated_pvs(state, r);

	if (!strcmp(rq, "vg_remove"))
		return vg_remove(state, r);

	if (!strcmp(rq, "vg_lookup"))
		return vg_lookup(state, r);

	if (!strcmp(rq, "pv_list"))
		return pv_list(state, r);

	if (!strcmp(rq, "vg_list"))
		return vg_list(state, r);

	if (!strcmp(rq, "dump"))
		return dump(state);

	return reply_fail("request not implemented");
}

static int init(daemon_state *s)
{
	pthread_mutexattr_t rec;
	lvmetad_state *ls = s->private;
	ls->log = s->log;

	pthread_mutexattr_init(&rec);
	pthread_mutexattr_settype(&rec, PTHREAD_MUTEX_RECURSIVE_NP);
	pthread_mutex_init(&ls->lock.pvid_to_pvmeta, &rec);
	pthread_mutex_init(&ls->lock.vgid_to_metadata, &rec);
	pthread_mutex_init(&ls->lock.pvid_to_vgid, NULL);
	pthread_mutex_init(&ls->lock.vg_lock_map, NULL);
	pthread_mutex_init(&ls->token_lock, NULL);
	create_metadata_hashes(ls);

	ls->lock.vg = dm_hash_create(32);
	ls->token[0] = 0;

	/* Set up stderr logging depending on the -l option. */
	if (!daemon_log_parse(ls->log, DAEMON_LOG_OUTLET_STDERR, ls->log_config, 1))
		return 0;

	DEBUGLOG(s, "initialised state: vgid_to_metadata = %p", ls->vgid_to_metadata);
	if (!ls->pvid_to_vgid || !ls->vgid_to_metadata)
		return 0;

	/* if (ls->initial_registrations)
	   _process_initial_registrations(ds->initial_registrations); */

	return 1;
}

static int fini(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	struct dm_hash_node *n;

	DEBUGLOG(s, "fini");

	destroy_metadata_hashes(ls);

	/* Destroy the lock hashes now. */
	dm_hash_iterate(n, ls->lock.vg) {
		pthread_mutex_destroy(dm_hash_get_data(ls->lock.vg, n));
		free(dm_hash_get_data(ls->lock.vg, n));
	}

	dm_hash_destroy(ls->lock.vg);
	return 1;
}

static void usage(const char *prog, FILE *file)
{
	fprintf(file, "Usage:\n"
		"%s [-V] [-h] [-f] [-l {all|wire|debug}] [-s path]\n\n"
		"   -V       Show version of lvmetad\n"
		"   -h       Show this help information\n"
		"   -f       Don't fork, run in the foreground\n"
		"   -l       Logging message level (-l {all|wire|debug})\n"
		"   -p       Set path to the pidfile\n"
		"   -s       Set path to the socket to listen on\n\n", prog);
}

int main(int argc, char *argv[])
{
	signed char opt;
	lvmetad_state ls = { .log_config = "" };
	daemon_state s = {
		.daemon_fini = fini,
		.daemon_init = init,
		.handler = handler,
		.name = "lvmetad",
		.pidfile = getenv("LVM_LVMETAD_PIDFILE") ? : LVMETAD_PIDFILE,
		.private = &ls,
		.protocol = "lvmetad",
		.protocol_version = 1,
		.socket_path = getenv("LVM_LVMETAD_SOCKET") ? : LVMETAD_SOCKET,
	};

	// use getopt_long
	while ((opt = getopt(argc, argv, "?fhVl:p:s:")) != EOF) {
		switch (opt) {
		case 'h':
			usage(argv[0], stdout);
			exit(0);
		case '?':
			usage(argv[0], stderr);
			exit(0);
		case 'f':
			s.foreground = 1;
			break;
		case 'l':
			ls.log_config = optarg;
			break;
		case 'p':
			s.pidfile = optarg;
			break;
		case 's': // --socket
			s.socket_path = optarg;
			break;
		case 'V':
			printf("lvmetad version: " LVM_VERSION "\n");
			exit(1);
		}
	}

	daemon_start(s);

	return 0;
}
