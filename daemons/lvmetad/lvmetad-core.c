#define _GNU_SOURCE
#define _XOPEN_SOURCE 500  /* pthread */

#include <assert.h>
#include <pthread.h>
#include <malloc.h>
#include <stdint.h>
#include <unistd.h>

#include "libdevmapper.h"
#include "daemon-server.h"

typedef struct {
	struct dm_hash_table *pvs;
	struct dm_hash_table *vgs;
	struct dm_hash_table *vg_names;
	struct dm_hash_table *vgname_map;
	struct dm_hash_table *pvid_map;
	struct {
		struct dm_hash_table *vg;
		pthread_mutex_t pvs;
		pthread_mutex_t vgs;
		pthread_mutex_t pvid_map;
	} lock;
} lvmetad_state;

__attribute__ ((format(printf, 1, 2)))
static void debug(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[D %lu] ", pthread_self());
	vfprintf(stderr, fmt, ap);
	va_end(ap);
};

static void lock_pvs(lvmetad_state *s) { pthread_mutex_lock(&s->lock.pvs); }
static void unlock_pvs(lvmetad_state *s) { pthread_mutex_unlock(&s->lock.pvs); }

static void lock_vgs(lvmetad_state *s) { pthread_mutex_lock(&s->lock.vgs); }
static void unlock_vgs(lvmetad_state *s) { pthread_mutex_unlock(&s->lock.vgs); }

static void lock_pvid_map(lvmetad_state *s) { pthread_mutex_lock(&s->lock.pvid_map); }
static void unlock_pvid_map(lvmetad_state *s) { pthread_mutex_unlock(&s->lock.pvid_map); }

/*
 * TODO: It may be beneficial to clean up the vg lock hash from time to time,
 * since if we have many "rogue" requests for nonexistent things, we will keep
 * allocating memory that we never release. Not good.
 */
static struct dm_config_tree *lock_vg(lvmetad_state *s, const char *id) {
	pthread_mutex_t *vg;
	struct dm_config_tree *cft;

	lock_vgs(s);
	vg = dm_hash_lookup(s->lock.vg, id);
	if (!vg) {
		pthread_mutexattr_t rec;
		pthread_mutexattr_init(&rec);
		pthread_mutexattr_settype(&rec, PTHREAD_MUTEX_RECURSIVE_NP);
		vg = malloc(sizeof(pthread_mutex_t));
		pthread_mutex_init(vg, &rec);
		dm_hash_insert(s->lock.vg, id, vg);
	}
	debug("lock VG %s\n", id);
	pthread_mutex_lock(vg);
	cft = dm_hash_lookup(s->vgs, id);
	unlock_vgs(s);
	return cft;
}

