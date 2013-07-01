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

#include "lib.h"
#include "toolcontext.h"
#include "metadata.h"
#include "device.h"
#include "lvmetad.h"
#include "lvmcache.h"
#include "lvmetad-client.h"
#include "format-text.h" // TODO for disk_locn, used as a DA representation
#include "crc.h"

static daemon_handle _lvmetad;
static int _lvmetad_use = 0;
static int _lvmetad_connected = 0;

static char *_lvmetad_token = NULL;
static const char *_lvmetad_socket = NULL;
static struct cmd_context *_lvmetad_cmd = NULL;

void lvmetad_disconnect(void)
{
	if (_lvmetad_connected)
		daemon_close(_lvmetad);
	_lvmetad_connected = 0;
	_lvmetad_cmd = NULL;
}

void lvmetad_init(struct cmd_context *cmd)
{
	if (!_lvmetad_use && !access(LVMETAD_PIDFILE, F_OK))
		log_warn("WARNING: lvmetad is running but disabled."
			 " Restart lvmetad before enabling it!");
	_lvmetad_cmd = cmd;
}

static void _lvmetad_connect(void)
{
	if (!_lvmetad_use || !_lvmetad_socket || _lvmetad_connected)
		return;

	_lvmetad = lvmetad_open(_lvmetad_socket);
	if (_lvmetad.socket_fd >= 0 && !_lvmetad.error) {
		log_debug_lvmetad("Successfully connected to lvmetad on fd %d.",
				  _lvmetad.socket_fd);
		_lvmetad_connected = 1;
	}
}

void lvmetad_connect_or_warn(void)
{
	if (!_lvmetad_use)
		return;

	if (!_lvmetad_connected)
		_lvmetad_connect();

	if ((_lvmetad.socket_fd < 0 || _lvmetad.error))
		log_warn("WARNING: Failed to connect to lvmetad: %s. Falling back to internal scanning.",
			 strerror(_lvmetad.error));
}

int lvmetad_active(void)
{
	if (!_lvmetad_use)
		return 0;

	if (!_lvmetad_connected)
		_lvmetad_connect();

	if ((_lvmetad.socket_fd < 0 || _lvmetad.error))
		log_debug_lvmetad("Failed to connect to lvmetad: %s.", strerror(_lvmetad.error));

	return _lvmetad_connected;
}

void lvmetad_set_active(int active)
{
	_lvmetad_use = active;
}

/*
 * Use a crc of the strings in the filter as the lvmetad token.
 */
void lvmetad_set_token(const struct dm_config_value *filter)
{
	int ft = 0;

	if (_lvmetad_token)
		dm_free(_lvmetad_token);

	while (filter && filter->type == DM_CFG_STRING) {
		ft = calc_crc(ft, (const uint8_t *) filter->v.str, strlen(filter->v.str));
		filter = filter->next;
	}

	if (!dm_asprintf(&_lvmetad_token, "filter:%u", ft))
		log_warn("WARNING: Failed to set lvmetad token. Out of memory?");
}

void lvmetad_release_token(void)
{
	dm_free(_lvmetad_token);
	_lvmetad_token = NULL;
}

void lvmetad_set_socket(const char *sock)
{
	_lvmetad_socket = sock;
}

static daemon_reply _lvmetad_send(const char *id, ...)
{
	va_list ap;
	daemon_reply repl;
	daemon_request req;
	int try = 0;

retry:
	req = daemon_request_make(id);

	if (_lvmetad_token)
		daemon_request_extend(req, "token = %s", _lvmetad_token, NULL);

	va_start(ap, id);
	daemon_request_extend_v(req, ap);
	va_end(ap);

	repl = daemon_send(_lvmetad, req);

	daemon_request_destroy(req);

	if (!repl.error && !strcmp(daemon_reply_str(repl, "response", ""), "token_mismatch") &&
	    try < 2 && !test_mode()) {
		if (lvmetad_pvscan_all_devs(_lvmetad_cmd, NULL)) {
			++ try;
			daemon_reply_destroy(repl);
			goto retry;
		}
	}

	return repl;
}

