/*
 * Copyright (C) 2012-2015 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _XOPEN_SOURCE 500  /* pthread */

#define _REENTRANT

#include "tool.h"

#include "daemon-io.h"
#include "daemon-server.h"
#include "daemon-log.h"
#include "lvm-version.h"
#include "lvmetad-client.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>

#define LVMETAD_SOCKET DEFAULT_RUN_DIR "/lvmetad.socket"

/*
 * cache states:
 * . Empty: no devices visible to the system have been added to lvmetad
 * . Scanning: some devices visible to the system have been added to lvmetad
 * . Initialized: all devices visible to the system have been added to lvmetad
 * . Outdated: event on system or storage is not yet processed by lvmetad
 *   Outdated variations:
 *   - MissingDev: device added to system, not yet added to lvmetad
 *   - RemovedDev: device removed from system, not yet removed from lvmetad
 *   - MissingVG: new vg is written on disk, not yet added to lvmetad
 *   - RemovedVG: vg is removed on disk, not yet removed in lvmetad
 *   - ChangedVG: vg metadata is changed on disk, not yet updated in lvmetad
 *   - MissingPV: new pv is written on disk, not yet added to in lvmetad
 *   - RemovedPV: pv is removed on disk, not yet removed in lvmetad
 *   - ChangedPV: pv metadata is changed on disk, not yet updated in lvmetad
 * . Updated: events have been processed by lvmetad
 *
 * state transitions:
 * . Empty -> Scanning
 * . Scanning -> Initialized
 * . Initialized -> Scanning
 * . Initialized -> Outdated
 * . Outdated -> Updated
 * . Updated -> Outdated
 * . Updated -> Scanning
 * . Outdated -> Scanning
 *
 * state transitions caused by:
 * . Empty is caused by:
 *   - starting/restarting lvmetad
 * . Scanning is caused by:
 *   - running pvscan --cache
 *   - running any command with different global_filter (token mismatch)
 *   - running any command while lvmetad is Empty
 *   - running a report/display command with --foreign
 *   - running a report/display command with --shared
 *   - running a command using lvmlockd global lock where global state is changed
 * . Initialized is caused by:
 *   - completion of Scanning
 * . Outdated is caused by:
 *   - device being added or removed on the system
 *   - creating/removing/changing a VG
 *   - creating/removing/changing a PV
 * . Updated is caused by:
 *   - receiving and processing all events
 *
 * request handling:
 * . Empty: short period during startup, token error returned
 * . Scanning: should be very short, lvmetad responds to requests with
 *   the token error "updating"
 * . Initialized: lvmetad responds to requests
 * . Updated: lvmetad responds to requests
 * . Outdated: should be very short, lvmetad responds to requests
 *
 * In general, the cache state before and after the transition
 * "Updated -> Scanning -> Initialized" should match, unless
 * events occur during that transition.
 *
 * The Scanning state includes:
 * . receive a request to set the token to "updating" (Scanning state begins.)
 * . receive a pv_clear_all request to clear current cache
 * . receive a number of pv_found events to repopulate cache
 * . receive a request to set the token to a hash value (Initialized state begins.)
 *
 * The transition from Outdated to Updated depends on lvm commands
 * sending events to lvmetad, i.e. pv_found, pv_gone, vg_update,
 * vg_remove.  Prior to receiving these events, lvmetad is not aware
 * that it is in the Outdated state.
 *
 * When using a shared VG with lvmlockd, the Outdated state can last a
 * longer time, but it won't be used in that state.  lvmlockd forces a
 * transition "Outdated -> Scanning -> Initialized" before the cache
 * is used.
 */


/*
 * valid/invalid state of cached metadata
 *
 * Normally when using lvmetad, the state is kept up-to-date through a
 * combination of notifications from clients and updates triggered by uevents.
 * When using lvmlockd, the lvmetad state is expected to become out of
 * date (invalid/stale) when other hosts make changes to the metadata on disk.
 *
 * To deal with this, the metadata cached in lvmetad can be flagged as invalid.
 * This invalid flag is returned along with the metadata when read by a
 * command.  The command can check for the invalid flag and decide that it
 * should either use the stale metadata (uncommon), or read the latest metadata
 * from disk rather than using the invalid metadata that was returned.  If the
 * command reads the latest metadata from disk, it can choose to send it to
 * lvmetad to update the cached copy and clear the invalid flag in lvmetad.
 * Otherwise, the next command to read the metadata from lvmetad will also
 * receive the invalid metadata with the invalid flag (and like the previous
 * command, it too may choose to read the latest metadata from disk and can
 * then also choose to update the lvmetad copy.)
 *
 * For purposes of tracking the invalid state, LVM metadata is considered
 * to be either VG-specific or global.  VG-specific metadata is metadata
 * that is isolated to a VG, such as the LVs it contains.  Global
 * metadata is metadata that is not isolated to a single VG.  Global
 * metdata includes:
 * . the VG namespace (which VG names are used)
 * . the set of orphan PVs (which PVs are in VGs and which are not)
 * . properties of orphan PVs (the size of an orphan PV)
 *
 * If the metadata for a single VG becomes invalid, the VGFL_INVALID
 * flag can be set in the vg_info struct for that VG.  If the global
 * metdata becomes invalid, the GLFL_INVALID flag can be set in the
 * lvmetad daemon state.
 *
 * If a command reads VG metadata and VGFL_INVALID is set, an
 * extra config node called "vg_invalid" is added to the config
 * data returned to the command.
 *
 * If a command reads global metdata and GLFL_INVALID is set, an
 * extra config node called "global_invalid" is added to the
 * config data returned to the command.
 *
 * If a command sees vg_invalid, and wants the latest VG metadata,
 * it only needs to scan disks of the PVs in that VG.
 * It can then use vg_update to send the latest metadata to lvmetad
 * which clears the VGFL_INVALID flag.
 *
 * If a command sees global_invalid, and wants the latest metadata,
 * it should scan all devices to update lvmetad, and then send
 * lvmetad the "set_global_info global_invalid=0" message to clear
 * GLFL_INVALID.
 *
 * (When rescanning devices to update lvmetad, the command must use
 * the global filter cmd->lvmetad_filter so that it processes the same
 * devices that are seen by lvmetad.)
 *
 * The lvmetad INVALID flags can be set by sending lvmetad the messages:
 *
 * . set_vg_info with the latest VG seqno.  If the VG seqno is larger
 *   than the cached VG seqno, VGFL_INVALID is set for the VG.
 *
 * . set_global_info with global_invalid=1 sets GLFL_INVALID.
 *
 * Different entities could use these functions to invalidate metadata
 * if/when they detected that the cache is stale.  How they detect that
 * the cache is stale depends on the details of the specific entity.
 *
 * In the case of lvmlockd, it embeds values into its locks to keep track
 * of when other nodes have changed metadata on disk related to those locks.
 * When acquring locks it can look at these values and detect that
 * the metadata associated with the lock has been changed.
 * When the values change, it uses set_vg_info/set_global_info to
 * invalidate the lvmetad cache.
 *
 * The values that lvmlockd distributes through its locks are the
 * latest VG seqno in VG locks and a global counter in the global lock.
 * When a host acquires a VG lock and sees that the embedded seqno is
 * larger than it was previously, it knows that it should invalidate the
 * lvmetad cache for the VG.  If the host acquires the global lock
 * and sees that the counter is larger than previously, it knows that
 * it should invalidate the global info in lvmetad.  This invalidation
 * is done before the lock is returned to the command.  This way the
 * invalid flag will be set on the metadata before the command reads
 * it from lvmetad.
 */

struct vg_info {
	int64_t external_version;
	uint32_t flags; /* VGFL_ */
};

#define GLFL_INVALID                   0x00000001
#define GLFL_DISABLE                   0x00000002
#define GLFL_DISABLE_REASON_DIRECT     0x00000004
				    /* 0x00000008 */
#define GLFL_DISABLE_REASON_DUPLICATES 0x00000010
#define GLFL_DISABLE_REASON_VGRESTORE  0x00000020
#define GLFL_DISABLE_REASON_REPAIR     0x00000040

#define GLFL_DISABLE_REASON_ALL (GLFL_DISABLE_REASON_DIRECT | GLFL_DISABLE_REASON_REPAIR | GLFL_DISABLE_REASON_DUPLICATES | GLFL_DISABLE_REASON_VGRESTORE)

#define VGFL_INVALID 0x00000001

#define CMD_NAME_SIZE 32

typedef struct {
	daemon_idle *idle;
	log_state *log; /* convenience */
	const char *log_config;

	struct dm_hash_table *pvid_to_pvmeta;
	struct dm_hash_table *device_to_pvid; /* shares locks with above */

	struct dm_hash_table *vgid_to_metadata;
	struct dm_hash_table *vgid_to_vgname;
	struct dm_hash_table *vgid_to_outdated_pvs;
	struct dm_hash_table *vgid_to_info;
	struct dm_hash_table *vgname_to_vgid;
	struct dm_hash_table *pvid_to_vgid;
	char token[128];
	char update_cmd[CMD_NAME_SIZE];
	int update_pid;
	int update_timeout;
	uint64_t update_begin;
	uint32_t flags; /* GLFL_ */
	pthread_mutex_t token_lock;
	pthread_mutex_t info_lock;
	pthread_rwlock_t cache_lock;
} lvmetad_state;

static uint64_t _monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;
	return ts.tv_sec;
}

static void destroy_metadata_hashes(lvmetad_state *s)
{
	struct dm_hash_node *n = NULL;

	dm_hash_iterate(n, s->vgid_to_metadata)
		dm_config_destroy(dm_hash_get_data(s->vgid_to_metadata, n));

	dm_hash_iterate(n, s->vgid_to_outdated_pvs)
		dm_config_destroy(dm_hash_get_data(s->vgid_to_outdated_pvs, n));

	dm_hash_iterate(n, s->pvid_to_pvmeta)
		dm_config_destroy(dm_hash_get_data(s->pvid_to_pvmeta, n));

	dm_hash_iterate(n, s->vgid_to_vgname)
		dm_free(dm_hash_get_data(s->vgid_to_vgname, n));

	dm_hash_iterate(n, s->vgname_to_vgid)
		dm_free(dm_hash_get_data(s->vgname_to_vgid, n));

	dm_hash_iterate(n, s->vgid_to_info)
		dm_free(dm_hash_get_data(s->vgid_to_info, n));

	dm_hash_iterate(n, s->device_to_pvid)
		dm_free(dm_hash_get_data(s->device_to_pvid, n));

	dm_hash_iterate(n, s->pvid_to_vgid)
		dm_free(dm_hash_get_data(s->pvid_to_vgid, n));

	dm_hash_destroy(s->pvid_to_pvmeta);
	dm_hash_destroy(s->vgid_to_metadata);
	dm_hash_destroy(s->vgid_to_vgname);
	dm_hash_destroy(s->vgid_to_outdated_pvs);
	dm_hash_destroy(s->vgid_to_info);
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
	s->vgid_to_info = dm_hash_create(32);
	s->pvid_to_vgid = dm_hash_create(32);
	s->vgname_to_vgid = dm_hash_create(32);
}

static response reply_fail(const char *reason)
{
	return daemon_reply_simple("failed", "reason = %s", reason, NULL);
}

static response reply_unknown(const char *reason)
{
	return daemon_reply_simple("unknown", "reason = %s", reason, NULL);
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

/*
 * Either the "big" vgs lock, or a per-vg lock needs to be held before entering
 * this function.
 *
 * cft and vg is data being sent to the caller.
 */

static int update_pv_status(lvmetad_state *s,
			    struct dm_config_tree *cft,
			    struct dm_config_node *vg)
{
	struct dm_config_node *pv;
	const char *uuid;
	struct dm_config_tree *pvmeta;
	struct dm_config_node *pvmeta_cn;
	int ret = 1;

	for (pv = pvs(vg); pv; pv = pv->sib) {
		if (!(uuid = dm_config_find_str(pv->child, "id", NULL))) {
			ERROR(s, "update_pv_status found no uuid for PV");
			continue;
		}

		pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, uuid);

		set_flag(cft, pv, "status", "MISSING", !pvmeta);

		if (pvmeta) {
			if (!(pvmeta_cn = dm_config_clone_node(cft, pvmeta->root->child, 1))) {
				ERROR(s, "update_pv_status out of memory");
				ret = 0;
				goto out;
			}

			merge_pvmeta(pv, pvmeta_cn);
		}
	}
out:
	return ret;
}

