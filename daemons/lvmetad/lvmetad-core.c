#include <assert.h>
#include <pthread.h>

#include "libdevmapper.h"
#include <malloc.h>
#include <stdint.h>

#include "../common/daemon-server.h"

typedef struct {
	struct dm_hash_table *pvs;
	struct dm_hash_table *vgs;
	struct dm_hash_table *pvid_map;
	struct {
		struct dm_hash_table *vg;
		pthread_mutex_t pvs;
		pthread_mutex_t vgs;
		pthread_mutex_t pvid_map;
	} lock;
} lvmetad_state;

void debug(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[D %u] ", pthread_self());
	vfprintf(stderr, fmt, ap);
	va_end(ap);
};

void lock_pvs(lvmetad_state *s) { pthread_mutex_lock(&s->lock.pvs); }
void unlock_pvs(lvmetad_state *s) { pthread_mutex_unlock(&s->lock.pvs); }

void lock_vgs(lvmetad_state *s) { pthread_mutex_lock(&s->lock.vgs); }
void unlock_vgs(lvmetad_state *s) { pthread_mutex_unlock(&s->lock.vgs); }

void lock_pvid_map(lvmetad_state *s) { pthread_mutex_lock(&s->lock.pvid_map); }
void unlock_pvid_map(lvmetad_state *s) { pthread_mutex_unlock(&s->lock.pvid_map); }

/*
 * TODO: It may be beneficial to clean up the vg lock hash from time to time,
 * since if we have many "rogue" requests for nonexistent things, we will keep
 * allocating memory that we never release. Not good.
 */
struct config_tree *lock_vg(lvmetad_state *s, const char *id) {
	lock_vgs(s);
	pthread_mutex_t *vg = dm_hash_lookup(s->lock.vg, id);
	if (!vg) {
		pthread_mutexattr_t rec;
		pthread_mutexattr_init(&rec);
		pthread_mutexattr_settype(&rec, PTHREAD_MUTEX_RECURSIVE_NP);
		vg = malloc(sizeof(pthread_mutex_t));
		pthread_mutex_init(vg, &rec);
		dm_hash_insert(s->lock.vg, id, vg);
	}
	pthread_mutex_lock(vg);
	struct config_tree *cft = dm_hash_lookup(s->vgs, id);
	unlock_vgs(s);
	return cft;
}

void unlock_vg(lvmetad_state *s, const char *id) {
	lock_vgs(s); /* someone might be changing the s->lock.vg structure right
		      * now, so avoid stepping on each other's toes */
	pthread_mutex_unlock(dm_hash_lookup(s->lock.vg, id));
	unlock_vgs(s);
}

static struct config_node *pvs(struct config_node *vg)
{
	struct config_node *pv = find_config_node(vg, "metadata/physical_volumes");
	if (pv)
		pv = pv->child;
	return pv;
}

/*
 * TODO: This set_flag function is pretty generic and might make sense in a
 * library here or there.
 */
static void set_flag(struct config_tree *cft, struct config_node *parent,
		     char *field, const char *flag, int want) {
	struct config_value *value = NULL, *pred = NULL;
	struct config_node *node = find_config_node(parent->child, field);
	int found = 0;

	if (node)
		value = node->v;

	while (value && value->type != CFG_EMPTY_ARRAY && strcmp(value->v.str, flag)) {
		pred = value;
		value = value->next;
	}

	if (value && want)
		return;

	if (!value && !want)
		return;

	if (value && !want) {
		if (pred)
			pred->next = value->next;
		if (value == node->v)
			node->v = value->next;
	}

	if (!value && want) {
		if (!node) {
			node = create_config_node(cft, field);
			node->sib = parent->child;
			node->v = create_config_value(cft);
			node->v->type = CFG_EMPTY_ARRAY;
			node->parent = parent;
			parent->child = node;
		}
		struct config_value *new = create_config_value(cft);
		new->type = CFG_STRING;
		new->v.str = flag;
		new->next = node->v;
		node->v = new;
	}
}

