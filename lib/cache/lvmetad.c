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
#include "filter.h"

static int _using_lvmetad = 0;
static daemon_handle _lvmetad;

void lvmetad_init(void)
{
	const char *socket = getenv("LVM_LVMETAD_SOCKET");
	if (_using_lvmetad) { /* configured by the toolcontext */
		_lvmetad = lvmetad_open(socket ?: DEFAULT_RUN_DIR "/lvmetad.socket");
		if (_lvmetad.socket_fd < 0 || _lvmetad.error) {
			log_warn("WARNING: Failed to connect to lvmetad: %s. Falling back to internal scanning.", strerror(_lvmetad.error));
			_using_lvmetad = 0;
		}
	}
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
		log_very_verbose("Request to %s %s%sin lvmetad did not find object.",
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
	struct device *device;
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

	device = dev_cache_get_by_devt(devt, cmd->filter);
	if (!device && fallback)
		device = dev_cache_get_by_devt(fallback, cmd->filter);

	if (!device) {
		log_error("No device found for PV %s.", pvid_txt);
		return NULL;
	}

	if (!pvid_txt || !id_read_format(&pvid, pvid_txt)) {
		log_error("Missing or ill-formatted PVID for PV: %s.", pvid_txt);
		return NULL;
	}

	if (vgid_txt)
		id_read_format(&vgid, vgid_txt);
	else
		strcpy((char*)&vgid, fmt->orphan_vg_name);

	if (!vgname)
		vgname = fmt->orphan_vg_name;

	if (!(info = lvmcache_add(fmt->labeller, (const char *)&pvid, device,
				  vgname, (const char *)&vgid, 0)))
		return_NULL;

	lvmcache_get_label(info)->sector = label_sector;
	lvmcache_set_device_size(info, devsize);
	lvmcache_del_das(info);
	lvmcache_del_mdas(info);

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

	return info;
}

struct volume_group *lvmetad_vg_lookup(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct volume_group *vg = NULL;
	daemon_reply reply;
	char uuid[64];
	struct format_instance *fid;
	struct format_instance_ctx fic;
	struct dm_config_node *top;
	const char *name;
	const char *fmt_name;
	struct format_type *fmt;
	struct dm_config_node *pvcn;
	struct pv_list *pvl;
	struct lvmcache_info *info;

	if (!_using_lvmetad)
		return NULL;

	if (vgid) {
		if (!id_write_format((const struct id*)vgid, uuid, sizeof(uuid)))
			return_0;
		reply = daemon_send_simple(_lvmetad, "vg_lookup", "uuid = %s", uuid, NULL);
	} else {
		if (!vgname)
			log_error(INTERNAL_ERROR "VG name required (VGID not available)");
		reply = daemon_send_simple(_lvmetad, "vg_lookup", "name = %s", vgname, NULL);
	}

	if (!strcmp(daemon_reply_str(reply, "response", ""), "OK")) {

		top = dm_config_find_node(reply.cft->root, "metadata");
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
				if (!lvmcache_fid_add_mdas_pv(info, fid)) {
					vg = NULL;
					goto_out;	/* FIXME error path */
				}
			} /* else probably missing */
		}

		lvmcache_update_vg(vg, 0);
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
	char *buf = NULL;
	daemon_reply reply;
	struct dm_hash_node *n;
	struct metadata_area *mda;
	char mda_id[128], *num;
	struct pv_list *pvl;
	struct lvmcache_info *info;
	struct _fixup_baton baton;

	if (!vg)
		return 0;

	if (!_using_lvmetad)
		return 1; /* fake it */

	/* TODO. This is not entirely correct, since export_vg_to_buffer
	 * adds trailing nodes to the buffer. We may need to use
	 * export_vg_to_config_tree and format the buffer ourselves. It
	 * does, however, work for now, since the garbage is well
	 * formatted and has no conflicting keys with the rest of the
	 * request.  */
	if (!export_vg_to_buffer(vg, &buf)) {
		log_error("Could not format VG metadata.");
		return 0;
	}

	reply = daemon_send_simple(_lvmetad, "vg_update", "vgname = %s", vg->name,
				   "metadata = %b", strchr(buf, '{'), NULL);
	dm_free(buf);

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
		if (pvl->pv->dev && !lvmetad_pv_found(pvl->pv->id, pvl->pv->dev,
						      vg->fid ? vg->fid->fmt : pvl->pv->fmt,
						      pvl->pv->label_sector, NULL))
			return 0;
	}

	return 1;
}