static int _token_update(void)
{
	daemon_reply repl;

	log_debug_lvmetad("Sending updated token to lvmetad: %s", _lvmetad_token ? : "<NONE>");
	repl = _lvmetad_send("token_update", NULL);

	if (repl.error || strcmp(daemon_reply_str(repl, "response", ""), "OK")) {
		daemon_reply_destroy(repl);
		return 0;
	}

	daemon_reply_destroy(repl);
	return 1;
}

/*
 * Helper; evaluate the reply from lvmetad, check for errors, print diagnostics
 * and return a summary success/failure exit code.
 *
 * If found is set, *found indicates whether or not device exists,
 * and missing device is not treated as an error.
 */
static int _lvmetad_handle_reply(daemon_reply reply, const char *action, const char *object,
				 int *found)
{
	if (reply.error) {
		log_error("Request to %s %s%sin lvmetad gave response %s.",
			  action, object, *object ? " " : "", strerror(reply.error));
		return 0;
	}

	/* All OK? */
	if (!strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		if (found)
			*found = 1;
		return 1;
	}

	/* Unknown device permitted? */
	if (found && !strcmp(daemon_reply_str(reply, "response", ""), "unknown")) {
		log_very_verbose("Request to %s %s%sin lvmetad did not find any matching object.",
				 action, object, *object ? " " : "");
		*found = 0;
		return 1;
	}

	log_error("Request to %s %s%sin lvmetad gave response %s. Reason: %s",
		  action, object, *object ? " " : "", 
		  daemon_reply_str(reply, "response", "<missing>"),
		  daemon_reply_str(reply, "reason", "<missing>"));

	return 0;
}

static int _read_mda(struct lvmcache_info *info,
		     struct format_type *fmt,
		     const struct dm_config_node *cn)
{
	struct metadata_area_ops *ops;

	dm_list_iterate_items(ops, &fmt->mda_ops)
		if (ops->mda_import_text && ops->mda_import_text(info, cn))
			return 1;

	return 0;
}

static struct lvmcache_info *_pv_populate_lvmcache(
	struct cmd_context *cmd, struct dm_config_node *cn, dev_t fallback)
{
	struct device *dev;
	struct id pvid, vgid;
	char mda_id[32];
	char da_id[32];
	int i = 0;
	struct dm_config_node *mda = NULL;
	struct dm_config_node *da = NULL;
	uint64_t offset, size;
	struct lvmcache_info *info;
	const char *pvid_txt = dm_config_find_str(cn->child, "id", NULL),
		   *vgid_txt = dm_config_find_str(cn->child, "vgid", NULL),
		   *vgname = dm_config_find_str(cn->child, "vgname", NULL),
		   *fmt_name = dm_config_find_str(cn->child, "format", NULL);
	dev_t devt = dm_config_find_int(cn->child, "device", 0);
	uint64_t devsize = dm_config_find_int64(cn->child, "dev_size", 0),
		 label_sector = dm_config_find_int64(cn->child, "label_sector", 0);

	struct format_type *fmt = fmt_name ? get_format_by_name(cmd, fmt_name) : NULL;

	if (!fmt) {
		log_error("PV %s not recognised. Is the device missing?", pvid_txt);
		return NULL;
	}

	dev = dev_cache_get_by_devt(devt, cmd->filter);
	if (!dev && fallback)
		dev = dev_cache_get_by_devt(fallback, cmd->filter);

	if (!dev) {
		log_error("No device found for PV %s.", pvid_txt);
		return NULL;
	}

	if (!pvid_txt || !id_read_format(&pvid, pvid_txt)) {
		log_error("Missing or ill-formatted PVID for PV: %s.", pvid_txt);
		return NULL;
	}

	if (vgid_txt) {
		if (!id_read_format(&vgid, vgid_txt))
			return_NULL;
	} else
		strcpy((char*)&vgid, fmt->orphan_vg_name);

	if (!vgname)
		vgname = fmt->orphan_vg_name;

	if (!(info = lvmcache_add(fmt->labeller, (const char *)&pvid, dev,
				  vgname, (const char *)&vgid, 0)))
		return_NULL;

	lvmcache_get_label(info)->sector = label_sector;
	lvmcache_set_device_size(info, devsize);
	lvmcache_del_das(info);
	lvmcache_del_mdas(info);
	lvmcache_del_bas(info);