/* Either the "big" vgs lock, or a per-vg lock needs to be held before entering
 * this function. */
static int update_pv_status(lvmetad_state *s, struct config_tree *cft, struct config_node *vg, int act)
{
	int complete = 1;

	lock_pvs(s);
	struct config_node *pv = pvs(vg);
	while (pv) {
		const char *uuid = find_config_str(pv->child, "id", NULL);
		int found = uuid ? (dm_hash_lookup(s->pvs, uuid) ? 1 : 0) : 0;
		if (act)
			set_flag(cft, pv, "status", "MISSING", !found);
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

static response vg_by_uuid(lvmetad_state *s, request r)
{
	const char *uuid = daemon_request_str(r, "uuid", "NONE");
	debug("vg_by_uuid: %s (vgs = %p)\n", uuid, s->vgs);
	struct config_tree *cft = lock_vg(s, uuid);
	if (!cft || !cft->root) {
		unlock_vg(s, uuid);
		return daemon_reply_simple("failed", "reason = %s", "uuid not found", NULL);
	}

	struct config_node *metadata = cft->root;

	response res = { .buffer = NULL };
	struct config_node *n;
	res.cft = create_config_tree(NULL, 0);

	/* The response field */
	res.cft->root = n = create_config_node(res.cft, "response");
	n->v->type = CFG_STRING;
	n->v->v.str = "OK";

	/* The metadata section */
	n = n->sib = clone_config_node(res.cft, metadata, 1);
	n->parent = res.cft->root;
	res.error = 0;
	unlock_vg(s, uuid);

	update_pv_status(s, cft, n, 1);

	return res;
}

static int compare_value(struct config_value *a, struct config_value *b)
{
	if (a->type > b->type)
		return 1;
	if (a->type < b->type)
		return -1;

	switch (a->type) {
	case CFG_STRING: return strcmp(a->v.str, b->v.str);
	case CFG_FLOAT: return a->v.r == b->v.r;
	case CFG_INT: return a->v.i == b->v.i;
	case CFG_EMPTY_ARRAY: return 0;
	}

	if (a->next && b->next)
		return compare_value(a->next, b->next);
}

static int compare_config(struct config_node *a, struct config_node *b)
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
int update_pvid_map(lvmetad_state *s, struct config_tree *vg, const char *vgid)
{
	struct config_node *pv = pvs(vg);

	if (!vgid)
		return 0;

	while (pv) {
		char *pvid = find_config_str(pv->child, "id", NULL);
		dm_hash_insert(s->pvid_map, pvid, vgid);
		pv = pv->sib;
	}

	return 1;
}

/* No locks need to be held. The pointers are never used outside of the scope of
 * this function, so they can be safely destroyed after update_metadata returns
 * (anything that might have been retained is copied). */
static int update_metadata(lvmetad_state *s, const char *_vgid, struct config_node *metadata)
{
	int retval = 0;
	lock_vgs(s);
	struct config_tree *old = dm_hash_lookup(s->vgs, _vgid);
	lock_vg(s, _vgid);
	unlock_vgs(s);

	int seq = find_config_int(metadata, "metadata/seqno", -1);
	int haveseq = -1;

	if (old)
		haveseq = find_config_int(old->root, "metadata/seqno", -1);

	if (seq < 0)
		goto out;

	if (seq == haveseq) {
		retval = 1;
		if (compare_config(metadata, old->root))
			retval = 0;
		goto out;
	}

	if (seq < haveseq) {
		// TODO: we may want to notify the client that their metadata is
		// out of date?
		retval = 1;
		goto out;
	}

	struct config_tree *cft = create_config_tree(NULL, 0);
	cft->root = clone_config_node(cft, metadata, 0);
	const char *vgid = find_config_str(cft->root, "metadata/id", NULL);

	if (!vgid)
		goto out;

	lock_pvid_map(s);

	if (haveseq >= 0 && haveseq < seq) {
		/* temporarily orphan all of our PVs */
		update_pvid_map(s, old, "#orphan");
		/* need to update what we have since we found a newer version */
		destroy_config_tree(old);
		dm_hash_remove(s->vgs, vgid);
	}

	lock_vgs(s);
	dm_hash_insert(s->vgs, vgid, cft);
	unlock_vgs(s);

	update_pvid_map(s, cft, vgid);

	unlock_pvid_map(s);
	retval = 1;
out:
	unlock_vg(s, _vgid);
	return retval;
}

static response pv_add(lvmetad_state *s, request r)
{
	struct config_node *metadata = find_config_node(r.cft->root, "metadata");
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);

	if (!pvid)
		return daemon_reply_simple("failed", "reason = %s", "need PV UUID", NULL);

	debug("pv_add %s, vgid = %s\n", pvid, vgid);

	lock_pvs(s);
	dm_hash_insert(s->pvs, pvid, (void*)1);
	unlock_pvs(s);

	if (metadata) {
		if (!vgid)
			return daemon_reply_simple("failed", "reason = %s", "need VG UUID", NULL);
		if (daemon_request_int(r, "metadata/seqno", -1) < 0)
			return daemon_reply_simple("failed", "reason = %s", "need VG seqno", NULL);

		if (!update_metadata(s, vgid, metadata))
			return daemon_reply_simple("failed", "reason = %s",
						   "metadata update failed", NULL);
	} else {
		lock_pvid_map(s);
		vgid = dm_hash_lookup(s->pvid_map, pvid);
		unlock_pvid_map(s);
	}

	int complete = 0;
	if (vgid) {
		struct config_tree *cft = lock_vg(s, vgid);
		complete = update_pv_status(s, cft, cft->root, 0);
		unlock_vg(s, vgid);
	}

	return daemon_reply_simple("OK",
				   "status = %s", complete ? "complete" : "partial",
				   "vgid = %s", vgid ? vgid : "#orphan",
				   NULL);
}

static void pv_del(lvmetad_state *s, request r)
{
}

static response handler(daemon_state s, client_handle h, request r)
{
	lvmetad_state *state = s.private;
	const char *rq = daemon_request_str(r, "request", "NONE");

	debug("REQUEST: %s\n", rq);

	if (!strcmp(rq, "pv_add"))
		return pv_add(state, r);
	else if (!strcmp(rq, "pv_del"))
		pv_del(state, r);
	else if (!strcmp(rq, "vg_by_uuid"))
		return vg_by_uuid(state, r);

	return daemon_reply_simple("OK", NULL);
}

static int init(daemon_state *s)
{
	lvmetad_state *ls = s->private;

	ls->pvs = dm_hash_create(32);
	ls->vgs = dm_hash_create(32);
	ls->pvid_map = dm_hash_create(32);

	ls->lock.vg = dm_hash_create(32);
	pthread_mutexattr_t rec;
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
	debug("fini\n");
	lvmetad_state *ls = s->private;
	struct dm_hash_node *n = dm_hash_get_first(ls->vgs);
	while (n) {
		destroy_config_tree(dm_hash_get_data(ls->vgs, n));
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
	daemon_state s;
	lvmetad_state ls;
	int _restart = 0;

	s.name = "lvmetad";
	s.private = &ls;
	s.daemon_init = init;
	s.daemon_fini = fini;
	s.handler = handler;
	s.socket_path = "/var/run/lvm/lvmetad.socket";
	s.pidfile = "/var/run/lvm/lvmetad.pid";

	while ((opt = getopt(argc, argv, "?fhVdR")) != EOF) {
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
		case 'V':
			printf("lvmetad version 0\n");
			exit(1);
			break;
		}
	}

	daemon_start(s);
	return 0;
}