int lvmetad_vg_remove(struct volume_group *vg)
{
	char uuid[64];
	daemon_reply reply;
	int result;

	if (!_using_lvmetad)
		return 1; /* just fake it */

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	reply = daemon_send_simple(_lvmetad, "vg_remove", "uuid = %s", uuid, NULL);

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

	if (!_using_lvmetad)
		return_0;

	if (!id_write_format(&pvid, uuid, sizeof(uuid)))
		return_0;

	reply = daemon_send_simple(_lvmetad, "pv_lookup", "uuid = %s", uuid, NULL);

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

	if (!_using_lvmetad)
		return_0;

	reply = daemon_send_simple(_lvmetad, "pv_lookup", "device = %d", dev->dev, NULL);

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

	if (!_using_lvmetad)
		return 1;

	reply = daemon_send_simple(_lvmetad, "pv_list", NULL);

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

	if (!_using_lvmetad)
		return 1;

	reply = daemon_send_simple(_lvmetad, "vg_list", NULL);

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

struct _print_mda_baton {
	int i;
	char *buffer;
};

static int _print_mda(struct metadata_area *mda, void *baton)
{
	int result = 0;
	struct _print_mda_baton *b = baton;
	char *buf, *mda_txt;

	if (!mda->ops->mda_export_text) /* do nothing */
		return 1;

	buf = b->buffer;
	mda_txt = mda->ops->mda_export_text(mda);
	if (!dm_asprintf(&b->buffer, "%s mda%i { %s }", b->buffer ?: "", b->i, mda_txt))
		goto_out;
	b->i ++;
	result = 1;
out:
	dm_free(mda_txt);
	dm_free(buf);
	return result;
}

static int _print_da(struct disk_locn *da, void *baton)
{
	struct _print_mda_baton *b;
	char *buf;

	if (!da)
		return 1;

	b = baton;
	buf = b->buffer;
	if (!dm_asprintf(&b->buffer, "%s da%i { offset = %" PRIu64
			 " size = %" PRIu64 " }",
			 b->buffer ?: "", b->i, da->offset, da->size))
	{
		dm_free(buf);
		return_0;
	}
	b->i ++;
	dm_free(buf);

	return 1;
}

static const char *_print_mdas(struct lvmcache_info *info)
{
	struct _print_mda_baton baton = { .i = 0, .buffer = NULL };

	if (!lvmcache_foreach_mda(info, &_print_mda, &baton))
		return NULL;
	baton.i = 0;
	if (!lvmcache_foreach_da(info, &_print_da, &baton))
		return NULL;

	return baton.buffer;
}

int lvmetad_pv_found(struct id pvid, struct device *device, const struct format_type *fmt,
		     uint64_t label_sector, struct volume_group *vg)
{
	char uuid[64];
	daemon_reply reply;
	struct lvmcache_info *info;
	const char *mdas = NULL;
	char *pvmeta;
	char *buf = NULL;
	int result;

	if (!_using_lvmetad)
		return 1;

	if (!id_write_format(&pvid, uuid, sizeof(uuid)))
                return_0;

	/* FIXME A more direct route would be much preferable. */
	if ((info = lvmcache_info_from_pvid((const char *)&pvid, 0)))
		mdas = _print_mdas(info);

	if (!dm_asprintf(&pvmeta,
			 "{ device = %" PRIu64 "\n"
			 "  dev_size = %" PRIu64 "\n"
			 "  format = \"%s\"\n"
			 "  label_sector = %" PRIu64 "\n"
			 "  id = \"%s\"\n"
			 "  %s"
			 "}", device->dev,
			 info ? lvmcache_device_size(info) : 0,
			 fmt->name, label_sector, uuid, mdas ?: "")) {
		dm_free((char *)mdas);
		return_0;
	}

	dm_free((char *)mdas);

	if (vg) {
		/*
		 * TODO. This is not entirely correct, since export_vg_to_buffer
		 * adds trailing garbage to the buffer. We may need to use
		 * export_vg_to_config_tree and format the buffer ourselves. It
		 * does, however, work for now, since the garbage is well
		 * formatted and has no conflicting keys with the rest of the
		 * request.
		 */
		if (!export_vg_to_buffer(vg, &buf)) {
			dm_free(pvmeta);
			return_0;
		}

		reply = daemon_send_simple(_lvmetad,
					   "pv_found",
					   "pvmeta = %b", pvmeta,
					   "vgname = %s", vg->name,
					   "metadata = %b", strchr(buf, '{'),
					   NULL);
	} else {
		/* There are no MDAs on this PV. */
		reply = daemon_send_simple(_lvmetad,
					   "pv_found",
					   "pvmeta = %b", pvmeta,
					   NULL);
	}

	dm_free(pvmeta);

	result = _lvmetad_handle_reply(reply, "update PV", uuid, NULL);
	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone(dev_t device, const char *pv_name)
{
	daemon_reply reply;
	int result;
	int found;

	if (!_using_lvmetad)
		return 1;

	reply = daemon_send_simple(_lvmetad, "pv_gone", "device = %d", device, NULL);

	result = _lvmetad_handle_reply(reply, "drop PV", pv_name, &found);
	/* We don't care whether or not the daemon had the PV cached. */

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone_by_dev(struct device *dev)
{
	return lvmetad_pv_gone(dev->dev, dev_name(dev));
}

int lvmetad_active(void)
{
	return _using_lvmetad;
}

void lvmetad_set_active(int active)
{
	_using_lvmetad = active;
}

/*
 * The following code implements pvscan --cache.
 */

struct _pvscan_lvmetad_baton {
	struct volume_group *vg;
	struct format_instance *fid;
};

static int _pvscan_lvmetad_single(struct metadata_area *mda, void *baton)
{
	struct _pvscan_lvmetad_baton *b = baton;
	struct volume_group *this = mda->ops->vg_read(b->fid, "", mda, 1);

	/* FIXME Also ensure contents match etc. */
	if (!b->vg || this->seqno > b->vg->seqno)
		b->vg = this;
	else if (b->vg)
		release_vg(this);

	return 1;
}

int pvscan_lvmetad_single(struct cmd_context *cmd, struct device *dev)
{
	struct label *label;
	struct lvmcache_info *info;
	struct physical_volume pv;
	struct _pvscan_lvmetad_baton baton;
	/* Create a dummy instance. */
	struct format_instance_ctx fic = { .type = 0 };

	if (!lvmetad_active()) {
		log_error("Cannot proceed since lvmetad is not active.");
		return 0;
	}

	if (!label_read(dev, &label, 0)) {
		log_print("No PV label found on %s.", dev_name(dev));
		if (!lvmetad_pv_gone_by_dev(dev))
			goto_bad;
		return 1;
	}

	info = (struct lvmcache_info *) label->info;
	memset(&pv, 0, sizeof(pv));

	baton.vg = NULL;
	baton.fid = lvmcache_fmt(info)->ops->create_instance(lvmcache_fmt(info),
							     &fic);

	lvmcache_foreach_mda(info, _pvscan_lvmetad_single, &baton);
	if (!baton.vg)
		lvmcache_fmt(info)->ops->destroy_instance(baton.fid);

	/*
	 * NB. If this command failed and we are relying on lvmetad to have an
	 * *exact* image of the system, the lvmetad instance that went out of
	 * sync needs to be killed.
	 */
	if (!lvmetad_pv_found(*(struct id *)dev->pvid, dev, lvmcache_fmt(info),
			      label->sector, baton.vg)) {
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