	do {
		sprintf(mda_id, "mda%d", i);
		mda = dm_config_find_node(cn->child, mda_id);
		if (mda)
			_read_mda(info, fmt, mda);
		++i;
	} while (mda);

	i = 0;
	do {
		sprintf(da_id, "da%d", i);
		da = dm_config_find_node(cn->child, da_id);
		if (da) {
			if (!dm_config_get_uint64(da->child, "offset", &offset)) return_0;
			if (!dm_config_get_uint64(da->child, "size", &size)) return_0;
			lvmcache_add_da(info, offset, size);
		}
		++i;
	} while (da);

	i = 0;
	do {
		sprintf(da_id, "ea%d", i);
		da = dm_config_find_node(cn->child, da_id);
		if (da) {
			if (!dm_config_get_uint64(da->child, "offset", &offset)) return_0;
			if (!dm_config_get_uint64(da->child, "size", &size)) return_0;
			lvmcache_add_ba(info, offset, size);
		}
		++i;
	} while (da);

	return info;
}

struct volume_group *lvmetad_vg_lookup(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct volume_group *vg = NULL;
	daemon_reply reply;
	int found;
	char uuid[64];
	struct format_instance *fid;
	struct format_instance_ctx fic;
	struct dm_config_node *top;
	const char *name, *diag_name;
	const char *fmt_name;
	struct format_type *fmt;
	struct dm_config_node *pvcn;
	struct pv_list *pvl;
	struct lvmcache_info *info;

	if (!lvmetad_active())
		return NULL;

	if (vgid) {
		if (!id_write_format((const struct id*)vgid, uuid, sizeof(uuid)))
			return_NULL;
		log_debug_lvmetad("Asking lvmetad for VG %s (%s)", uuid, vgname ? : "name unknown");
		reply = _lvmetad_send("vg_lookup", "uuid = %s", uuid, NULL);
		diag_name = uuid;
	} else {
		if (!vgname) {
			log_error(INTERNAL_ERROR "VG name required (VGID not available)");
			reply = _lvmetad_send("vg_lookup", "name = %s", "MISSING", NULL);
			goto out;
		}
		log_debug_lvmetad("Asking lvmetad for VG %s", vgname);
		reply = _lvmetad_send("vg_lookup", "name = %s", vgname, NULL);
		diag_name = vgname;
	}

	if (_lvmetad_handle_reply(reply, "lookup VG", diag_name, &found) && found) {

		if (!(top = dm_config_find_node(reply.cft->root, "metadata"))) {
			log_error(INTERNAL_ERROR "metadata config node not found.");
			goto out;
		}

		name = daemon_reply_str(reply, "name", NULL);

		/* fall back to lvm2 if we don't know better */
		fmt_name = dm_config_find_str(top, "metadata/format", "lvm2");
		if (!(fmt = get_format_by_name(cmd, fmt_name))) {
			log_error(INTERNAL_ERROR
				  "We do not know the format (%s) reported by lvmetad.",
				  fmt_name);
			goto out;
		}

		fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
		fic.context.vg_ref.vg_name = name;
		fic.context.vg_ref.vg_id = vgid;

		if (!(fid = fmt->ops->create_instance(fmt, &fic)))
			goto_out;

		if ((pvcn = dm_config_find_node(top, "metadata/physical_volumes")))
			for (pvcn = pvcn->child; pvcn; pvcn = pvcn->sib)
				_pv_populate_lvmcache(cmd, pvcn, 0);

		top->key = name;
		if (!(vg = import_vg_from_config_tree(reply.cft, fid)))
			goto_out;

		dm_list_iterate_items(pvl, &vg->pvs) {
			if ((info = lvmcache_info_from_pvid((const char *)&pvl->pv->id, 0))) {
				pvl->pv->label_sector = lvmcache_get_label(info)->sector;
				pvl->pv->dev = lvmcache_device(info);
				if (!pvl->pv->dev)
					pvl->pv->status |= MISSING_PV;
				else
					check_reappeared_pv(vg, pvl->pv);
				if (!lvmcache_fid_add_mdas_pv(info, fid)) {
					vg = NULL;
					goto_out;	/* FIXME error path */
				}
			} else
				pvl->pv->status |= MISSING_PV; /* probably missing */
		}

		lvmcache_update_vg(vg, 0);
		vg_mark_partial_lvs(vg, 1);
	}

out:
	daemon_reply_destroy(reply);

	return vg;
}