static void unlock_vg(lvmetad_state *s, const char *id) {
	debug("unlock VG %s\n", id);
	lock_vgs(s); /* someone might be changing the s->lock.vg structure right
		      * now, so avoid stepping on each other's toes */
	pthread_mutex_unlock(dm_hash_lookup(s->lock.vg, id));
	unlock_vgs(s);
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
			node = dm_config_create_node(cft, field);
			node->sib = parent->child;
			node->v = dm_config_create_value(cft);
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

/* Either the "big" vgs lock, or a per-vg lock needs to be held before entering
 * this function. */
static int update_pv_status(lvmetad_state *s,
			    struct dm_config_tree *cft,
			    struct dm_config_node *vg, int act)
{
	struct dm_config_node *pv;
	int complete = 1;

	lock_pvs(s);
	pv = pvs(vg);
	while (pv) {
		const char *uuid = dm_config_find_str(pv->child, "id", NULL);
		int found = uuid ? (dm_hash_lookup(s->pvs, uuid) ? 1 : 0) : 0;
		if (act &&
		    !set_flag(cft, pv, "status", "MISSING", !found)) {
			complete =  0;
			break;
		}
		if (!found) {
			complete = 0;
			if (!act) { // optimisation
				unlock_pvs(s);
				return complete;
			}
		}
		pv = pv->sib;
	}
	unlock_pvs(s);

	return complete;
}

static response vg_lookup(lvmetad_state *s, request r)
{
	struct dm_config_tree *cft;
	struct dm_config_node *metadata, *n;
	response res = { .buffer = NULL };

	const char *uuid = daemon_request_str(r, "uuid", NULL),
		   *name = daemon_request_str(r, "name", NULL);
	debug("vg_lookup: uuid = %s, name = %s\n", uuid, name);

	if (!uuid || !name) {
		lock_vgs(s);
		if (name && !uuid)
			uuid = dm_hash_lookup(s->vgname_map, (void *)name);
		if (uuid && !name)
			name = dm_hash_lookup(s->vg_names, (void *)uuid);
		unlock_vgs(s);
	}

	debug("vg_lookup: updated uuid = %s, name = %s\n", uuid, name);

	if (!uuid)
		return daemon_reply_simple("failed", "reason = %s", "VG not found", NULL);

	cft = lock_vg(s, uuid);
	if (!cft || !cft->root) {
		unlock_vg(s, uuid);
		return daemon_reply_simple("failed", "reason = %s", "UUID not found", NULL);
	}

	metadata = cft->root;
	res.cft = dm_config_create();

	/* The response field */
	res.cft->root = n = dm_config_create_node(res.cft, "response");
	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = "OK";

	n = n->sib = dm_config_create_node(res.cft, "name");
	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = name;

	/* The metadata section */
	n = n->sib = dm_config_clone_node(res.cft, metadata, 1);
	n->parent = res.cft->root;
	res.error = 0;
	unlock_vg(s, uuid);

	update_pv_status(s, cft, n, 1); /* FIXME error reporting */

	return res;
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
	case DM_CFG_FLOAT: r = (a->v.f == b->v.f); break;
	case DM_CFG_INT: r = (a->v.i == b->v.i); break;
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

	if (result)
		return result;

	if (a->sib && b->sib)
		result = compare_config(a->sib, b->sib);
	if (a->sib && !b->sib)
		result = 1;
	if (!a->sib && b->sib)
		result = -1;

	return result;
}

/* You need to be holding the pvid_map lock already to call this. */
static int update_pvid_map(lvmetad_state *s, struct dm_config_tree *vg, const char *vgid)
{
	struct dm_config_node *pv = pvs(vg->root);

	if (!vgid)
		return 0;

	while (pv) {
		const char *pvid = dm_config_find_str(pv->child, "id", NULL);
		dm_hash_insert(s->pvid_map, pvid, (void *) vgid);
		pv = pv->sib;
	}

	return 1;
}

/* A pvid map lock needs to be held. */
static int remove_metadata(lvmetad_state *s, const char *vgid)
{
	struct dm_config_tree *old;
	const char *oldname;
	lock_vgs(s);
	old = dm_hash_lookup(s->vgs, vgid);
	oldname = dm_hash_lookup(s->vg_names, vgid);
	unlock_vgs(s);

	if (!old)
		return 0;

	update_pvid_map(s, old, "#orphan");
	/* need to update what we have since we found a newer version */
	dm_hash_remove(s->vgs, vgid);
	dm_hash_remove(s->vg_names, vgid);
	dm_hash_remove(s->vgname_map, oldname);
	dm_config_destroy(old);
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

	lock_vgs(s);
	old = dm_hash_lookup(s->vgs, _vgid);
	lock_vg(s, _vgid);
	unlock_vgs(s);

	seq = dm_config_find_int(metadata, "metadata/seqno", -1);

	if (old) {
		haveseq = dm_config_find_int(old->root, "metadata/seqno", -1);
		oldname = dm_hash_lookup(s->vg_names, _vgid);
		assert(oldname);
	}

	if (seq < 0)
		goto out;

	if (seq == haveseq) {
		retval = 1;
		if (compare_config(metadata, old->root))
			retval = 0;
		debug("Not updating metadata for %s at %d (equal = %d)\n", _vgid, haveseq, retval);
		goto out;
	}

	if (seq < haveseq) {
		debug("Refusing to update metadata for %s at %d to %d\n", _vgid, haveseq, seq);
		// TODO: we may want to notify the client that their metadata is
		// out of date?
		retval = 1;
		goto out;
	}

	cft = dm_config_create();
	cft->root = dm_config_clone_node(cft, metadata, 0);

	vgid = dm_config_find_str(cft->root, "metadata/id", NULL);

	if (!vgid || !name) {
		debug("Name '%s' or uuid '%s' missing!\n", name, vgid);
		goto out;
	}

	lock_pvid_map(s);

	if (haveseq >= 0 && haveseq < seq) {
		debug("Updating metadata for %s at %d to %d\n", _vgid, haveseq, seq);
		/* temporarily orphan all of our PVs */
		remove_metadata(s, vgid);
	}

	lock_vgs(s);
	dm_hash_insert(s->vgs, vgid, cft);
	debug("Mapping %s to %s\n", vgid, name);
	dm_hash_insert(s->vg_names, vgid, dm_pool_strdup(dm_config_memory(cft), name));
	dm_hash_insert(s->vgname_map, name, (void *)vgid);
	unlock_vgs(s);

	update_pvid_map(s, cft, vgid);

	unlock_pvid_map(s);
	retval = 1;
out:
	unlock_vg(s, _vgid);
	return retval;
}

static response pv_gone(lvmetad_state *s, request r)
{
	int found = 0;
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	debug("pv_gone: %s\n", pvid);

	lock_pvs(s);
	found = dm_hash_lookup(s->pvs, pvid) ? 1 : 0;
	dm_hash_remove(s->pvs, pvid);
	unlock_pvs(s);

	if (found)
		return daemon_reply_simple("OK", NULL);
	else
		return daemon_reply_simple("failed", "reason = %s", "PVID does not exist", NULL);
}

static response pv_found(lvmetad_state *s, request r)
{
	struct dm_config_node *metadata = dm_config_find_node(r.cft->root, "metadata");
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	const char *vgname = daemon_request_str(r, "vgname", NULL);
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);
	int complete = 0;

	if (!pvid)
		return daemon_reply_simple("failed", "reason = %s", "need PV UUID", NULL);

	debug("pv_found %s, vgid = %s\n", pvid, vgid);

	lock_pvs(s);
	dm_hash_insert(s->pvs, pvid, (void*)1);
	unlock_pvs(s);

	if (metadata) {
		if (!vgid)
			return daemon_reply_simple("failed", "reason = %s", "need VG UUID", NULL);
		if (!vgname)
			return daemon_reply_simple("failed", "reason = %s", "need VG name", NULL);
		if (daemon_request_int(r, "metadata/seqno", -1) < 0)
			return daemon_reply_simple("failed", "reason = %s", "need VG seqno", NULL);

		if (!update_metadata(s, vgname, vgid, metadata))
			return daemon_reply_simple("failed", "reason = %s",
						   "metadata update failed", NULL);
	} else {
		lock_pvid_map(s);
		vgid = dm_hash_lookup(s->pvid_map, pvid);
		unlock_pvid_map(s);
	}

	if (vgid) {
		struct dm_config_tree *cft = lock_vg(s, vgid);
		if (!cft) {
			unlock_vg(s, vgid);
			return daemon_reply_simple("failed", "reason = %s", "vg unknown and no PV metadata", NULL);
		}
		complete = update_pv_status(s, cft, cft->root, 0);
		unlock_vg(s, vgid);
	}

	return daemon_reply_simple("OK",
				   "status = %s", complete ? "complete" : "partial",
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

	lock_pvid_map(s);
	remove_metadata(s, vgid);
	unlock_pvid_map(s);

	return daemon_reply_simple("OK", NULL);
}

static response handler(daemon_state s, client_handle h, request r)
{
	lvmetad_state *state = s.private;
	const char *rq = daemon_request_str(r, "request", "NONE");

	debug("REQUEST: %s\n", rq);

	if (!strcmp(rq, "pv_found"))
		return pv_found(state, r);

	if (!strcmp(rq, "pv_gone"))
		pv_gone(state, r);

	if (!strcmp(rq, "vg_update"))
		return vg_update(state, r);

	if (!strcmp(rq, "vg_remove"))
		return vg_remove(state, r);

	if (!strcmp(rq, "vg_lookup"))
		return vg_lookup(state, r);

	return daemon_reply_simple("failed", "reason = %s", "no such request", NULL);
}

static int init(daemon_state *s)
{
	pthread_mutexattr_t rec;
	lvmetad_state *ls = s->private;

	ls->pvs = dm_hash_create(32);
	ls->vgs = dm_hash_create(32);
	ls->vg_names = dm_hash_create(32);
	ls->pvid_map = dm_hash_create(32);
	ls->vgname_map = dm_hash_create(32);
	ls->lock.vg = dm_hash_create(32);
	pthread_mutexattr_init(&rec);
	pthread_mutexattr_settype(&rec, PTHREAD_MUTEX_RECURSIVE_NP);
	pthread_mutex_init(&ls->lock.pvs, NULL);
	pthread_mutex_init(&ls->lock.vgs, &rec);
	pthread_mutex_init(&ls->lock.pvid_map, NULL);

	debug("initialised state: vgs = %p\n", ls->vgs);
	if (!ls->pvs || !ls->vgs)
		return 0;

	/* if (ls->initial_registrations)
	   _process_initial_registrations(ds->initial_registrations); */

	return 1;
}

static int fini(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	struct dm_hash_node *n = dm_hash_get_first(ls->vgs);

	debug("fini\n");
	while (n) {
		dm_config_destroy(dm_hash_get_data(ls->vgs, n));
		n = dm_hash_get_next(ls->vgs, n);
	}

	n = dm_hash_get_first(ls->lock.vg);
	while (n) {
		pthread_mutex_destroy(dm_hash_get_data(ls->lock.vg, n));
		free(dm_hash_get_data(ls->lock.vg, n));
		n = dm_hash_get_next(ls->lock.vg, n);
	}

	dm_hash_destroy(ls->lock.vg);
	dm_hash_destroy(ls->pvs);
	dm_hash_destroy(ls->vgs);
	dm_hash_destroy(ls->pvid_map);
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
	s.socket_path = "/var/run/lvm/lvmetad.socket";
	s.pidfile = "/var/run/lvm/lvmetad.pid";
        s.log_level = 0;

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
		case 's':
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