static struct dm_config_node *add_last_node(struct dm_config_tree *cft, const char *node_name)
{
	struct dm_config_node *cn, *last;

	cn = cft->root;
	last = cn;

	while (cn->sib) {
		last = cn->sib;
		cn = last;
	}

	cn = dm_config_create_node(cft, node_name);
	if (!cn)
		return NULL;

	cn->v = NULL;
	cn->sib = NULL;
	cn->parent = cft->root;
	last->sib = cn;

	return cn;
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
		vgname = dm_hash_lookup(s->vgid_to_vgname, vgid);
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

	DEBUGLOG(s, "pv_list");

	buffer_init( &res.buffer );

	if (!(res.cft = dm_config_create()))
		return res; /* FIXME error reporting */

	/* The response field */
	if (!(res.cft->root = make_text_node(res.cft, "response", "OK", NULL, NULL)))
		return res; /* FIXME doomed */

	cn_pvs = make_config_node(res.cft, "physical_volumes", NULL, res.cft->root);

	dm_hash_iterate(n, s->pvid_to_pvmeta) {
		id = dm_hash_get_key(s->pvid_to_pvmeta, n);
		cn = make_pv_node(s, id, res.cft, cn_pvs, cn);
	}

	if (s->flags & GLFL_INVALID)
		add_last_node(res.cft, "global_invalid");

	return res;
}

static response pv_lookup(lvmetad_state *s, request r)
{
	const char *pvid = daemon_request_str(r, "uuid", NULL);
	int64_t devt = daemon_request_int(r, "device", 0);
	response res = { 0 };
	struct dm_config_node *pv;

	DEBUGLOG(s, "pv_lookup pvid %s", pvid);

	buffer_init( &res.buffer );

	if (!pvid && !devt)
		return reply_fail("need PVID or device");

	if (!(res.cft = dm_config_create()))
		return reply_fail("out of memory");

	if (!(res.cft->root = make_text_node(res.cft, "response", "OK", NULL, NULL)))
		return reply_fail("out of memory");

	if (!pvid && devt)
		pvid = dm_hash_lookup_binary(s->device_to_pvid, &devt, sizeof(devt));

	if (!pvid) {
		WARN(s, "pv_lookup: could not find device %" PRIu64, devt);
		dm_config_destroy(res.cft);
		return reply_unknown("device not found");
	}

	pv = make_pv_node(s, pvid, res.cft, NULL, res.cft->root);
	if (!pv) {
		dm_config_destroy(res.cft);
		return reply_unknown("PV not found");
	}

	pv->key = "physical_volume";

	if (s->flags & GLFL_INVALID)
		add_last_node(res.cft, "global_invalid");

	return res;
}

static response vg_list(lvmetad_state *s, request r)
{
	struct dm_config_node *cn, *cn_vgs, *cn_last = NULL;
	struct dm_hash_node *n;
	const char *id;
	const char *name;
	response res = { 0 };

	DEBUGLOG(s, "vg_list");

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

	if (s->flags & GLFL_INVALID)
		add_last_node(res.cft, "global_invalid");
bad:
	return res;
}

static void mark_outdated_pv(lvmetad_state *s, const char *vgid, const char *pvid)
{
	struct dm_config_tree *pvmeta, *outdated_pvs;
	struct dm_config_node *list, *cft_vgid;
	struct dm_config_value *v;

	pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, pvid);

	/* if the MDA exists and is used, it will have ignore=0 set */
	if (!pvmeta ||
	    (dm_config_find_int64(pvmeta->root, "pvmeta/mda0/ignore", 1) &&
	     dm_config_find_int64(pvmeta->root, "pvmeta/mda1/ignore", 1)))
		return;

	ERROR(s, "PV %s has outdated metadata for VG %s", pvid, vgid);

	outdated_pvs = dm_hash_lookup(s->vgid_to_outdated_pvs, vgid);
	if (!outdated_pvs) {
		if (!(outdated_pvs = config_tree_from_string_without_dup_node_check("outdated_pvs/pv_list = []")) ||
		    !(cft_vgid = make_text_node(outdated_pvs, "vgid", dm_pool_strdup(outdated_pvs->mem, vgid),
						outdated_pvs->root, NULL)))
			abort();
		if (!dm_hash_insert(s->vgid_to_outdated_pvs, cft_vgid->v->v.str, outdated_pvs))
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
	struct vg_info *info;
	response res = { 0 };
	const char *uuid = daemon_request_str(r, "uuid", NULL);
	const char *name = daemon_request_str(r, "name", NULL);
	int count = 0;

	buffer_init( &res.buffer );

	if (!uuid && !name) {
		ERROR(s, "vg_lookup with no uuid or name");
		return reply_unknown("VG not found");

	} else if (!uuid || !name) {
		DEBUGLOG(s, "vg_lookup vgid %s name %s needs lookup",
			 uuid ?: "none", name ?: "none");

		if (name && !uuid)
			uuid = dm_hash_lookup_with_count(s->vgname_to_vgid, name, &count);
		else if (uuid && !name)
			name = dm_hash_lookup(s->vgid_to_vgname, uuid);

		if (name && uuid && (count > 1)) {
			DEBUGLOG(s, "vg_lookup name %s vgid %s found %d vgids",
				 name, uuid, count);
			return daemon_reply_simple("multiple", "reason = %s", "Multiple VGs found with same name", NULL);
		}

		if (!uuid || !name)
			return reply_unknown("VG not found");

	} else {
		char *name_lookup = dm_hash_lookup(s->vgid_to_vgname, uuid);
		char *uuid_lookup = dm_hash_lookup_with_val(s->vgname_to_vgid, name, uuid, strlen(uuid) + 1);

		/* FIXME: comment out these sanity checks when not testing */

		if (!name_lookup || !uuid_lookup) {
			ERROR(s, "vg_lookup vgid %s name %s found incomplete mapping uuid %s name %s",
			      uuid, name, uuid_lookup ?: "none", name_lookup ?: "none");
			return reply_unknown("VG mapping incomplete");
		} else if (strcmp(name_lookup, name) || strcmp(uuid_lookup, uuid)) {
			ERROR(s, "vg_lookup vgid %s name %s found inconsistent mapping uuid %s name %s",
			      uuid, name, uuid_lookup, name_lookup);
			return reply_unknown("VG mapping inconsistent");
		}
	}

	DEBUGLOG(s, "vg_lookup vgid %s name %s", uuid ?: "none", name ?: "none");

	cft = dm_hash_lookup(s->vgid_to_metadata, uuid);
	if (!cft || !cft->root) {
		return reply_unknown("UUID not found");
	}

	metadata = cft->root;
	if (!(res.cft = dm_config_create()))
		goto nomem_un;

	/* The response field */
	if (!(res.cft->root = n = dm_config_create_node(res.cft, "response")))
		goto nomem_un;

	if (!(n->v = dm_config_create_value(cft)))
		goto nomem_un;

	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = "OK";

	if (!(n = n->sib = dm_config_create_node(res.cft, "name")))
		goto nomem_un;

	if (!(n->v = dm_config_create_value(res.cft)))
		goto nomem_un;

	n->parent = res.cft->root;
	n->v->type = DM_CFG_STRING;
	n->v->v.str = name;

	/* The metadata section */
	if (!(n = n->sib = dm_config_clone_node(res.cft, metadata, 1)))
		goto nomem_un;
	n->parent = res.cft->root;

	if (!update_pv_status(s, res.cft, n))
		goto nomem;
	chain_outdated_pvs(s, uuid, res.cft, n);

	if (s->flags & GLFL_INVALID)
		add_last_node(res.cft, "global_invalid");

	info = dm_hash_lookup(s->vgid_to_info, uuid);
	if (info && (info->flags & VGFL_INVALID)) {
		if (!add_last_node(res.cft, "vg_invalid"))
			goto nomem;
	}

	return res;

nomem_un:
nomem:
	reply_fail("out of memory");
	ERROR(s, "vg_lookup vgid %s name %s out of memory.", uuid ?: "none", name ?: "none");
	ERROR(s, "lvmetad could not be updated and is aborting.");
	exit(EXIT_FAILURE);
}

static int vg_remove_if_missing(lvmetad_state *s, const char *vgid, int update_pvids);

enum update_pvid_mode { UPDATE_ONLY, REMOVE_EMPTY, MARK_OUTDATED };

/* You need to be holding the pvid_to_vgid lock already to call this. */
static int _update_pvid_to_vgid(lvmetad_state *s, struct dm_config_tree *vg,
				const char *vgid, int mode)
{
	struct dm_config_node *pv;
	struct dm_hash_table *to_check;
	struct dm_hash_node *n;
	const char *pvid;
	char *vgid_old;
	char *vgid_dup;
	const char *check_vgid;
	int r = 0;

	if (!vgid)
		return 0;

	if (!(to_check = dm_hash_create(32)))
		goto abort_daemon;

	for (pv = pvs(vg->root); pv; pv = pv->sib) {
		if (!(pvid = dm_config_find_str(pv->child, "id", NULL))) {
			ERROR(s, "PV has no id for update_pvid_to_vgid");
			continue;
		}

		vgid_old = dm_hash_lookup(s->pvid_to_vgid, pvid);

		if ((mode == REMOVE_EMPTY) && vgid_old) {
			/* This copies the vgid_old string, doesn't reference it. */
			if ((dm_hash_lookup(to_check, vgid_old) != (void*) 1) &&
			    !dm_hash_insert(to_check, vgid_old, (void*) 1)) {
				ERROR(s, "update_pvid_to_vgid out of memory for hash insert vgid_old %s", vgid_old);
				goto abort_daemon;
			}
		}

		if (mode == MARK_OUTDATED)
			mark_outdated_pv(s, vgid, pvid);

		if (!(vgid_dup = dm_strdup(vgid))) {
			ERROR(s, "update_pvid_to_vgid out of memory for vgid %s", vgid);
			goto abort_daemon;
		}

		if (!dm_hash_insert(s->pvid_to_vgid, pvid, vgid_dup)) {
			ERROR(s, "update_pvid_to_vgid out of memory for hash insert vgid %s", vgid_dup);
			dm_free(vgid_dup);
			goto abort_daemon;
		}

		/* pvid_to_vgid no longer references vgid_old */
		dm_free(vgid_old);

		DEBUGLOG(s, "moving PV %s to VG %s", pvid, vgid);
	}

	dm_hash_iterate(n, to_check) {
		check_vgid = dm_hash_get_key(to_check, n);
		vg_remove_if_missing(s, check_vgid, 0);
	}

	r = 1;
	dm_hash_destroy(to_check);

	return r;

abort_daemon:
	ERROR(s, "lvmetad could not be updated and is aborting.");
	if (to_check)
		dm_hash_destroy(to_check);
	exit(EXIT_FAILURE);
}

/* A pvid map lock needs to be held if update_pvids = 1. */
static int remove_metadata(lvmetad_state *s, const char *vgid, int update_pvids)
{
	struct dm_config_tree *meta_lookup;
	struct dm_config_tree *outdated_pvs_lookup;
	struct vg_info *info_lookup;
	char *name_lookup = NULL;
	char *vgid_lookup = NULL;

	/* get data pointers from hash table so they can be freed */

	info_lookup = dm_hash_lookup(s->vgid_to_info, vgid);
	meta_lookup = dm_hash_lookup(s->vgid_to_metadata, vgid);
	name_lookup = dm_hash_lookup(s->vgid_to_vgname, vgid);
	outdated_pvs_lookup = dm_hash_lookup(s->vgid_to_outdated_pvs, vgid);
	if (name_lookup)
		vgid_lookup = dm_hash_lookup_with_val(s->vgname_to_vgid, name_lookup, vgid, strlen(vgid) + 1);

	/* remove hash table mappings */

	dm_hash_remove(s->vgid_to_info, vgid);
	dm_hash_remove(s->vgid_to_metadata, vgid);
	dm_hash_remove(s->vgid_to_vgname, vgid);
	dm_hash_remove(s->vgid_to_outdated_pvs, vgid);
	if (name_lookup)
		dm_hash_remove_with_val(s->vgname_to_vgid, name_lookup, vgid, strlen(vgid) + 1);

	/* update_pvid_to_vgid will clear/free the pvid_to_vgid hash */
	if (update_pvids && meta_lookup)
		(void) _update_pvid_to_vgid(s, meta_lookup, "#orphan", 0);

	/* free the unmapped data */

	if (meta_lookup)
		dm_config_destroy(meta_lookup);
	if (outdated_pvs_lookup)
		dm_config_destroy(outdated_pvs_lookup);
	dm_free(info_lookup);
	dm_free(name_lookup);
	dm_free(vgid_lookup);
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

	return 1;
}