struct _fixup_baton {
	int i;
	int find;
	int ignore;
};

static int _fixup_ignored(struct metadata_area *mda, void *baton) {
	struct _fixup_baton *b = baton;
	if (b->i == b->find)
		mda_set_ignored(mda, b->ignore);
	b->i ++;
	return 1;
}

int lvmetad_vg_update(struct volume_group *vg)
{
	daemon_reply reply;
	struct dm_hash_node *n;
	struct metadata_area *mda;
	char mda_id[128], *num;
	struct pv_list *pvl;
	struct lvmcache_info *info;
	struct _fixup_baton baton;
	struct dm_config_tree *vgmeta;

	if (!vg)
		return 0;

	if (!lvmetad_active() || test_mode())
		return 1; /* fake it */

	if (!(vgmeta = export_vg_to_config_tree(vg)))
		return_0;

	log_debug_lvmetad("Sending lvmetad updated metadata for VG %s (seqno %" PRIu32 ")", vg->name, vg->seqno);
	reply = _lvmetad_send("vg_update", "vgname = %s", vg->name,
			      "metadata = %t", vgmeta, NULL);
	dm_config_destroy(vgmeta);

	if (!_lvmetad_handle_reply(reply, "update VG", vg->name, NULL)) {
		daemon_reply_destroy(reply);
		return 0;
	}

	daemon_reply_destroy(reply);

	n = (vg->fid && vg->fid->metadata_areas_index) ?
		dm_hash_get_first(vg->fid->metadata_areas_index) : NULL;
	while (n) {
		mda = dm_hash_get_data(vg->fid->metadata_areas_index, n);
		strcpy(mda_id, dm_hash_get_key(vg->fid->metadata_areas_index, n));
		if ((num = strchr(mda_id, '_'))) {
			*num = 0;
			++num;
			if ((info = lvmcache_info_from_pvid(mda_id, 0))) {
				memset(&baton, 0, sizeof(baton));
				baton.find = atoi(num);
				baton.ignore = mda_is_ignored(mda);
				lvmcache_foreach_mda(info, _fixup_ignored, &baton);
			}
		}
		n = dm_hash_get_next(vg->fid->metadata_areas_index, n);
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		/* NB. the PV fmt pointer is sometimes wrong during vgconvert */
		if (pvl->pv->dev && !lvmetad_pv_found(&pvl->pv->id, pvl->pv->dev,
						      vg->fid ? vg->fid->fmt : pvl->pv->fmt,
						      pvl->pv->label_sector, NULL, NULL))
			return 0;
	}

	return 1;
}

