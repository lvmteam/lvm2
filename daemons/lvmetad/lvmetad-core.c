#include <assert.h>

#include "libdevmapper.h"
#include <malloc.h>
#include <stdint.h>

#include "../common/daemon-server.h"

typedef struct {
	struct dm_hash_table *pvs;
	struct dm_hash_table *vgs;
	struct dm_hash_table *pvid_to_vgid;
} lvmetad_state;

static response vg_by_uuid(lvmetad_state *s, request r)
{
	const char *uuid = daemon_request_str(r, "uuid", "NONE");
	fprintf(stderr, "[D] vg_by_uuid: %s (vgs = %p)\n", uuid, s->vgs);
	struct config_tree *cft = dm_hash_lookup(s->vgs, uuid);
	if (!cft || !cft->root)
		return daemon_reply_simple("failed", "reason = %s", "uuid not found", NULL);

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
	return res;
}

static void set_flag(struct config_tree *cft, struct config_node *parent,
		     const char *field, const char *flag, int want) {
	struct config_value *value = NULL, *pred = NULL;
	struct config_node *node = find_config_node(parent->child, field);
	int found = 0;

	if (node)
		value = node->v;

	while (value && strcmp(value->v.str, flag)) {
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

struct config_node *pvs(struct config_tree *vg)
{
	struct config_node *pv = find_config_node(vg->root, "metadata/physical_volumes");
	if (pv)
		pv = pv->child;
	return pv;
}

static void update_pv_status_in_vg(lvmetad_state *s, struct config_tree *vg)
{
	struct config_node *pv = pvs(vg);
	while (pv) {
		const char *uuid = find_config_str(pv->child, "id", "N/A");
		const char *vgid = find_config_str(vg->root, "metadata/id", "N/A");
		int found = dm_hash_lookup(s->pvs, uuid) ? 1 : 0;
		set_flag(vg, pv, "status", "MISSING", !found);
		pv = pv->sib;
	}
}

static int vg_status(lvmetad_state *s, const char *vgid)
{
	struct config_tree *vg = dm_hash_lookup(s->vgs, vgid);
	struct config_node *pv = pvs(vg);

	while (pv) {
		const char *uuid = find_config_str(pv->child, "id", "N/A");
		const char *vgid = find_config_str(vg->root, "metadata/id", "N/A");
		int found = dm_hash_lookup(s->pvs, uuid) ? 1 : 0;
		if (!found)
			return 0;
		pv = pv->sib;
	}

	return 1;
}

/*
 * Walk through metadata cache and update PV flags to reflect our current
 * picture of the PVs in the system. If pvid is non-NULL, this is used as a hint
 * as to which PV has changed state. Otherwise, all flags are recomputed from
 * authoritative data (the s->pvs hash).
 */
static void update_pv_status(lvmetad_state *s, const char *pvid)
{
	if (pvid) {
		const char *vgid = dm_hash_lookup(s->pvid_to_vgid, pvid);
		assert(vgid);
		struct config_tree *vg = dm_hash_lookup(s->vgs, vgid);
		assert(vg);
		update_pv_status_in_vg(s, vg->root);
	} else {
		struct dm_hash_node *n = dm_hash_get_first(s->vgs);
		while (n) {
			struct config_tree *vg = dm_hash_get_data(s->vgs, n);
			update_pv_status_in_vg(s, vg);
			n = dm_hash_get_next(s->vgs, n);
		}
	}
}

struct config_tree *vg_from_pvid(lvmetad_state *s, const char *pvid)
{
	struct dm_hash_node *n = dm_hash_get_first(s->vgs);

	while (n) {
		struct config_tree *vg = dm_hash_get_data(s->vgs, n);
		struct config_node *pv = pvs(vg);

		while (pv) {
			const char *uuid = find_config_str(pv->child, "id", "N/A");
			if (!strcmp(uuid, pvid))
				return vg;
			pv = pv->sib;
		}

		n = dm_hash_get_next(s->vgs, n);
	}
	return NULL;
}

static int update_metadata(lvmetad_state *s, const char *vgid, struct config_node *metadata)
{
	struct config_tree *old = dm_hash_lookup(s->vgs, vgid);
	int seq = find_config_int(metadata, "metadata/seqno", -1);
	int haveseq = -1;

	if (old)
		haveseq = find_config_int(old->root, "metadata/seqno", -1);

	if (seq < 0)
		return 0; /* bad */

	if (seq == haveseq) {
		// TODO: compare old->root with metadata to ensure equality
		return 1;
	}

	if (seq < haveseq) {
		// TODO: we may want to notify the client that their metadata is
		// out of date?
		return 1;
	}

	if (haveseq >= 0 && haveseq < seq) {
		/* need to update what we have since we found a newer version */
		destroy_config_tree(old);
		dm_hash_remove(s->vgs, vgid);
	}

	struct config_tree *cft = create_config_tree(NULL, 0);
	cft->root = clone_config_node(cft, metadata, 0);
	dm_hash_insert(s->vgs, vgid, cft);

	return 1;
}

static response pv_add(lvmetad_state *s, request r)
{
	struct config_node *metadata = find_config_node(r.cft->root, "metadata");
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);

	if (!pvid)
		return daemon_reply_simple("failed", "reason = %s", "need PV UUID", NULL);

	dm_hash_insert(s->pvs, pvid, 1);

	if (metadata) {
		if (!vgid)
			return daemon_reply_simple("failed", "reason = %s", "need VG UUID", NULL);
		if (daemon_request_int(r, "metadata/seqno", -1) < 0)
			return daemon_reply_simple("failed", "reason = %s", "need VG seqno", NULL);

		if (!update_metadata(s, vgid, metadata))
			return daemon_reply_simple("failed", "reason = %s",
						   "metadata update failed", NULL);
	} else {
		struct config_tree *vg = vg_from_pvid(s, pvid);
		if (vg)
			vgid = find_config_str(vg->root, "metadata/id", NULL);
	}

	update_pv_status(s, NULL);
	int complete = vgid ? vg_status(s, vgid) : 0;

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

	fprintf(stderr, "[D] REQUEST: %s\n", rq);

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
	fprintf(stderr, "[D] initialised state: vgs = %p\n", ls->vgs);
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
	while (n) {
		destroy_config_tree(dm_hash_get_data(ls->vgs, n));
		n = dm_hash_get_next(ls->vgs, n);
	}
	dm_hash_destroy(ls->pvs);
	dm_hash_destroy(ls->vgs);
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