/*
 * Remove all hash table references to arg_name and arg_vgid
 * so that new metadata using this name and/or vgid can be added
 * without interference previous data.
 *
 * This is used if a command updates metadata in the cache,
 * but update_metadata finds that what's in the cache is not
 * consistent with a normal transition between old and new
 * metadata.  If this happens, it assumes that the command
 * is providing the correct metadata, so it first calls this
 * function to purge all records of the old metadata so the
 * new metadata can be added.
 */

static void _purge_metadata(lvmetad_state *s, const char *arg_name, const char *arg_vgid)
{
	char *rem_vgid;

	remove_metadata(s, arg_vgid, 1);

	if ((rem_vgid = dm_hash_lookup_with_val(s->vgname_to_vgid, arg_name, arg_vgid, strlen(arg_vgid) + 1))) {
		dm_hash_remove_with_val(s->vgname_to_vgid, arg_name, arg_vgid, strlen(arg_vgid) + 1);
		dm_free(rem_vgid);
	}
}

/*
 * Updates for new vgid and new metadata.
 *
 * Remove any existing vg_info struct since it will be
 * recreated by lvmlockd if/when needed.
 *
 * Remove any existing outdated pvs since their metadata
 * will no longer be associated with this VG.
 */

static int _update_metadata_new_vgid(lvmetad_state *s,
				     const char *arg_name,
				     const char *old_vgid,
				     const char *new_vgid,
				     struct dm_config_tree *old_meta,
				     struct dm_config_tree *new_meta)
{
	struct vg_info *rem_info;
	struct dm_config_tree *rem_outdated;
	char *new_vgid_dup = NULL;
	char *arg_name_dup = NULL;
	int abort_daemon = 0;
	int retval = 0;

	if (!(new_vgid_dup = dm_strdup(new_vgid)))
		goto ret;

	if (!(arg_name_dup = dm_strdup(arg_name)))
		goto ret;

	/*
	 * Temporarily orphan the PVs in the old metadata.
	 */
	if (!_update_pvid_to_vgid(s, old_meta, "#orphan", 0)) {
		ERROR(s, "update_metadata_new_vgid failed to move PVs for %s old_vgid %s", arg_name, old_vgid);
		abort_daemon = 1;
		goto ret;
	}

	/*
	 * Remove things related to the old vgid. (like remove_metadata)
	 */

	if ((rem_info = dm_hash_lookup(s->vgid_to_info, old_vgid))) {
		dm_hash_remove(s->vgid_to_info, old_vgid);
		dm_free(rem_info);
	}

	if ((rem_outdated = dm_hash_lookup(s->vgid_to_outdated_pvs, old_vgid))) {
		dm_hash_remove(s->vgid_to_outdated_pvs, old_vgid);
		dm_config_destroy(rem_outdated);
	}

	dm_hash_remove(s->vgid_to_metadata, old_vgid);
	dm_config_destroy(old_meta);
	old_meta = NULL;

	dm_hash_remove_with_val(s->vgname_to_vgid, arg_name, old_vgid, strlen(old_vgid) + 1);
	dm_hash_remove(s->vgid_to_vgname, old_vgid);
	dm_free((char *)old_vgid);
	old_vgid = NULL;

	/*
	 * Insert things with the new vgid.
	 */