int lvmetad_vg_remove(struct volume_group *vg)
{
	char uuid[64];
	daemon_reply reply;
	int result;

	if (!lvmetad_active() || test_mode())
		return 1; /* just fake it */

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Telling lvmetad to remove VGID %s (%s)", uuid, vg->name);
	reply = _lvmetad_send("vg_remove", "uuid = %s", uuid, NULL);
	result = _lvmetad_handle_reply(reply, "remove VG", vg->name, NULL);

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_lookup(struct cmd_context *cmd, struct id pvid, int *found)
{
	char uuid[64];
	daemon_reply reply;
	int result = 0;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return_0;

	if (!id_write_format(&pvid, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Asking lvmetad for PV %s", uuid);
	reply = _lvmetad_send("pv_lookup", "uuid = %s", uuid, NULL);
	if (!_lvmetad_handle_reply(reply, "lookup PV", "", found))
		goto_out;

	if (found && !*found)
		goto out_success;

	if (!(cn = dm_config_find_node(reply.cft->root, "physical_volume")))
		goto_out;
        else if (!_pv_populate_lvmcache(cmd, cn, 0))
		goto_out;

out_success:
	result = 1;

out:
	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_lookup_by_dev(struct cmd_context *cmd, struct device *dev, int *found)
{
	int result = 0;
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return_0;

	log_debug_lvmetad("Asking lvmetad for PV on %s", dev_name(dev));
	reply = _lvmetad_send("pv_lookup", "device = %" PRId64, (int64_t) dev->dev, NULL);
	if (!_lvmetad_handle_reply(reply, "lookup PV", dev_name(dev), found))
		goto_out;

	if (found && !*found)
		goto out_success;

	cn = dm_config_find_node(reply.cft->root, "physical_volume");
	if (!cn || !_pv_populate_lvmcache(cmd, cn, dev->dev))
		goto_out;

out_success:
	result = 1;

out:
	daemon_reply_destroy(reply);
	return result;
}

int lvmetad_pv_list_to_lvmcache(struct cmd_context *cmd)
{
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known PVs");
	reply = _lvmetad_send("pv_list", NULL);
	if (!_lvmetad_handle_reply(reply, "list PVs", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "physical_volumes")))
		for (cn = cn->child; cn; cn = cn->sib)
			_pv_populate_lvmcache(cmd, cn, 0);

	daemon_reply_destroy(reply);

	return 1;
}

int lvmetad_vg_list_to_lvmcache(struct cmd_context *cmd)
{
	struct volume_group *tmp;
	struct id vgid;
	const char *vgid_txt;
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_active())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known VGs");
	reply = _lvmetad_send("vg_list", NULL);
	if (!_lvmetad_handle_reply(reply, "list VGs", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "volume_groups")))
		for (cn = cn->child; cn; cn = cn->sib) {
			vgid_txt = cn->key;
			if (!id_read_format(&vgid, vgid_txt)) {
				stack;
				continue;
			}

			/* the call to lvmetad_vg_lookup will poke the VG into lvmcache */
			tmp = lvmetad_vg_lookup(cmd, NULL, (const char*)&vgid);
			release_vg(tmp);
		}

	daemon_reply_destroy(reply);
	return 1;
}

struct _extract_dl_baton {
	int i;
	struct dm_config_tree *cft;
	struct dm_config_node *pre_sib;
};

static int _extract_mda(struct metadata_area *mda, void *baton)
{
	struct _extract_dl_baton *b = baton;
	struct dm_config_node *cn;
	char id[32];

	if (!mda->ops->mda_export_text) /* do nothing */
		return 1;

	(void) dm_snprintf(id, 32, "mda%d", b->i);
	if (!(cn = make_config_node(b->cft, id, b->cft->root, b->pre_sib)))
		return 0;
	if (!mda->ops->mda_export_text(mda, b->cft, cn))
		return 0;

	b->i ++;
	b->pre_sib = cn; /* for efficiency */

	return 1;
}

static int _extract_disk_location(const char *name, struct disk_locn *dl, void *baton)
{
	struct _extract_dl_baton *b = baton;
	struct dm_config_node *cn;
	char id[32];

	if (!dl)
		return 1;

	(void) dm_snprintf(id, 32, "%s%d", name, b->i);
	if (!(cn = make_config_node(b->cft, id, b->cft->root, b->pre_sib)))
		return 0;
	if (!config_make_nodes(b->cft, cn, NULL,
			       "offset = %"PRId64, (int64_t) dl->offset,
			       "size = %"PRId64, (int64_t) dl->size,
			       NULL))
		return 0;

	b->i ++;
	b->pre_sib = cn; /* for efficiency */

	return 1;
}

static int _extract_da(struct disk_locn *da, void *baton)
{
	return _extract_disk_location("da", da, baton);
}

static int _extract_ba(struct disk_locn *ba, void *baton)
{
	return _extract_disk_location("ba", ba, baton);
}

static int _extract_mdas(struct lvmcache_info *info, struct dm_config_tree *cft,
			 struct dm_config_node *pre_sib)
{
	struct _extract_dl_baton baton = { .i = 0, .cft = cft, .pre_sib = NULL };

	if (!lvmcache_foreach_mda(info, &_extract_mda, &baton))
		return 0;
	baton.i = 0;
	if (!lvmcache_foreach_da(info, &_extract_da, &baton))
		return 0;
	baton.i = 0;
	if (!lvmcache_foreach_ba(info, &_extract_ba, &baton))
		return 0;

	return 1;
}

int lvmetad_pv_found(const struct id *pvid, struct device *dev, const struct format_type *fmt,
		     uint64_t label_sector, struct volume_group *vg, activation_handler handler)
{
	char uuid[64];
	daemon_reply reply;
	struct lvmcache_info *info;
	struct dm_config_tree *pvmeta, *vgmeta;
	const char *status, *vgid;
	int result;

	if (!lvmetad_active() || test_mode())
		return 1;

	if (!id_write_format(pvid, uuid, sizeof(uuid)))
                return_0;

	pvmeta = dm_config_create();
	if (!pvmeta)
		return_0;

	info = lvmcache_info_from_pvid((const char *)pvid, 0);

	if (!(pvmeta->root = make_config_node(pvmeta, "pv", NULL, NULL))) {
		dm_config_destroy(pvmeta);
		return_0;
	}

	if (!config_make_nodes(pvmeta, pvmeta->root, NULL,
			       "device = %"PRId64, (int64_t) dev->dev,
			       "dev_size = %"PRId64, (int64_t) (info ? lvmcache_device_size(info) : 0),
			       "format = %s", fmt->name,
			       "label_sector = %"PRId64, (int64_t) label_sector,
			       "id = %s", uuid,
			       NULL))
	{
		dm_config_destroy(pvmeta);
		return_0;
	}

	if (info)
		/* FIXME A more direct route would be much preferable. */
		_extract_mdas(info, pvmeta, pvmeta->root);

	if (vg) {
		if (!(vgmeta = export_vg_to_config_tree(vg))) {
			dm_config_destroy(pvmeta);
			return_0;
		}

		log_debug_lvmetad("Telling lvmetad to store PV %s (%s) in VG %s", dev_name(dev), uuid, vg->name);
		reply = _lvmetad_send("pv_found",
				      "pvmeta = %t", pvmeta,
				      "vgname = %s", vg->name,
				      "metadata = %t", vgmeta,
				      NULL);
		dm_config_destroy(vgmeta);
	} else {
		/*
		 * There is no VG metadata stored on this PV.
		 * It might or might not be an orphan.
		 */
		log_debug_lvmetad("Telling lvmetad to store PV %s (%s)", dev_name(dev), uuid);
		reply = _lvmetad_send("pv_found", "pvmeta = %t", pvmeta, NULL);
	}

	dm_config_destroy(pvmeta);

	result = _lvmetad_handle_reply(reply, "update PV", uuid, NULL);

	if (vg && result &&
	    (daemon_reply_int(reply, "seqno_after", -1) != vg->seqno ||
	     daemon_reply_int(reply, "seqno_after", -1) != daemon_reply_int(reply, "seqno_before", -1)))
		log_warn("WARNING: Inconsistent metadata found for VG %s", vg->name);

	if (result && handler) {
		status = daemon_reply_str(reply, "status", "<missing>");
		vgid = daemon_reply_str(reply, "vgid", "<missing>");
		if (!strcmp(status, "partial"))
			handler(_lvmetad_cmd, vgid, 1, CHANGE_AAY);
		else if (!strcmp(status, "complete"))
			handler(_lvmetad_cmd, vgid, 0, CHANGE_AAY);
		else if (!strcmp(status, "orphan"))
			;
		else
			log_error("Request to %s %s in lvmetad gave status %s.",
			  "update PV", uuid, status);
	}

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone(dev_t devno, const char *pv_name, activation_handler handler)
{
	daemon_reply reply;
	int result;
	int found;

	if (!lvmetad_active() || test_mode())
		return 1;

	/*
         *  TODO: automatic volume deactivation takes place here *before*
         *        all cached info is gone - call handler. Also, consider
         *        integrating existing deactivation script  that deactivates
         *        the whole stack from top to bottom (not yet upstream).
         */

	log_debug_lvmetad("Telling lvmetad to forget any PV on %s", pv_name);
	reply = _lvmetad_send("pv_gone", "device = %" PRId64, (int64_t) devno, NULL);

	result = _lvmetad_handle_reply(reply, "drop PV", pv_name, &found);
	/* We don't care whether or not the daemon had the PV cached. */

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone_by_dev(struct device *dev, activation_handler handler)
{
	return lvmetad_pv_gone(dev->dev, dev_name(dev), handler);
}

/*
 * The following code implements pvscan --cache.
 */

struct _lvmetad_pvscan_baton {
	struct volume_group *vg;
	struct format_instance *fid;
};

static int _lvmetad_pvscan_single(struct metadata_area *mda, void *baton)
{
	struct _lvmetad_pvscan_baton *b = baton;
	struct volume_group *this = mda->ops->vg_read(b->fid, "", mda, 1);

	/* FIXME Also ensure contents match etc. */
	if (!b->vg || this->seqno > b->vg->seqno)
		b->vg = this;
	else if (b->vg)
		release_vg(this);

	return 1;
}

int lvmetad_pvscan_single(struct cmd_context *cmd, struct device *dev,
			  activation_handler handler)
{
	struct label *label;
	struct lvmcache_info *info;
	struct _lvmetad_pvscan_baton baton;
	/* Create a dummy instance. */
	struct format_instance_ctx fic = { .type = 0 };

	if (!lvmetad_active()) {
		log_error("Cannot proceed since lvmetad is not active.");
		return 0;
	}

	if (!label_read(dev, &label, 0)) {
		log_print_unless_silent("No PV label found on %s.", dev_name(dev));
		if (!lvmetad_pv_gone_by_dev(dev, handler))
			goto_bad;
		return 1;
	}

	info = (struct lvmcache_info *) label->info;

	baton.vg = NULL;
	baton.fid = lvmcache_fmt(info)->ops->create_instance(lvmcache_fmt(info),
							     &fic);

	if (!baton.fid)
		goto_bad;

	lvmcache_foreach_mda(info, _lvmetad_pvscan_single, &baton);

	/* LVM1 VGs have no MDAs. */
	if (!baton.vg && lvmcache_fmt(info) == get_format_by_name(cmd, "lvm1"))
		baton.vg = ((struct metadata_area *) dm_list_first(&baton.fid->metadata_areas_in_use))->
			ops->vg_read(baton.fid, lvmcache_vgname_from_info(info), NULL, 0);

	if (!baton.vg)
		lvmcache_fmt(info)->ops->destroy_instance(baton.fid);

	/*
	 * NB. If this command failed and we are relying on lvmetad to have an
	 * *exact* image of the system, the lvmetad instance that went out of
	 * sync needs to be killed.
	 */
	if (!lvmetad_pv_found((const struct id *) &dev->pvid, dev, lvmcache_fmt(info),
			      label->sector, baton.vg, handler)) {
		release_vg(baton.vg);
		goto_bad;
	}

	release_vg(baton.vg);
	return 1;

bad:
	/* FIXME kill lvmetad automatically if we can */
	log_error("Update of lvmetad failed. This is a serious problem.\n  "
		  "It is strongly recommended that you restart lvmetad immediately.");
	return 0;
}

int lvmetad_pvscan_all_devs(struct cmd_context *cmd, activation_handler handler)
{
	struct dev_iter *iter;
	struct device *dev;
	daemon_reply reply;
	int r = 1;
	char *future_token;
	int was_silent;

	if (!lvmetad_active()) {
		log_error("Cannot proceed since lvmetad is not active.");
		return 0;
	}

	if (!(iter = dev_iter_create(cmd->lvmetad_filter, 1))) {
		log_error("dev_iter creation failed");
		return 0;
	}

	future_token = _lvmetad_token;
	_lvmetad_token = (char *) "update in progress";
	if (!_token_update()) {
		dev_iter_destroy(iter);
		_lvmetad_token = future_token;
		return 0;
	}

	log_debug_lvmetad("Telling lvmetad to clear its cache");
	reply = _lvmetad_send("pv_clear_all", NULL);
	if (!_lvmetad_handle_reply(reply, "clear info about all PVs", "", NULL))
		r = 0;
	daemon_reply_destroy(reply);

	was_silent = silent_mode();
	init_silent(1);

	while ((dev = dev_iter_get(iter))) {
		if (sigint_caught()) {
			r = 0;
			stack;
			break;
		}
		if (!lvmetad_pvscan_single(cmd, dev, handler))
			r = 0;
	}

	init_silent(was_silent);

	dev_iter_destroy(iter);

	_lvmetad_token = future_token;
	if (!_token_update())
		return 0;

	return r;
}

