#include <assert.h>

#include "libdevmapper.h"
#include <malloc.h>
#include <stdint.h>

#include "metadata-exported.h"
#include "../common/daemon-server.h"

typedef struct {
	struct dm_pool *mem;
	struct dm_hash_table *pvs;
	struct dm_hash_table *vgs;
	struct dm_hash_table *pvid_to_vgid;
} lvmetad_state;

static response vg_by_uuid(lvmetad_state *s, request r)
{
	const char *uuid = daemon_request_str(r, "uuid", "NONE");
	fprintf(stderr, "[D] vg_by_uuid: %s (vgs = %p)\n", uuid, s->vgs);
	struct config_node *metadata = dm_hash_lookup(s->vgs, uuid);
	if (!metadata)
		return daemon_reply_simple("failed", "reason = %s", "uuid not found", NULL);
	fprintf(stderr, "[D] metadata: %p\n", metadata);

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

static void update_pv_status_in_vg(lvmetad_state *s, struct config_node *vg)
{
	struct config_node *pv = find_config_node(vg, "metadata/physical_volumes");
	if (pv)
		pv = pv->child;

	while (pv) {
		const char *uuid = find_config_str(pv->child, "id", "N/A");
		if (dm_hash_lookup(s->pvs, uuid)) {
			fprintf(stderr, "[D] PV %s found\n", uuid);
		} else {
			fprintf(stderr, "[D] PV %s is MISSING\n", uuid);
		}
		pv = pv->sib;
	}
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
		struct config_node *vg = dm_hash_lookup(s->vgs, vgid);
		assert(vg);
		update_pv_status_in_vg(s, vg);
	} else {
		struct dm_hash_node *n = dm_hash_get_first(s->vgs);
		while (n) {
			struct config_node *vg = dm_hash_get_data(s->vgs, n);
			fprintf(stderr, "[D] checking VG: %s\n",
				find_config_str(vg, "metadata/id", "?"));
			update_pv_status_in_vg(s, vg);
			n = dm_hash_get_next(s->vgs, n);
		}
	}
}

static int update_metadata(lvmetad_state *s, const char *vgid, struct config_node *metadata)
{
	struct config_node *metadata_clone =
		clone_config_node_with_mem(s->mem, metadata, 0);
	/* TODO: seqno-based comparison with existing metadata version */
	dm_hash_insert(s->vgs, vgid, (void*) metadata_clone);
	fprintf(stderr, "[D] metadata stored at %p\n", metadata_clone);
}

static response pv_add(lvmetad_state *s, request r)
{
	struct config_node *metadata = find_config_node(r.cft->root, "metadata");
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	fprintf(stderr, "[D] pv_add buffer: %s\n", r.buffer);

	if (!pvid)
		return daemon_reply_simple("failed", "reason = %s", "need PV UUID", NULL);

	dm_hash_insert(s->pvs, pvid, 1);

	if (metadata) {
		const char *vgid = daemon_request_str(r, "metadata/id", NULL);
		if (!vgid)
			return daemon_reply_simple("failed", "reason = %s", "need VG UUID", NULL);

		update_metadata(s, vgid, metadata);
	}

	update_pv_status(s, NULL);

	return daemon_reply_simple("OK", NULL);
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
	ls->mem = dm_pool_create("lvmetad", 1024); /* whatever */
	fprintf(stderr, "[D] initialised state: vgs = %p\n", ls->vgs);
	if (!ls->pvs || !ls->vgs || !ls->mem)
		return 0;

	/* if (ls->initial_registrations)
	   _process_initial_registrations(ds->initial_registrations); */

	return 1;
}

static int fini(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	dm_pool_destroy(ls->mem);
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