	if (!dm_hash_insert(s->vgid_to_metadata, new_vgid, new_meta)) {
		ERROR(s, "update_metadata_new_vgid out of memory for meta hash insert for %s %s", arg_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	if (!dm_hash_insert(s->vgid_to_vgname, new_vgid, arg_name_dup)) {
		ERROR(s, "update_metadata_new_vgid out of memory for name hash insert for %s %s", arg_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	if (!dm_hash_insert_allow_multiple(s->vgname_to_vgid, arg_name, new_vgid_dup, strlen(new_vgid_dup) + 1)) {
		ERROR(s, "update_metadata_new_vgid out of memory for vgid hash insert for %s %s", arg_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	/*
	 * Reassign PVs based on the new metadata.
	 */
	if (!_update_pvid_to_vgid(s, new_meta, new_vgid, 1)) {
		ERROR(s, "update_metadata_new_name failed to update PVs for %s %s", arg_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	DEBUGLOG(s, "update_metadata_new_vgid is done for %s %s", arg_name, new_vgid);
	retval = 1;
out:
ret:
	if (!new_vgid_dup || !arg_name_dup || abort_daemon) {
		ERROR(s, "lvmetad could not be updated and is aborting.");
		exit(EXIT_FAILURE);
	}

	if (!retval && new_meta)
		dm_config_destroy(new_meta);
	return retval;
}

/*
 * Updates for new name and new metadata.
 *
 * Remove any existing vg_info struct since it will be
 * recreated by lvmlockd if/when needed.
 *
 * Remove any existing outdated pvs since their metadata
 * will no longer be associated with this VG.
 */

static int _update_metadata_new_name(lvmetad_state *s,
				     const char *arg_vgid,
				     const char *old_name,
				     const char *new_name,
				     struct dm_config_tree *old_meta,
				     struct dm_config_tree *new_meta)
{
	struct vg_info *rem_info;
	struct dm_config_tree *rem_outdated;
	char *new_name_dup = NULL;
	char *arg_vgid_dup = NULL;
	int abort_daemon = 0;
	int retval = 0;

	if (!(new_name_dup = dm_strdup(new_name)))
		goto ret;

	if (!(arg_vgid_dup = dm_strdup(arg_vgid)))
		goto ret;

	/*
	 * Temporarily orphan the PVs in the old metadata.
	 */
	if (!_update_pvid_to_vgid(s, old_meta, "#orphan", 0)) {
		ERROR(s, "update_metadata_new_name failed to move PVs for old_name %s %s", old_name, arg_vgid);
		abort_daemon = 1;
		goto ret;
	}

	/*
	 * Remove things related to the old name.
	 */

	if ((rem_info = dm_hash_lookup(s->vgid_to_info, arg_vgid))) {
		dm_hash_remove(s->vgid_to_info, arg_vgid);
		dm_free(rem_info);
	}

	if ((rem_outdated = dm_hash_lookup(s->vgid_to_outdated_pvs, arg_vgid))) {
		dm_hash_remove(s->vgid_to_outdated_pvs, arg_vgid);
		dm_config_destroy(rem_outdated);
	}

	dm_hash_remove(s->vgid_to_metadata, arg_vgid);
	dm_config_destroy(old_meta);
	old_meta = NULL;

	dm_hash_remove(s->vgid_to_vgname, arg_vgid);
	dm_hash_remove_with_val(s->vgname_to_vgid, old_name, arg_vgid, strlen(arg_vgid) + 1);
	dm_free((char *)old_name);
	old_name = NULL;

	/*
	 * Insert things with the new name.
	 */

	if (!dm_hash_insert(s->vgid_to_metadata, arg_vgid, new_meta)) {
		ERROR(s, "update_metadata_new_name out of memory for meta hash insert for %s %s", new_name, arg_vgid);
		abort_daemon = 1;
		goto out;
	}

	if (!dm_hash_insert(s->vgid_to_vgname, arg_vgid, new_name_dup)) {
		ERROR(s, "update_metadata_new_name out of memory for name hash insert for %s %s", new_name, arg_vgid);
		abort_daemon = 1;
		goto out;
	}

	if (!dm_hash_insert_allow_multiple(s->vgname_to_vgid, new_name, arg_vgid_dup, strlen(arg_vgid_dup) + 1)) {
		ERROR(s, "update_metadata_new_name out of memory for vgid hash insert for %s %s", new_name, arg_vgid);
		abort_daemon = 1;
		goto out;
	}

	/*
	 * Reassign PVs based on the new metadata.
	 */
	if (!_update_pvid_to_vgid(s, new_meta, arg_vgid, 1)) {
		ERROR(s, "update_metadata_new_name failed to update PVs for %s %s", new_name, arg_vgid);
		abort_daemon = 1;
		goto out;
	}

	DEBUGLOG(s, "update_metadata_new_name is done for %s %s", new_name, arg_vgid);
	retval = 1;
out:
ret:
	if (!new_name_dup || !arg_vgid_dup || abort_daemon) {
		ERROR(s, "lvmetad could not be updated and is aborting.");
		exit(EXIT_FAILURE);
	}

	if (!retval && new_meta)
		dm_config_destroy(new_meta);
	return retval;
}


/*
 * Add new entries to all hash tables.
 */

static int _update_metadata_add_new(lvmetad_state *s, const char *new_name, const char *new_vgid,
				    struct dm_config_tree *new_meta)
{
	char *new_name_dup = NULL;
	char *new_vgid_dup = NULL;
	int abort_daemon = 0;
	int retval = 0;

	DEBUGLOG(s, "update_metadata_add_new for %s %s", new_name, new_vgid);

	if (!(new_name_dup = dm_strdup(new_name)))
		goto out_free;

	if (!(new_vgid_dup = dm_strdup(new_vgid)))
		goto out_free;

	if (!dm_hash_insert(s->vgid_to_metadata, new_vgid, new_meta)) {
		ERROR(s, "update_metadata_add_new out of memory for meta hash insert for %s %s", new_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	if (!dm_hash_insert(s->vgid_to_vgname, new_vgid, new_name_dup)) {
		ERROR(s, "update_metadata_add_new out of memory for name hash insert for %s %s", new_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	if (!dm_hash_insert_allow_multiple(s->vgname_to_vgid, new_name, new_vgid_dup, strlen(new_vgid_dup) + 1)) {
		ERROR(s, "update_metadata_add_new out of memory for vgid hash insert for %s %s", new_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	if (!_update_pvid_to_vgid(s, new_meta, new_vgid, 1)) {
		ERROR(s, "update_metadata_add_new failed to update PVs for %s %s", new_name, new_vgid);
		abort_daemon = 1;
		goto out;
	}

	DEBUGLOG(s, "update_metadata_add_new is done for %s %s", new_name, new_vgid);
	retval = 1;
out:
out_free:
	if (!new_name_dup || !new_vgid_dup || abort_daemon) {
		dm_free(new_name_dup);
		dm_free(new_vgid_dup);
		ERROR(s, "lvmetad could not be updated and is aborting.");
		exit(EXIT_FAILURE);
	}

	if (!retval && new_meta)
		dm_config_destroy(new_meta);
	return retval;
}

/*
 * No locks need to be held. The pointers are never used outside of the scope of
 * this function, so they can be safely destroyed after update_metadata returns
 * (anything that might have been retained is copied).
 *
 * When this is called from pv_found, the metadata was read from a single
 * PV specified by the pvid arg and ret_old_seq is not NULL.  The metadata
 * should match the existing metadata (matching seqno).  If the metadata
 * from pv_found has a smaller seqno, it means that the PV is outdated
 * (was previously used in the VG and now reappeared after changes to the VG).
 * The next command to access the VG will erase the outdated PV and then clear
 * the outdated pv record here.  If the metadata from pv_found has a larger
 * seqno than the existing metadata, it means ... (existing pvs are outdated?)
 *
 * When this is caleld from vg_update, the metadata is from a command that
 * has new metadata that should replace the existing metadata.
 * pvid and ret_old_seq are both NULL.
 */

static int _update_metadata(lvmetad_state *s, const char *arg_name, const char *arg_vgid,
			    struct dm_config_node *new_metadata, int *ret_old_seq,
			    const char *pvid)
{
	struct dm_config_tree *old_meta = NULL;
	struct dm_config_tree *new_meta = NULL;
	const char *arg_name_lookup; /* name lookup result from arg_vgid */
	const char *arg_vgid_lookup; /* vgid lookup result from arg_name */
	const char *old_name = NULL;
	const char *new_name = NULL;
	const char *old_vgid = NULL;
	const char *new_vgid = NULL;
	const char *new_metadata_vgid;
	int new_seq;
	int old_seq = -1;
	int needs_repair = 0;
	int abort_daemon = 0;
	int retval = 0;
	int count = 0;

	if (!arg_vgid || !arg_name) {
		ERROR(s, "update_metadata missing args arg_vgid %s arg_name %s pvid %s",
		      arg_vgid ?: "none", arg_name ?: "none", pvid ?: "none");
		return 0;
	}

	DEBUGLOG(s, "update_metadata begin arg_vgid %s arg_name %s pvid %s",
		 arg_vgid, arg_name, pvid ?: "none");

	/*
	 * Begin by figuring out what has changed:
	 * . the VG could be new - found no existing record of the vgid or name.
	 * . the VG could have a new vgid - found an existing record of the name.
	 * . the VG could have a new name - found an existing record of the vgid.
	 * . the VG could have unchanged vgid and name - found existing record of both.
	 */

	arg_name_lookup = dm_hash_lookup(s->vgid_to_vgname, arg_vgid);
	arg_vgid_lookup = dm_hash_lookup_with_val(s->vgname_to_vgid, arg_name, arg_vgid, strlen(arg_vgid) + 1);

	/*
	 * A new VG when there is no existing record of the name or vgid args.
	 */
	if (!arg_name_lookup && !arg_vgid_lookup) {
		new_vgid = arg_vgid;
		new_name = arg_name;

		DEBUGLOG(s, "update_metadata new name %s and new vgid %s",
			 new_name, new_vgid);
		goto update;
	}

	/*
	 * An existing name has a new vgid (new_vgid = arg_vgid).
	 * A lookup of the name arg was successful in finding arg_vgid_lookup,
	 * but that resulting vgid doesn't match the arg_vgid.
	 */
	if (arg_vgid_lookup && strcmp(arg_vgid_lookup, arg_vgid)) {
		if (arg_name_lookup) {
			/*
			 * This shouldn't happen.
			 * arg_vgid should be new and should not map to any name.
			 */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s unexpected arg_name_lookup %s",
			      arg_vgid, arg_name, arg_name_lookup);
			needs_repair = 1;
			goto update;
		}

		new_vgid = arg_vgid;
		old_vgid = dm_hash_lookup_with_count(s->vgname_to_vgid, arg_name, &count);

		/*
		 * FIXME: this ensures that arg_name maps to only one existing
		 * VG (old_vgid), because if it maps to multiple vgids, then we
		 * don't know which one should get the new vgid (arg_vgid).  If
		 * this function was given both the existing name and existing
		 * vgid to identify the VG, then this wouldn't be a problem.
		 * But as it is now, the vgid arg to this function is the new
		 * vgid and the existing VG is specified only by name.
		 */
		if (old_vgid && (count > 1)) {
			ERROR(s, "update_metadata arg_vgid %s arg_name %s found %d vgids for name",
			      arg_vgid, arg_name, count);
			old_vgid = NULL;
		}

		if (!old_vgid) {
			/* This shouldn't happen. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s no old_vgid",
			      arg_vgid, arg_name);
			needs_repair = 1;
			goto update;
		}

		if (!(old_meta = dm_hash_lookup(s->vgid_to_metadata, old_vgid))) {
			/* This shouldn't happen. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s old_vgid %s no old_meta",
			      arg_vgid, arg_name, old_vgid);
			needs_repair = 1;
			goto update;
		}

		DEBUGLOG(s, "update_metadata existing name %s has new vgid %s old vgid %s",
			 arg_name, new_vgid, old_vgid);
		goto update;
	}

	/*
	 * An existing vgid has a new name (new_name = arg_name).
	 * A lookup of the vgid arg was successful in finding arg_name_lookup,
	 * but that resulting name doesn't match the arg_name.
	 */
	if (arg_name_lookup && strcmp(arg_name_lookup, arg_name)) {
		if (arg_vgid_lookup) {
			/*
			 * This shouldn't happen.
			 * arg_name should be new and should not map to any vgid.
			 */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s unexpected arg_vgid_lookup %s",
			      arg_vgid, arg_name, arg_vgid_lookup);
			needs_repair = 1;
			goto update;
		}

		new_name = arg_name;
		old_name = dm_hash_lookup(s->vgid_to_vgname, arg_vgid);

		if (!old_name) {
			/* This shouldn't happen. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s no old_name",
			      arg_vgid, arg_name);
			needs_repair = 1;
			goto update;
		}

		if (!(old_meta = dm_hash_lookup(s->vgid_to_metadata, arg_vgid))) {
			/* This shouldn't happen. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s old_name %s no old_meta",
			      arg_vgid, arg_name, old_name);
			needs_repair = 1;
			goto update;
		}

		DEBUGLOG(s, "update_metadata existing vgid %s has new name %s old name %s",
			 arg_vgid, new_name, old_name);
		goto update;
	}

	/*
	 * An existing VG has unchanged name and vgid.
	 */
	if (!new_vgid && !new_name) {
		if (!arg_vgid_lookup || !arg_name_lookup) {
			/* This shouldn't happen. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s missing lookups vgid %s name %s",
			      arg_vgid ?: "none", arg_name ?: "none", arg_vgid_lookup ?: "none", arg_name_lookup ?: "none");
			needs_repair = 1;
			goto update;
		}

		if (strcmp(arg_name_lookup, arg_name)) {
			/* This shouldn't happen. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s mismatch arg_name_lookup %s",
			      arg_vgid, arg_name, arg_name_lookup);
			needs_repair = 1;
			goto update;
		}

		if (strcmp(arg_vgid_lookup, arg_vgid)) {
			/* This shouldn't happen.  Two VGs with the same name is handled above. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s mismatch arg_vgid_lookup %s",
			      arg_vgid, arg_name, arg_vgid_lookup);
			needs_repair = 1;
			goto update;
		}

		/* old_vgid == arg_vgid, and old_name == arg_name */

		if (!(old_meta = dm_hash_lookup(s->vgid_to_metadata, arg_vgid))) {
			/* This shouldn't happen. */
			ERROR(s, "update_metadata arg_vgid %s arg_name %s no old_meta",
			      arg_vgid, arg_name);
			needs_repair = 1;
			goto update;
		}

		DEBUGLOG(s, "update_metadata existing vgid %s and existing name %s",
			 arg_vgid, arg_name);
		goto update;
	}

 update:
	filter_metadata(new_metadata); /* sanitize */

	/*
	 * FIXME: verify that there's at least one PV in common between
	 * the old and new metadata?
	 */

	if (!(new_meta = dm_config_create()) ||
	    !(new_meta->root = dm_config_clone_node(new_meta, new_metadata, 0))) {
		ERROR(s, "update_metadata out of memory for new metadata for %s %s",
		      arg_name, arg_vgid);
		/* FIXME: should we purge the old metadata here? */
		retval = 0;
		goto out;
	}

	/*
	 * Get the seqno from existing (old) and new metadata and perform
	 * sanity checks for transitions that generally shouldn't happen.
	 * Sometimes ignore the new metadata and leave the existing metadata
	 * alone, and sometimes purge the existing metadata and add the new.
	 * This often depends on whether the new metadata comes from a single
	 * PV (via pv_found) that's been scanned, or a vg_update sent from a
	 * command.
	 */

	new_seq = dm_config_find_int(new_metadata, "metadata/seqno", -1);

	if (old_meta)
		old_seq = dm_config_find_int(old_meta->root, "metadata/seqno", -1);

	if (ret_old_seq)
		*ret_old_seq = old_meta ? old_seq : new_seq;

	/*
	 * The new metadata has an invalid seqno.
	 * This shouldn't happen, but if it does, ignore the new metadata.
	 */
	if (new_seq <= 0) {
		ERROR(s, "update_metadata ignore new metadata because of invalid seqno for %s %s",
		      arg_vgid, arg_name);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		retval = 0;
		goto out;
	}

	/*
	 * The new metadata is missing an internal vgid.
	 * This shouldn't happen, but if it does, ignore the new metadata.
	 */
	if (!(new_metadata_vgid = dm_config_find_str(new_meta->root, "metadata/id", NULL))) {
		ERROR(s, "update_metadata has no internal vgid for %s %s",
		      arg_name, arg_vgid);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		retval = 0;
		goto out;
	}

	/*
	 * The new metadata internal vgid doesn't match the arg vgid.
	 * This shouldn't happen, but if it does, ignore the new metadata.
	 */
	if (strcmp(new_metadata_vgid, arg_vgid)) {
		ERROR(s, "update_metadata has bad internal vgid %s for %s %s",
		      new_metadata_vgid, arg_name, arg_vgid);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		retval = 0;
		goto out;
	}

	/*
	 * A single PV appears with metadata that's inconsistent with
	 * existing, ignore the PV.  FIXME: make it outdated?
	 */
	if (pvid && needs_repair) {
		ERROR(s, "update_metadata ignore inconsistent metadata on PV %s seqno %d for %s %s seqno %d",
		      pvid, new_seq, arg_vgid, arg_name, old_seq);
		if (old_meta)
			DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		retval = 0;
		goto out;
	}

	/*
	 * A VG update with metadata that's inconsistent with existing.
	 */
	if (!pvid && needs_repair) {
		ERROR(s, "update_metadata inconsistent with cache for vgid %s and name %s",
		      arg_vgid, arg_name);
		if (old_meta)
			DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		abort_daemon = 1;
		retval = 0;
		goto out;
	}

	/*
	 * A single PV appears with metadata that's older than the existing,
	 * e.g. an PV that had been in the VG has reappeared after the VG changed.
	 * old PV: the PV that lvmetad was told about first
	 * new PV: the PV that lvmetad is being told about here, second
	 * old_seq: the larger seqno on the old PV, for the newer version of the VG
	 * new_seq: the smaller seqno on the new PV, for the older version of the VG
	 *
	 * So, the new PV (by notification order) is "older" (in terms of
	 * VG seqno) than the old PV.
	 *
	 * Make the new PV outdated so it'll be cleared and keep the existing
	 * metadata from the old PV.
	 */
	if (pvid && (old_seq > 0) && (new_seq < old_seq)) {
		ERROR(s, "update_metadata ignoring outdated metadata on PV %s seqno %d for %s %s seqno %d",
		      pvid, new_seq, arg_vgid, arg_name, old_seq);
		DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		mark_outdated_pv(s, arg_vgid, pvid);
		retval = 0;
		goto out;
	}

	/*
	 * A single PV appears with metadata that's newer than the existing,
	 * e.g. a PV has been found with VG metadata that is newer than the
	 * VG metdata we know about.  This can happen when scanning PVs after
	 * an outdated PV (with an older version of the VG metadata) has
	 * reappeared.  The rescanning may initially scan the outdated PV
	 * and notify lvmetad about it, and then scan a current PV from
	 * the VG and notify lvmetad about it.
	 * old PV: the PV that lvmetad was told about first
	 * new PV: the PV that lvmetad is being told about here, second
	 * old_seq: the smaller seqno on the old PV, for the older version of the VG
	 * new_seq: the larger seqno on the new PV, for the newer version of the VG
	 *
	 * Make the existing PVs outdated, and use the new metadata.
	 */
	if (pvid && (old_seq > 0) && (new_seq > old_seq)) {
		ERROR(s, "update_metadata found newer metadata on PV %s seqno %d for %s %s seqno %d",
		      pvid, new_seq, arg_vgid, arg_name, old_seq);
		DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		_update_pvid_to_vgid(s, old_meta, arg_vgid, MARK_OUTDATED);
	}

	/*
	 * The existing/old metadata has an invalid seqno.
	 * This shouldn't happen, but if it does, purge old and add the new.
	 */
	if (old_meta && (old_seq <= 0)) {
		ERROR(s, "update_metadata bad old seqno %d for %s %s",
		      old_seq, arg_name, arg_vgid);
		DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		_purge_metadata(s, arg_name, arg_vgid);
		new_name = arg_name;
		new_vgid = arg_vgid;
		old_name = NULL;
		old_vgid = NULL;
		old_meta = NULL;
		old_seq = -1;
	}

	/*
	 * A single PV appears with a seqno matching existing metadata,
	 * but unmatching metadata content.  This shouldn't happen,
	 * but if it does, ignore the PV.  FIXME: make it outdated?
	 */
	if (pvid && (new_seq == old_seq) && compare_config(new_metadata, old_meta->root)) {
		ERROR(s, "update_metadata from pv %s same seqno %d with unmatching data for %s %s",
		      pvid, new_seq, arg_name, arg_vgid);
		DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		retval = 0;
		goto out;
	}

	/*
	 * A VG update with metadata matching existing seqno but unmatching content.
	 * This shouldn't happen, but if it does, purge existing and add the new.
	 */
	if (!pvid && (new_seq == old_seq) && compare_config(new_metadata, old_meta->root)) {
		ERROR(s, "update_metadata same seqno %d with unmatching data for %s %s",
		      new_seq, arg_name, arg_vgid);
		DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		_purge_metadata(s, arg_name, arg_vgid);
		new_name = arg_name;
		new_vgid = arg_vgid;
		old_name = NULL;
		old_vgid = NULL;
		old_meta = NULL;
		old_seq = -1;
	}

	/*
	 * A VG update with metadata older than existing.  VG updates should
	 * have increasing seqno.  This shouldn't happen, but if it does,
	 * purge existing and add the new.
	 */
	if (!pvid && (new_seq < old_seq)) {
		ERROR(s, "update_metadata new seqno %d less than old seqno %d for %s %s",
		      new_seq, old_seq, arg_name, arg_vgid);
		DEBUGLOG_cft(s, "OLD: ", old_meta->root);
		DEBUGLOG_cft(s, "NEW: ", new_metadata);
		_purge_metadata(s, arg_name, arg_vgid);
		new_name = arg_name;
		new_vgid = arg_vgid;
		old_name = NULL;
		old_vgid = NULL;
		old_meta = NULL;
		old_seq = -1;
	}

	/*
	 * All the checks are done, do one of the four possible updates
	 * outlined above:
	 */

	/*
	 * Add metadata for a new VG to the cache.
	 */
	if (new_name && new_vgid)
		return _update_metadata_add_new(s, new_name, new_vgid, new_meta);

	/*
	 * Update cached metadata for a VG with a new vgid.
	 */
	if (new_vgid)
		return _update_metadata_new_vgid(s, arg_name, old_vgid, new_vgid, old_meta, new_meta);

	/*
	 * Update cached metadata for a renamed VG.
	 */
	if (new_name)
		return _update_metadata_new_name(s, arg_vgid, old_name, new_name, old_meta, new_meta);

	/*
	 * If the old and new seqnos are the same, we've already compared the
	 * old/new metadata and verified it's the same, so there's no reason
	 * to replace old meta with new meta.
	 */
	if (old_seq == new_seq) {
		DEBUGLOG(s, "update_metadata skipped for %s %s seqno %d is unchanged",
			 arg_name, arg_vgid, old_seq);
		dm_config_destroy(new_meta);
		new_meta = NULL;
		retval = 1;
		goto out;
	}

	/*
	 * Update cached metdata for a VG with unchanged name and vgid.
	 * Replace the old metadata with the new metadata.
	 * old_meta is the old copy of the metadata from the cache.
	 * new_meta is the new copy of the metadata from the command.
	 */
	DEBUGLOG(s, "update_metadata for %s %s from %d to %d", arg_name, arg_vgid, old_seq, new_seq);

	/*
	 * The PVs in the VG may have changed in the new metadata, so
	 * temporarily orphan all of the PVs in the existing VG.
	 * The PVs that are still in the VG will be reassigned to this
	 * VG below by the next call to _update_pvid_to_vgid().
	 */
	if (!_update_pvid_to_vgid(s, old_meta, "#orphan", 0)) {
		ERROR(s, "update_metadata failed to move PVs for %s %s", arg_name, arg_vgid);
		abort_daemon = 1;
		retval = 0;
		goto out;
	}

	/*
	 * The only hash table update that is needed is the actual
	 * metadata config tree in vgid_to_metadata.  The VG name
	 * and vgid are unchanged.
	 */

	dm_hash_remove(s->vgid_to_metadata, arg_vgid);
	dm_config_destroy(old_meta);
	old_meta = NULL;

	if (!dm_hash_insert(s->vgid_to_metadata, arg_vgid, new_meta)) {
		ERROR(s, "update_metadata out of memory for hash insert for %s %s", arg_name, arg_vgid);
		abort_daemon = 1;
		retval = 0;
		goto out;
	}

	/*
	 * Map the PVs in the new metadata to the vgid.
	 * All pre-existing PVs were temporarily orphaned above.
	 * Previous PVs that were removed from the VG will not
	 * be remapped.  New PVs that were added to the VG will
	 * be newly mapped to this vgid, and previous PVs that
	 * remain in the VG will be remapped to the VG again.
	 */
	if (!_update_pvid_to_vgid(s, new_meta, arg_vgid, 1)) {
		ERROR(s, "update_metadata failed to update PVs for %s %s", arg_name, arg_vgid);
		abort_daemon = 1;
		retval = 0;
	} else {
		DEBUGLOG(s, "update_metadata is done for %s %s", arg_name, arg_vgid);
		retval = 1;
	}

out:
	if (abort_daemon) {
		ERROR(s, "lvmetad could not be updated is aborting.");
		exit(EXIT_FAILURE);
	}

	if (!retval && new_meta)
		dm_config_destroy(new_meta);
	return retval;
}

static response pv_gone(lvmetad_state *s, request r)
{
	const char *arg_pvid = NULL;
	char *old_pvid = NULL;
	const char *pvid;
	int64_t device;
	struct dm_config_tree *pvmeta;
	char *vgid;

	arg_pvid = daemon_request_str(r, "uuid", NULL);
	device = daemon_request_int(r, "device", 0);

	if (!arg_pvid && device > 0)
		old_pvid = dm_hash_lookup_binary(s->device_to_pvid, &device, sizeof(device));

	if (!arg_pvid && !old_pvid) {
		DEBUGLOG(s, "pv_gone device %" PRIu64 " not found", device);
		return reply_unknown("device not in cache");
	}

	pvid = arg_pvid ? arg_pvid : old_pvid;

	DEBUGLOG(s, "pv_gone %s device %" PRIu64, pvid ?: "none", device);

	if (!(pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, pvid))) {
		DEBUGLOG(s, "pv_gone %s device %" PRIu64 " has no PV metadata",
			 pvid ?: "none", device);
		return reply_unknown("PVID does not exist");
	}

	vgid = dm_hash_lookup(s->pvid_to_vgid, pvid);

	dm_hash_remove_binary(s->device_to_pvid, &device, sizeof(device));
	dm_hash_remove(s->pvid_to_pvmeta, pvid);

	if (vgid) {
		char *vgid_dup;
		/*
		 * vg_remove_if_missing will clear and free the pvid_to_vgid
		 * mappings for this vg, which will free the "vgid" string that
		 * was returned above from the pvid_to_vgid lookup.
		 */
		if (!(vgid_dup = dm_strdup(vgid)))
			return reply_fail("out of memory");

		vg_remove_if_missing(s, vgid_dup, 1);
		dm_free(vgid_dup);
		vgid_dup = NULL;
		vgid = NULL;
	}

	dm_config_destroy(pvmeta);
	dm_free(old_pvid);

	return daemon_reply_simple("OK", NULL );
}

static response pv_clear_all(lvmetad_state *s, request r)
{
	DEBUGLOG(s, "pv_clear_all");

	destroy_metadata_hashes(s);
	create_metadata_hashes(s);

	return daemon_reply_simple("OK", NULL);
}

/*
 * Returns 1 if PV metadata exists for all PVs in a VG.
 */
static int _vg_is_complete(lvmetad_state *s, struct dm_config_tree *vgmeta)
{
	struct dm_config_node *vg = vgmeta->root;
	struct dm_config_node *pv;
	int complete = 1;
	const char *pvid;

	for (pv = pvs(vg); pv; pv = pv->sib) {
		if (!(pvid = dm_config_find_str(pv->child, "id", NULL)))
			continue;

		if (!dm_hash_lookup(s->pvid_to_pvmeta, pvid)) {
			complete = 0;
			break;
		}
	}

	return complete;
}

/*
 * pv_found: a PV has appeared and been scanned
 * It contains PV metadata, and optionally VG metadata.
 * Both kinds of metadata should be added to the cache
 * and hash table mappings related to the PV and device
 * should be updated.
 *
 * Input values from request:
 * . arg_pvmeta:  PV metadata from the found pv
 * . arg_pvid:    pvid from arg_pvmeta (pvmeta/id)
 * . arg_device:  device from arg_pvmeta (pvmeta/device)
 * . arg_vgmeta:  VG metadata from the found pv (optional)
 * . arg_name:    VG name from found pv (optional)
 * . arg_vgid:    VG vgid from arg_vgmeta  (optional)
 *
 * Search for existing mappings in hash tables:
 * . pvid_to_pvmeta (which produces pvid to device)
 * . device_to_pvid
 * . pvid_to_vgid
 *
 * Existing data from cache:
 * . old_pvmeta:         result of pvid_to_pvmeta(arg_pvid)
 * . arg_device_lookup:  result of old_pvmeta:pvmeta/device using arg_pvid
 * . arg_pvid_lookup:    result of device_to_pvid(arg_device)
 * . arg_vgid_lookup:    result of pvid_to_vgid(arg_pvid)
 *
 * When arg_pvid doesn't match arg_pvid_lookup:
 * . a new PV replaces a previous PV on arg_device
 * . prev_pvid_on_dev:   set to arg_pvid_lookup, pvid of the prev PV
 * . prev_pvmeta_on_dev: result pvid_to_pvmeta(prev_pvid_on_dev)
 * . prev_vgid_on_dev:   result of pvid_to_vgid(prev_pvid_on_dev)
 *
 * Old PV on old device
 * . no PV/device mappings have changed
 * . arg_pvid_lookup == arg_pvid && arg_device_lookup == arg_device
 * . arg_device was used to look up a PV and found a PV with
 *   the same pvid as arg_pvid
 * . arg_pvid was used to look up a PV and found a PV on the
 *   same device as arg_device
 * . new_pvmeta may be more recent than old_pvmeta
 *
 * New PV on new device
 * . add new mappings in hash tables
 * . !arg_pvid_lookup && !arg_device_lookup
 * . arg_device was used to look up a PV and found nothing
 * . arg_pvid was used to look up a PV and found nothing
 *
 * New PV on old device
 * . a new PV replaces a previous PV on a device
 * . arg_pvid_lookup != arg_pvid
 * . arg_device was used to look up a PV and found a PV with
 *   a different pvid than arg_pvid
 * . replace existing mappings for arg_device and arg_pvid
 * . replace existing old_pvmeta with new_pvmeta
 * . remove arg_device association with prev PV (prev_pvid_on_dev)
 * . possibly remove prev PV (if arg_device was previously a duplicate)
 *
 * Old PV on new device
 * . a duplicate PV
 * . arg_device_lookup != arg_device
 * . arg_pvid was used to look up a PV, and found that the PV
 *   has a different device than arg_device.
 */

static response pv_found(lvmetad_state *s, request r)
{
	struct dm_config_node *arg_vgmeta = NULL;
	struct dm_config_node *arg_pvmeta = NULL;
	struct dm_config_tree *old_pvmeta = NULL;
	struct dm_config_tree *new_pvmeta = NULL;
	struct dm_config_tree *prev_pvmeta_on_dev = NULL;
	struct dm_config_tree *vgmeta = NULL;
	const char *arg_pvid = NULL;
	const char *arg_pvid_lookup = NULL;
	const char *new_pvid = NULL;
	char *new_pvid_dup = NULL;
	const char *arg_name = NULL;
	const char *arg_vgid = NULL;
	const char *arg_vgid_lookup = NULL;
	const char *prev_pvid_on_dev = NULL;
	const char *prev_vgid_on_dev = NULL;
	const char *vg_status = NULL;
	uint64_t arg_device = 0;
	uint64_t arg_device_lookup = 0;
	uint64_t new_device = 0;
	uint64_t old_device = 0;
	int arg_seqno = -1;
	int old_seqno = -1;
	int vg_status_seqno = -1;
	int changed = 0;

	/*
	 * New input values.
	 */

	if (!(arg_pvmeta = dm_config_find_node(r.cft->root, "pvmeta"))) {
		ERROR(s, "Ignore PV without PV metadata");
		return reply_fail("Ignore PV without PV metadata");
	}

	if (!(arg_pvid = daemon_request_str(r, "pvmeta/id", NULL))) {
		ERROR(s, "Ignore PV without PV UUID");
		return reply_fail("Ignore PV without PV UUID");
	}

	if (!dm_config_get_uint64(arg_pvmeta, "pvmeta/device", &arg_device)) {
		ERROR(s, "Ignore PV without device pvid %s", arg_pvid);
		return reply_fail("Ignore PV without device");
	}

	if ((arg_vgmeta = dm_config_find_node(r.cft->root, "metadata"))) {
		arg_name = daemon_request_str(r, "vgname", NULL);
		arg_vgid = daemon_request_str(r, "metadata/id", NULL);
		arg_seqno = daemon_request_int(r, "metadata/seqno", -1);

		if (!arg_name || !arg_vgid || (arg_seqno < 0))
			ERROR(s, "Ignore VG metadata from PV %s", arg_pvid);
		if (!arg_name)
			return reply_fail("Ignore VG metadata from PV without VG name");
		if (!arg_vgid)
			return reply_fail("Ignore VG metadata from PV without VG vgid");
		if (arg_seqno < 0)
			return reply_fail("Ignore VG metadata from PV without VG seqno");
	}

	/* Make a copy of the new pvmeta that can be inserted into cache. */
	if (!(new_pvmeta = dm_config_create()) ||
	    !(new_pvmeta->root = dm_config_clone_node(new_pvmeta, arg_pvmeta, 0))) {
		ERROR(s, "pv_found out of memory for new pvmeta %s", arg_pvid);
		goto nomem;
	}

	/*
	 * Existing (old) cache values.
	 */

	old_pvmeta = dm_hash_lookup(s->pvid_to_pvmeta, arg_pvid);
	if (old_pvmeta)
		dm_config_get_uint64(old_pvmeta->root, "pvmeta/device", &arg_device_lookup);

	arg_pvid_lookup = dm_hash_lookup_binary(s->device_to_pvid, &arg_device, sizeof(arg_device));

	/*
	 * Determine which of the four possible changes is happening
	 * by comparing the existing/old and new values:
	 * old PV, old device
	 * new PV, new device
	 * new PV, old device
	 * old PV, new device
	 */

	if (arg_pvid_lookup && arg_device_lookup &&
	    (arg_device == arg_device_lookup) &&
	    !strcmp(arg_pvid_lookup, arg_pvid)) {
		/*
		 * Old PV on old device (existing values unchanged)
		 */
		new_pvid = NULL;
		new_device = 0;

		DEBUGLOG(s, "pv_found pvid %s on device %" PRIu64 " matches existing",
			arg_pvid, arg_device);

	} else if (!arg_pvid_lookup && !arg_device_lookup) {
		/*
		 * New PV on new device (no existing values)
		 */
		new_pvid = arg_pvid;
		new_device = arg_device;

		DEBUGLOG(s, "pv_found pvid %s on device %" PRIu64 " is new",
			arg_pvid, arg_device);

	} else if (arg_pvid_lookup && strcmp(arg_pvid_lookup, arg_pvid)) {
		/*
		 * New PV on old device (existing device reused for new PV)
		 */
		new_pvid = arg_pvid;
		new_device = 0;
		prev_pvid_on_dev = arg_pvid_lookup;
		prev_pvmeta_on_dev = dm_hash_lookup(s->pvid_to_pvmeta, arg_pvid_lookup);
		prev_vgid_on_dev = dm_hash_lookup(s->pvid_to_vgid, arg_pvid_lookup);

		DEBUGLOG(s, "pv_found pvid %s vgid %s on device %" PRIu64 " previous pvid %s vgid %s",
			arg_pvid, arg_vgid ?: "none", arg_device,
			prev_pvid_on_dev, prev_vgid_on_dev ?: "none");

	} else if (arg_device_lookup && (arg_device_lookup != arg_device)) {
		/*
		 * Old PV on new device (existing PV on a new device, i.e. duplicate)
		 */
		new_device = arg_device;
		new_pvid = NULL;
		old_device = arg_device_lookup;

		DEBUGLOG(s, "pv_found pvid %s vgid %s on device %" PRIu64 " duplicate %" PRIu64,
			arg_pvid, arg_vgid ?: "none", arg_device, arg_device_lookup);

	} else {
		ERROR(s, "pv_found pvid %s vgid %s on device %" PRIu64 " unknown lookup %s %s %" PRIu64,
		      arg_pvid,
		      arg_vgid ?: "none",
		      arg_device,
		      arg_pvid_lookup ?: "none",
		      arg_vgid_lookup ?: "none",
		      arg_device_lookup);
		return reply_fail("Ignore PV for unknown state");
	}

	/*
	 * Make changes to hashes device_to_pvid and pvid_to_pvmeta for each case.
	 */

	if (!new_pvid && !new_device) {
		/*
		 * Old PV on old device (unchanged)
		 * . add new_pvmeta, replacing old_pvmeta
		 */
		if (compare_config(old_pvmeta->root, new_pvmeta->root))
			changed |= 1;

		if (!dm_hash_insert(s->pvid_to_pvmeta, arg_pvid, new_pvmeta))
			goto nomem_free1;

	} else if (new_pvid && new_device) {
		/*
		 * New PV on new device (new entry)
		 * . add new_device/new_pvid mapping
		 * . add new_pvmeta
		 */
		changed |= 1;

		DEBUGLOG(s, "pv_found new entry device_to_pvid %" PRIu64 " to %s",
			 new_device, new_pvid);

		if (!(new_pvid_dup = dm_strdup(new_pvid)))
			goto nomem_free1;

		if (!dm_hash_insert_binary(s->device_to_pvid, &new_device, sizeof(new_device), new_pvid_dup))
			goto nomem_free2;

		if (!dm_hash_insert(s->pvid_to_pvmeta, new_pvid, new_pvmeta))
			goto nomem_free1;

	} else if (new_pvid && !new_device) {
		/*
		 * New PV on old device (existing device reused for new PV).
		 * The previous PV on arg_device is replaced by the new one.
		 *
		 * Don't free prev_pvid or prev_vgid strings because they are
		 * used at the end to check the VG metadata.
		 */
		changed |= 1;

		if (prev_pvmeta_on_dev) {
			DEBUGLOG(s, "pv_found new pvid device_to_pvid %" PRIu64 " to %s removes prev pvid %s",
				 arg_device, new_pvid, prev_pvid_on_dev);

			dm_hash_remove(s->pvid_to_pvmeta, prev_pvid_on_dev);
			dm_config_destroy(prev_pvmeta_on_dev);
			prev_pvmeta_on_dev = NULL;

			/* removes arg_device/prev_pvid_on_dev mapping */
			dm_hash_remove_binary(s->device_to_pvid, &arg_device, sizeof(arg_device));

			/*
			 * The new PV replacing the prev PV was copied from
			 * another existing PV, creating a duplicate PV which
			 * we ignore.
			 */
			if (dm_hash_lookup(s->pvid_to_pvmeta, new_pvid)) {
				DEBUGLOG(s, "pv_found ignore duplicate device %" PRIu64 " of existing PV for pvid %s",
				         arg_device, arg_pvid);
				dm_config_destroy(new_pvmeta);
				/* device_to_pvid no longer references prev_pvid_lookup */
				dm_free((void*)prev_pvid_on_dev);
				s->flags |= GLFL_DISABLE;
				s->flags |= GLFL_DISABLE_REASON_DUPLICATES;
				return reply_fail("Ignore duplicate PV");
			}
		}


		if (!(new_pvid_dup = dm_strdup(new_pvid)))
			goto nomem_free1;

		if (!dm_hash_insert_binary(s->device_to_pvid, &arg_device, sizeof(arg_device), new_pvid_dup))
			goto nomem_free2;

		if (!dm_hash_insert(s->pvid_to_pvmeta, new_pvid, new_pvmeta))
			goto nomem_free1;

	} else if (new_device && !new_pvid) {
		/*
		 * Old PV on new device (duplicate)
		 * Ignore it.
		 */
		DEBUGLOG(s, "pv_found ignore duplicate device %" PRIu64 " of existing device %" PRIu64 " for pvid %s",
			 new_device, old_device, arg_pvid);
		dm_config_destroy(new_pvmeta);
		s->flags |= GLFL_DISABLE;
		s->flags |= GLFL_DISABLE_REASON_DUPLICATES;
		return reply_fail("Ignore duplicate PV");
	}

	if (old_pvmeta)
		dm_config_destroy(old_pvmeta);

	/*
	 * Update VG metadata cache with arg_vgmeta from the PV, or
	 * if the PV holds no VG metadata, then look up the vgid and
	 * name of the VG so we can check if the VG is complete.
	 */
	if (arg_vgmeta) {
		DEBUGLOG(s, "pv_found pvid %s has VG %s %s seqno %d", arg_pvid, arg_name, arg_vgid, arg_seqno);

		if (!_update_metadata(s, arg_name, arg_vgid, arg_vgmeta, &old_seqno, arg_pvid)) {
			ERROR(s, "Cannot use VG metadata for %s %s from PV %s on %" PRIu64,
			      arg_name, arg_vgid, arg_pvid, arg_device);
		}

		changed |= (old_seqno != arg_seqno);
	} else {
		arg_vgid = dm_hash_lookup(s->pvid_to_vgid, arg_pvid);

		if (arg_vgid) {
			arg_name = dm_hash_lookup(s->vgid_to_vgname, arg_vgid);
		}
	}

	/*
	 * Check if the VG is complete (all PVs have been found) because
	 * the reply indicates if the the VG is complete or partial.
	 * The "vgmeta" from dm_hash_lookup will be a copy of arg_vgmeta that
	 * was cloned and added to the cache by update_metadata.
	 */
	if (!arg_vgid || !strcmp(arg_vgid, "#orphan")) {
		DEBUGLOG(s, "pv_found pvid %s on %" PRIu64 " not in VG %s",
			 arg_pvid, arg_device, arg_vgid ?: "");
		vg_status = "orphan";
		goto prev_vals;
	}

	if (!(vgmeta = dm_hash_lookup(s->vgid_to_metadata, arg_vgid))) {
		ERROR(s, "pv_found %s on %" PRIu64 " vgid %s no VG metadata found",
		      arg_pvid, arg_device, arg_vgid);
	} else {
		vg_status = _vg_is_complete(s, vgmeta) ? "complete" : "partial";
		vg_status_seqno = dm_config_find_int(vgmeta->root, "metadata/seqno", -1);
	}

 prev_vals:
	/*
	 * If the device previously held a different VG (prev_vgid_on_dev),
	 * then that VG should be removed if no devices are left for it.
	 *
	 * The mapping from the device's previous pvid to the previous vgid
	 * is removed.
	 */

	if (prev_pvid_on_dev || prev_vgid_on_dev) {
		DEBUGLOG(s, "pv_found pvid %s on %" PRIu64 " had prev pvid %s prev vgid %s",
			 arg_pvid, arg_device,
			 prev_pvid_on_dev ?: "none",
			 prev_vgid_on_dev ?: "none");
	}

	if (prev_vgid_on_dev) {
		char *tmp_vgid;

	       	if (!arg_vgid || strcmp(arg_vgid, prev_vgid_on_dev)) {
			tmp_vgid = dm_strdup(prev_vgid_on_dev);
			/* vg_remove_if_missing will clear and free
			   the string pointed to by prev_vgid_on_dev. */
			vg_remove_if_missing(s, tmp_vgid, 1);
			dm_free(tmp_vgid);
		}

		/* vg_remove_if_missing may have remapped prev_pvid_on_dev to orphan */
		if ((tmp_vgid = dm_hash_lookup(s->pvid_to_vgid, prev_pvid_on_dev))) {
			dm_hash_remove(s->pvid_to_vgid, prev_pvid_on_dev);
			dm_free(tmp_vgid);
		}
	}

	/* This was unhashed from device_to_pvid above. */
	dm_free((void *)prev_pvid_on_dev);

	return daemon_reply_simple("OK",
				   "status = %s", vg_status,
				   "changed = " FMTd64, (int64_t) changed,
				   "vgid = %s", arg_vgid ? arg_vgid : "#orphan",
				   "vgname = %s", arg_name ? arg_name : "#orphan",
				   "seqno_before = " FMTd64, (int64_t) old_seqno,
				   "seqno_after = " FMTd64, (int64_t) vg_status_seqno,
				   NULL);

 nomem_free2:
	dm_free(new_pvid_dup);
 nomem_free1:
	dm_config_destroy(new_pvmeta);
 nomem:
	ERROR(s, "pv_found %s is out of memory.", arg_pvid);
	ERROR(s, "lvmetad could not be updated is aborting.");
	reply_fail("out of memory");
	exit(EXIT_FAILURE);
}

static response vg_clear_outdated_pvs(lvmetad_state *s, request r)
{
	struct dm_config_tree *outdated_pvs;
	const char *vgid = daemon_request_str(r, "vgid", NULL);

	if (!vgid)
		return reply_fail("need VG UUID");

	DEBUGLOG(s, "vg_clear_outdated_pvs vgid %s", vgid);

	if ((outdated_pvs = dm_hash_lookup(s->vgid_to_outdated_pvs, vgid))) {
		dm_config_destroy(outdated_pvs);
		dm_hash_remove(s->vgid_to_outdated_pvs, vgid);
	}
	return daemon_reply_simple("OK", NULL);
}

static void vg_info_update(lvmetad_state *s, const char *uuid,
                           struct dm_config_node *metadata)
{
	struct vg_info *info;
	int64_t cache_version;

	cache_version = dm_config_find_int64(metadata, "metadata/seqno", -1);
	if (cache_version == -1)
		return;

	info = (struct vg_info *) dm_hash_lookup(s->vgid_to_info, uuid);
	if (!info)
		return;

	if (cache_version >= info->external_version)
		info->flags &= ~VGFL_INVALID;
}

static response vg_update(lvmetad_state *s, request r)
{
	struct dm_config_node *metadata = dm_config_find_node(r.cft->root, "metadata");
	const char *vgid = daemon_request_str(r, "metadata/id", NULL);
	const char *vgname = daemon_request_str(r, "vgname", NULL);

	DEBUGLOG(s, "vg_update vgid %s name %s", vgid ?: "none", vgname ?: "none");

	if (metadata) {
		if (!vgid) {
			ERROR(s, "vg_update failed: need VG UUID");
			reply_fail("vg_update: need VG UUID");
			goto fail;
		}
		if (!vgname) {
			ERROR(s, "vg_update failed: need VG name");
			reply_fail("vg_update: need VG name");
			goto fail;
		}
		if (daemon_request_int(r, "metadata/seqno", -1) < 0) {
			ERROR(s, "vg_update failed: need VG seqno");
			reply_fail("vg_update: need VG seqno");
			goto fail;
		}

		/* TODO defer metadata update here; add a separate vg_commit
		 * call; if client does not commit, die */

		if (!_update_metadata(s, vgname, vgid, metadata, NULL, NULL)) {
			ERROR(s, "vg_update failed: metadata update failed");
			reply_fail("vg_update: failed metadata update");
			goto fail;
		}

		vg_info_update(s, vgid, metadata);
	}
	return daemon_reply_simple("OK", NULL);

fail:
	ERROR(s, "lvmetad could not be updated is aborting.");
	exit(EXIT_FAILURE);
}

static response vg_remove(lvmetad_state *s, request r)
{
	const char *vgid = daemon_request_str(r, "uuid", NULL);

	if (!vgid)
		return reply_fail("need VG UUID");

	DEBUGLOG(s, "vg_remove: %s", vgid);

	remove_metadata(s, vgid, 1);

	return daemon_reply_simple("OK", NULL);
}

/*
 * Whether lvmetad is disabled is determined only by the single
 * flag GLFL_DISABLE.  The REASON flags are only explanatory
 * additions to GLFL_DISABLE, and do not control the disabled state.
 * The REASON flags can accumulate if multiple reasons exist for
 * the disabled flag.  When clearing GLFL_DISABLE, all REASON flags
 * are cleared.  The caller clearing GLFL_DISABLE should only do so
 * when all the reasons for it have gone.
 */

static response set_global_info(lvmetad_state *s, request r)
{
	const int global_invalid = daemon_request_int(r, "global_invalid", -1);
	const int global_disable = daemon_request_int(r, "global_disable", -1);
	const char *reason;
	uint32_t reason_flags = 0;

	if ((reason = daemon_request_str(r, "disable_reason", NULL))) {
		if (strstr(reason, LVMETAD_DISABLE_REASON_DIRECT))
			reason_flags |= GLFL_DISABLE_REASON_DIRECT;
		if (strstr(reason, LVMETAD_DISABLE_REASON_REPAIR))
			reason_flags |= GLFL_DISABLE_REASON_REPAIR;
		if (strstr(reason, LVMETAD_DISABLE_REASON_DUPLICATES))
			reason_flags |= GLFL_DISABLE_REASON_DUPLICATES;
		if (strstr(reason, LVMETAD_DISABLE_REASON_VGRESTORE))
			reason_flags |= GLFL_DISABLE_REASON_VGRESTORE;
	}

	if (global_invalid != -1) {
		DEBUGLOG(s, "set global info invalid from %d to %d",
			 (s->flags & GLFL_INVALID) ? 1 : 0, global_invalid);
	}

	if (global_disable != -1) {
		DEBUGLOG(s, "set global info disable from %d to %d %s",
			 (s->flags & GLFL_DISABLE) ? 1 : 0, global_disable,
			 reason ? reason : "");
	}

	if (global_invalid == 1)
		s->flags |= GLFL_INVALID;

	else if (global_invalid == 0)
		s->flags &= ~GLFL_INVALID;

	if (global_disable == 1) {
		s->flags |= GLFL_DISABLE;
		s->flags |= reason_flags;

	} else if (global_disable == 0) {
		s->flags &= ~GLFL_DISABLE;
		s->flags &= ~GLFL_DISABLE_REASON_ALL;
	}

	return daemon_reply_simple("OK", NULL);
}

#define REASON_BUF_SIZE 64

/*
 * Save the time when "updating" begins, and the config setting for how long
 * the update is allowed to take.  Before returning "updating" as the token
 * value in get_global_info, check if the update has exceeded the max allowed
 * time.  If so, then return "none" as the current token value (i.e.
 * uninitialized), so that the command will repopulate our cache.
 *
 * This automatically clears a stuck update, where a command started to update
 * the cache and then failed, leaving the token set to "update in progress".
 */

static response get_global_info(lvmetad_state *s, request r)
{
	/* This buffer should be large enough to hold all the possible reasons. */
	char reason[REASON_BUF_SIZE] = { 0 };
	char flag_str[64];
	int pid;

	pid = (int)daemon_request_int(r, "pid", 0);

	if (s->flags & GLFL_DISABLE) {
		snprintf(reason, REASON_BUF_SIZE, "%s%s%s%s",
			 (s->flags & GLFL_DISABLE_REASON_DIRECT)     ? LVMETAD_DISABLE_REASON_DIRECT "," : "",
			 (s->flags & GLFL_DISABLE_REASON_REPAIR)     ? LVMETAD_DISABLE_REASON_REPAIR "," : "",
			 (s->flags & GLFL_DISABLE_REASON_DUPLICATES) ? LVMETAD_DISABLE_REASON_DUPLICATES "," : "",
			 (s->flags & GLFL_DISABLE_REASON_VGRESTORE)  ? LVMETAD_DISABLE_REASON_VGRESTORE "," : "");
	}

	if (!reason[0])
		strcpy(reason, "none");

	/*
	 * If the current update has timed out, then return
	 * token of "none" which means "uninitialized" so that
	 * the caller will repopulate lvmetad.
	 */
	if (s->update_begin && s->update_timeout) {
		if (_monotonic_seconds() - s->update_begin >= s->update_timeout) {
			DEBUGLOG(s, "global info cancel update after timeout %d len %d begin %llu pid %d cmd %s",
				 s->update_timeout,
				 (int)(_monotonic_seconds() - s->update_begin),
				 (unsigned long long)s->update_begin,
				 s->update_pid, s->update_cmd);
			memset(s->token, 0, sizeof(s->token));
			s->update_begin = 0;
			s->update_timeout = 0;
			s->update_pid = 0;
			memset(s->update_cmd, 0, CMD_NAME_SIZE);
		}
	}

	memset(flag_str, 0, sizeof(flag_str));
	if (s->flags & GLFL_INVALID)
		strcat(flag_str, "Invalid");
	if (s->flags & GLFL_DISABLE)
		strcat(flag_str, "Disable");
	if (!flag_str[0])
		strcat(flag_str, "none");

	DEBUGLOG(s, "%d global info flags %s reason %s token %s update_pid %d",
		 pid, flag_str, reason, s->token[0] ? s->token : "none", s->update_pid);

	return daemon_reply_simple("OK", "global_invalid = " FMTd64, (int64_t)((s->flags & GLFL_INVALID) ? 1 : 0),
					 "global_disable = " FMTd64, (int64_t)((s->flags & GLFL_DISABLE) ? 1 : 0),
					 "disable_reason = %s", reason,
					 "daemon_pid = " FMTd64, (int64_t)getpid(),
					 "token = %s", s->token[0] ? s->token : "none",
					 "update_cmd = %s", s->update_cmd,
					 "update_pid = " FMTd64, (int64_t)s->update_pid,
					 "update_begin = " FMTd64, (int64_t)s->update_begin,
					 "update_timeout = " FMTd64, (int64_t)s->update_timeout,
					 NULL);
}

static response set_vg_info(lvmetad_state *s, request r)
{
	struct dm_config_tree *vg;
	struct vg_info *info;
	const char *name = NULL;
	const char *uuid = NULL;
	const int64_t new_version = daemon_request_int(r, "version", -1);
	int64_t cache_version = -1;

	if (new_version == -1)
		goto out;

	if (!(uuid = daemon_request_str(r, "uuid", NULL)))
		goto use_name;

	if ((vg = dm_hash_lookup(s->vgid_to_metadata, uuid)))
		goto vers;
use_name:
	if (!(name = daemon_request_str(r, "name", NULL)))
		goto out;

	if (!(uuid = dm_hash_lookup(s->vgname_to_vgid, name)))
		goto out;

	/* 
	 * FIXME: if we only have the name and multiple VGs have that name,
	 * then invalidate each of them.
	 */

	if (!(vg = dm_hash_lookup(s->vgid_to_metadata, uuid)))
		goto out;
vers:
	if (!new_version)
		goto inval;

	cache_version = dm_config_find_int64(vg->root, "metadata/seqno", -1);

	if (cache_version != -1 && new_version != -1 && cache_version >= new_version)
		goto out;
inval:
	DEBUGLOG(s, "set info VG name %s uuid %s cache_version %d new_version %d",
		 name ?: "none", uuid ?: "none", (int)cache_version, (int)new_version);

	info = dm_hash_lookup(s->vgid_to_info, uuid);
	if (!info) {
		if (!(info = dm_zalloc(sizeof(struct vg_info))))
			goto bad;
		if (!dm_hash_insert(s->vgid_to_info, uuid, (void*)info))
			goto bad;
	}

	info->external_version = new_version;
	info->flags |= VGFL_INVALID;

out:
	return daemon_reply_simple("OK", NULL);
bad:
	return reply_fail("out of memory");
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
		if (int_key)
			(void) dm_asprintf(&append, "    %d = \"%s\"\n", *(const int*)key, val);
		else
			(void) dm_asprintf(&append, "    %s = \"%s\"\n", key, val);
		if (append)
			buffer_append(buf, append);
		dm_free(append);
	}
	buffer_append(buf, "}\n");
}

static void _dump_info_version(struct buffer *buf, struct dm_hash_table *ht, const char *name, int int_key)
{
	char *append;
	struct dm_hash_node *n = dm_hash_get_first(ht);
	struct vg_info *info;

	buffer_append(buf, name);
	buffer_append(buf, " {\n");

	while (n) {
		const char *key = dm_hash_get_key(ht, n);
		info = dm_hash_get_data(ht, n);
		(void) dm_asprintf(&append, "    %s = %lld\n", key, (long long)info->external_version);
		if (append)
			buffer_append(buf, append);
		dm_free(append);
		n = dm_hash_get_next(ht, n);
	}
	buffer_append(buf, "}\n");
}

static void _dump_info_flags(struct buffer *buf, struct dm_hash_table *ht, const char *name, int int_key)
{
	char *append;
	struct dm_hash_node *n = dm_hash_get_first(ht);
	struct vg_info *info;

	buffer_append(buf, name);
	buffer_append(buf, " {\n");

	while (n) {
		const char *key = dm_hash_get_key(ht, n);
		info = dm_hash_get_data(ht, n);
		(void) dm_asprintf(&append, "    %s = %llx\n", key, (long long)info->flags);
		if (append)
			buffer_append(buf, append);
		dm_free(append);
		n = dm_hash_get_next(ht, n);
	}
	buffer_append(buf, "}\n");
}

static response dump(lvmetad_state *s)
{
	response res = { 0 };
	struct buffer *b = &res.buffer;

	buffer_init(b);

	/* Lock everything so that we get a consistent dump. */

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

	buffer_append(b, "\n# VGID to INFO version mapping\n\n");
	_dump_info_version(b, s->vgid_to_info, "vgid_to_info", 0);

	buffer_append(b, "\n# VGID to INFO flags mapping\n\n");
	_dump_info_flags(b, s->vgid_to_info, "vgid_to_info", 0);

	return res;
}

static response handler(daemon_state s, client_handle h, request r)
{
	response res;
	lvmetad_state *state = s.private;
	char prev_token[128] = { 0 };
	const char *rq;
	const char *token;
	const char *cmd;
	int prev_in_progress, this_in_progress;
	int update_timeout;
	int pid;
	int cache_lock = 0;
	int info_lock = 0;

	rq = daemon_request_str(r, "request", "NONE");
	token = daemon_request_str(r, "token", "NONE");
	pid = (int)daemon_request_int(r, "pid", 0);
	cmd = daemon_request_str(r, "cmd", "NONE");
	update_timeout = (int)daemon_request_int(r, "update_timeout", 0);

	pthread_mutex_lock(&state->token_lock);

	/*
	 * token_update: start populating the cache, i.e. a full update.
	 * To populate the lvmetad cache, a command does:
	 *
	 * - token_update, setting token to "update in progress"
	 *   (further requests during the update continue using
	 *   this same "update in progress" token)
	 * - pv_clear_all, to clear the current cache
	 * - pv_gone, for each PV
	 * - pv_found, for each PV to populate the cache
	 * - token_update, setting token to filter hash
	 */
	if (!strcmp(rq, "token_update")) {
		prev_in_progress = !strcmp(state->token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS);
		this_in_progress = !strcmp(token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS);

		if (!prev_in_progress && this_in_progress) {
			/* New update is starting (filter token is replaced by update token) */

			(void) dm_strncpy(prev_token, state->token, sizeof(prev_token));
			(void) dm_strncpy(state->token, token, sizeof(state->token));
			state->update_begin = _monotonic_seconds();
			state->update_timeout = update_timeout;
			state->update_pid = pid;
			strncpy(state->update_cmd, cmd, CMD_NAME_SIZE - 1);

			DEBUGLOG(state, "token_update begin %llu timeout %d pid %d cmd %s",
				 (unsigned long long)state->update_begin,
				 state->update_timeout,
				 state->update_pid,
				 state->update_cmd);

		} else if (prev_in_progress && this_in_progress) {
			/* Current update is cancelled and replaced by a new update */

			DEBUGLOG(state, "token_update replacing pid %d begin %llu len %d cmd %s",
				 state->update_pid,
				 (unsigned long long)state->update_begin,
				 (int)(_monotonic_seconds() - state->update_begin),
				 state->update_cmd);

			(void) dm_strncpy(prev_token, state->token, sizeof(prev_token));
			(void) dm_strncpy(state->token, token, sizeof(state->token));
			state->update_begin = _monotonic_seconds();
			state->update_timeout = update_timeout;
			state->update_pid = pid;
			strncpy(state->update_cmd, cmd, CMD_NAME_SIZE - 1);

			DEBUGLOG(state, "token_update begin %llu timeout %d pid %d cmd %s",
				 (unsigned long long)state->update_begin,
				 state->update_timeout,
				 state->update_pid,
				 state->update_cmd);

		} else if (prev_in_progress && !this_in_progress) {
			/* Update is finished, update token is replaced by filter token */

			if (state->update_pid != pid) {
				/* If a pid doing update was cancelled, ignore its token update at the end. */
				DEBUGLOG(state, "token_update ignored from cancelled update pid %d", pid);
				pthread_mutex_unlock(&state->token_lock);

				return daemon_reply_simple("token_mismatch",
							   "expected = %s", state->token,
							   "received = %s", token,
							   "update_pid = " FMTd64, (int64_t)state->update_pid,
							   "reason = %s", "another command has populated the cache",
							   NULL);
			}

			DEBUGLOG(state, "token_update end len %d pid %d new token %s",
				 (int)(_monotonic_seconds() - state->update_begin),
				 state->update_pid, token);

			(void) dm_strncpy(prev_token, state->token, sizeof(prev_token));
			(void) dm_strncpy(state->token, token, sizeof(state->token));
			state->update_begin = 0;
			state->update_timeout = 0;
			state->update_pid = 0;
			memset(state->update_cmd, 0, CMD_NAME_SIZE);
		}
		pthread_mutex_unlock(&state->token_lock);

		return daemon_reply_simple("OK",
					   "prev_token = %s", prev_token,
					   "update_pid = " FMTd64, (int64_t)state->update_pid,
					   NULL);
	}

	if (strcmp(token, state->token) && strcmp(rq, "dump") && strcmp(token, "skip")) {
		pthread_mutex_unlock(&state->token_lock);

		DEBUGLOG(state, "token_mismatch current \"%s\" got \"%s\" from pid %d cmd %s",
			 state->token, token, pid, cmd ?: "none");

		return daemon_reply_simple("token_mismatch",
					   "expected = %s", state->token,
					   "received = %s", token,
					   "update_pid = " FMTd64, (int64_t)state->update_pid,
					   "reason = %s", "another command has populated the cache",
					   NULL);
	}

	/* If a pid doing update was cancelled, ignore its update messages. */
	if (!strcmp(token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS) &&
	    state->update_pid && pid && (state->update_pid != pid)) {
		pthread_mutex_unlock(&state->token_lock);

		DEBUGLOG(state, "token_mismatch ignore update from pid %d current update pid %d",
			 pid, state->update_pid);

		return daemon_reply_simple("token_mismatch",
					   "expected = %s", state->token,
					   "received = %s", token,
					   "update_pid = " FMTd64, (int64_t)state->update_pid,
					   "reason = %s", "another command has populated the lvmetad cache",
					   NULL);
	}

	pthread_mutex_unlock(&state->token_lock);


	if (!strcmp(rq, "pv_found") ||
	    !strcmp(rq, "pv_gone") ||
	    !strcmp(rq, "vg_update") ||
	    !strcmp(rq, "vg_remove") ||
	    !strcmp(rq, "set_vg_info") ||
	    !strcmp(rq, "pv_clear_all") ||
	    !strcmp(rq, "vg_clear_outdated_pvs")) {
		pthread_rwlock_wrlock(&state->cache_lock);
		cache_lock = 1;
		goto do_rq;
	}

	if (!strcmp(rq, "pv_lookup") ||
	    !strcmp(rq, "vg_lookup") ||
	    !strcmp(rq, "pv_list") ||
	    !strcmp(rq, "vg_list") ||
	    !strcmp(rq, "dump")) {
		pthread_rwlock_rdlock(&state->cache_lock);
		cache_lock = 1;
		goto do_rq;
	}

	if (!strcmp(rq, "set_global_info") ||
	    !strcmp(rq, "get_global_info")) {
		pthread_mutex_lock(&state->info_lock);
		info_lock = 1;
		goto do_rq;
	}

 do_rq:

	if (!strcmp(rq, "pv_found"))
		res = pv_found(state, r);

	else if (!strcmp(rq, "pv_gone"))
		res = pv_gone(state, r);

	else if (!strcmp(rq, "pv_clear_all"))
		res = pv_clear_all(state, r);

	else if (!strcmp(rq, "pv_lookup"))
		res = pv_lookup(state, r);

	else if (!strcmp(rq, "vg_update"))
		res = vg_update(state, r);

	else if (!strcmp(rq, "vg_clear_outdated_pvs"))
		res = vg_clear_outdated_pvs(state, r);

	else if (!strcmp(rq, "vg_remove"))
		res = vg_remove(state, r);

	else if (!strcmp(rq, "vg_lookup"))
		res = vg_lookup(state, r);

	else if (!strcmp(rq, "pv_list"))
		res = pv_list(state, r);

	else if (!strcmp(rq, "vg_list"))
		res = vg_list(state, r);

	else if (!strcmp(rq, "set_global_info"))
		res = set_global_info(state, r);

	else if (!strcmp(rq, "get_global_info"))
		res = get_global_info(state, r);

	else if (!strcmp(rq, "set_vg_info"))
		res = set_vg_info(state, r);

	else if (!strcmp(rq, "dump"))
		res = dump(state);

	else
		res = reply_fail("request not implemented");

	if (cache_lock)
		pthread_rwlock_unlock(&state->cache_lock);
	if (info_lock)
		pthread_mutex_unlock(&state->info_lock);

	return res;
}

static int init(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	ls->log = s->log;

	pthread_mutex_init(&ls->token_lock, NULL);
	pthread_mutex_init(&ls->info_lock, NULL);
	pthread_rwlock_init(&ls->cache_lock, NULL);
	create_metadata_hashes(ls);

	ls->token[0] = 0;

	/* Set up stderr logging depending on the -l option. */
	if (!daemon_log_parse(ls->log, DAEMON_LOG_OUTLET_STDERR, ls->log_config, 1))
		return 0;

	DEBUGLOG(s, "initialised state: vgid_to_metadata = %p", ls->vgid_to_metadata);
	if (!ls->pvid_to_vgid || !ls->vgid_to_metadata)
		return 0;

	/* if (ls->initial_registrations)
	   _process_initial_registrations(ds->initial_registrations); */

	if (ls->idle)
		ls->idle->is_idle = 1;

	return 1;
}

static int fini(daemon_state *s)
{
	lvmetad_state *ls = s->private;

	DEBUGLOG(s, "fini");
	destroy_metadata_hashes(ls);
	return 1;
}

static int process_timeout_arg(const char *str, unsigned *max_timeouts)
{
	char *endptr;
	unsigned long l;

	errno = 0;
	l = strtoul(str, &endptr, 10);
	if (errno || *endptr || l >= UINT_MAX)
		return 0;

	*max_timeouts = (unsigned) l;

	return 1;
}

static void usage(const char *prog, FILE *file)
{
	fprintf(file, "Usage:\n"
		"%s [-V] [-h] [-f] [-l level[,level ...]] [-s path] [-t secs]\n\n"
		"   -V       Show version of lvmetad\n"
		"   -h       Show this help information\n"
		"   -f       Don't fork, run in the foreground\n"
		"   -l       Logging message levels (all,fatal,error,warn,info,wire,debug)\n"
		"   -p       Set path to the pidfile\n"
		"   -s       Set path to the socket to listen on\n"
		"   -t       Time to wait in seconds before shutdown on idle (missing or 0 = inifinite)\n\n", prog);
}

int main(int argc, char *argv[])
{
	signed char opt;
	struct timespec timeout;
	daemon_idle di = { .ptimeout = &timeout };
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
	while ((opt = getopt(argc, argv, "?fhVl:p:s:t:")) != EOF) {
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
		case 't':
			if (!process_timeout_arg(optarg, &di.max_timeouts)) {
				fprintf(stderr, "Invalid value of timeout parameter.\n");
				exit(EXIT_FAILURE);
			}
			/* 0 equals to wait indefinitely */
			if (di.max_timeouts)
				s.idle = ls.idle = &di;
			break;
		case 'V':
			printf("lvmetad version: " LVM_VERSION "\n");
			exit(1);
		}
	}

	daemon_start(s);

	return 0;
}
