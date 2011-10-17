/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
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
#include "segtype.h"
#include "text_export.h"
#include "text_import.h"
#include "config.h"
#include "activate.h"
#include "str_list.h"

#ifdef DMEVENTD
#  include "sharedlib.h"
#  include "libdevmapper-event.h"
#endif

/* Dm kernel module name for thin provisiong */
#define THIN_MODULE "thin-pool"

/*
 * Macro used as return argument - returns 0.
 * return is left to be written in the function for better readability.
 */
#define SEG_LOG_ERROR(t, p...) \
	log_error(t " segment %s of logical volume %s.", ## p, \
		  dm_config_parent_name(sn), seg->lv->name), 0;

static const char *_thin_pool_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static int _thin_pool_add_message(struct lv_segment *seg,
				  const char *key,
				  const struct dm_config_node *sn)
{
	const char *lv_name = NULL;
	struct logical_volume *lv = NULL;
	uint32_t device_id = 0;
	dm_thin_message_t type;

	/* Message must have only one from: create, trim, delete */
	if (dm_config_get_str(sn, "create", &lv_name)) {
		if (!(lv = find_lv(seg->lv->vg, lv_name)))
			return SEG_LOG_ERROR("Unknown LV %s for create message in",
					     lv_name);
		/* FIXME: switch to _SNAP later, if the created LV has an origin */
		type = DM_THIN_MESSAGE_CREATE_THIN;
	}

	if (dm_config_get_str(sn, "trim", &lv_name)) {
		if (lv)
			return SEG_LOG_ERROR("Unsupported message format in");
		if (!(lv = find_lv(seg->lv->vg, lv_name)))
			return SEG_LOG_ERROR("Unknown LV %s for trim message in",
					     lv_name);
		type = DM_THIN_MESSAGE_TRIM;
	}

	if (!dm_config_get_uint32(sn, "delete", &device_id)) {
		if (!lv)
			return SEG_LOG_ERROR("Unknown message in");
	} else {
		if (lv)
			return SEG_LOG_ERROR("Unsupported message format in");
		type = DM_THIN_MESSAGE_DELETE;
	}

	if (!attach_pool_message(seg, type, lv, device_id, 1))
		return_0;

	return 1;
}

static int _thin_pool_text_import(struct lv_segment *seg,
				  const struct dm_config_node *sn,
				  struct dm_hash_table *pv_hash __attribute__((unused)))
{
	const char *lv_name;
	struct logical_volume *pool_data_lv, *pool_metadata_lv;

	if (!dm_config_get_str(sn, "pool", &lv_name))
		return SEG_LOG_ERROR("Pool must be a string in");

	if (!(pool_data_lv = find_lv(seg->lv->vg, lv_name)))
		return SEG_LOG_ERROR("Unknown pool %s in", lv_name);

	if (!dm_config_get_str(sn, "metadata", &lv_name))
		return SEG_LOG_ERROR("Metadata must be a string in");

	if (!(pool_metadata_lv = find_lv(seg->lv->vg, lv_name)))
		return SEG_LOG_ERROR("Unknown metadata %s in", lv_name);

	if (!attach_pool_data_lv(seg, pool_data_lv))
		return_0;

	if (!attach_pool_metadata_lv(seg, pool_metadata_lv))
		return_0;

	if (!dm_config_get_uint64(sn, "transaction_id", &seg->transaction_id))
		return SEG_LOG_ERROR("Could not read transaction_id for");

	if (!dm_config_get_uint64(sn, "low_water_mark", &seg->low_water_mark))
		return SEG_LOG_ERROR("Could not read low_water_mark");

	if (!dm_config_get_uint32(sn, "data_block_size", &seg->data_block_size))
		return SEG_LOG_ERROR("Could not read data_block_size");

	if ((seg->data_block_size < DM_THIN_MIN_DATA_SIZE) ||
	    (seg->data_block_size > DM_THIN_MAX_DATA_SIZE))
		return SEG_LOG_ERROR("Unsupported value %u for data_block_size",
				     seg->device_id);

	if (dm_config_has_node(sn, "zero_new_blocks") &&
	    !dm_config_get_uint32(sn, "zero_new_blocks", &seg->zero_new_blocks))
		return SEG_LOG_ERROR("Could not read zero_new_blocks for");

	seg->lv->status |= THIN_POOL;

	/* Read messages */
	for (; sn; sn = sn->sib)
		if (!(sn->v) && !_thin_pool_add_message(seg, sn->key, sn->child))
			return_0;

	return 1;
}

static int _thin_pool_text_import_area_count(const struct dm_config_node *sn,
					     uint32_t *area_count)
{
	*area_count = 1;

	return 1;
}

static int _thin_pool_text_export(const struct lv_segment *seg, struct formatter *f)
{
	unsigned cnt = 0;
	struct lv_thin_message *tmsg;

	outf(f, "pool = \"%s\"", seg_lv(seg, 0)->name);
	outf(f, "metadata = \"%s\"", seg->pool_metadata_lv->name);
	outf(f, "transaction_id = %" PRIu64, seg->transaction_id);
	outf(f, "low_water_mark = %" PRIu64, seg->low_water_mark);
	outf(f, "data_block_size = %d", seg->data_block_size);

	if (seg->zero_new_blocks)
		outf(f, "zero_new_blocks = 1");

	dm_list_iterate_items(tmsg, &seg->thin_messages) {
		/* Extra validation */
		switch (tmsg->type) {
		case DM_THIN_MESSAGE_CREATE_SNAP:
		case DM_THIN_MESSAGE_CREATE_THIN:
		case DM_THIN_MESSAGE_TRIM:
			if (!lv_is_thin_volume(tmsg->u.lv)) {
				log_error(INTERNAL_ERROR
					  "LV %s is not a thin volume.",
					  tmsg->u.lv->name);
				return 0;
			}
			break;
		default:
			break;
		}

		if (!cnt)
			outnl(f);

		outf(f, "message%d {", ++cnt);
		out_inc_indent(f);

		switch (tmsg->type) {
		case DM_THIN_MESSAGE_CREATE_SNAP:
		case DM_THIN_MESSAGE_CREATE_THIN:
			outf(f, "create = \"%s\"", tmsg->u.lv->name);
			break;
		case DM_THIN_MESSAGE_TRIM:
			outf(f, "trim = \"%s\"", tmsg->u.lv->name);
			break;
		case DM_THIN_MESSAGE_DELETE:
			outf(f, "delete = %d", tmsg->u.delete_id);
			break;
		default:
			log_error(INTERNAL_ERROR "Passed unsupported message.");
			return 0;
		}

		out_dec_indent(f);
		outf(f, "}");
	}

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _thin_pool_add_target_line(struct dev_manager *dm,
				      struct dm_pool *mem __attribute__((unused)),
				      struct cmd_context *cmd __attribute__((unused)),
				      void **target_state __attribute__((unused)),
				      struct lv_segment *seg,
				      const struct lv_activate_opts *laopts __attribute__((unused)),
				      struct dm_tree_node *node, uint64_t len,
				      uint32_t *pvmove_mirror_count __attribute__((unused)))
{
	char *metadata_dlid, *pool_dlid;
	struct lv_thin_message *lmsg;
	struct dm_thin_message dmsg;

	if (!(metadata_dlid = build_dm_uuid(mem, seg->pool_metadata_lv->lvid.s, NULL))) {
		log_error("Failed to build uuid for metadata LV %s.",
			  seg->pool_metadata_lv->name);
		return 0;
	}

	if (!(pool_dlid = build_dm_uuid(mem, seg_lv(seg, 0)->lvid.s, NULL))) {
		log_error("Failed to build uuid for pool LV %s.",
			  seg_lv(seg, 0)->name);
		return 0;
	}

	if (!dm_tree_node_add_thin_pool_target(node, len, seg->transaction_id,
					       metadata_dlid, pool_dlid,
					       seg->data_block_size, seg->low_water_mark,
					       seg->zero_new_blocks ? 0 : 1))
		return_0;

	if (!dm_list_empty(&seg->thin_messages)) {
		dm_list_iterate_items(lmsg, &seg->thin_messages) {
			dmsg.type = lmsg->type;
			switch (lmsg->type) {
			case DM_THIN_MESSAGE_CREATE_SNAP:
				/* FIXME: to be implemented */
				log_debug("Thin pool create_snap %s.", lmsg->u.lv->name);
				dmsg.u.m_create_snap.device_id = first_seg(lmsg->u.lv)->device_id;
				dmsg.u.m_create_snap.origin_id = 0;//first_seg(first_seg(lmsg->u.lv)->origin)->device_id;
				if (!dm_tree_node_add_thin_pool_message(node, &dmsg))
					return_0;
				log_error(INTERNAL_ERROR "Sorry, not implemented yet.");
				return 0;
			case DM_THIN_MESSAGE_CREATE_THIN:
				log_debug("Thin pool create_thin %s.", lmsg->u.lv->name);
				dmsg.u.m_create_thin.device_id = first_seg(lmsg->u.lv)->device_id;
				if (!dm_tree_node_add_thin_pool_message(node, &dmsg))
					return_0;
				break;
			case DM_THIN_MESSAGE_DELETE:
				log_debug("Thin pool delete %u.", lmsg->u.delete_id);
				dmsg.u.m_delete.device_id = lmsg->u.delete_id;
				if (!dm_tree_node_add_thin_pool_message(node, &dmsg))
					return_0;
				break;
			case DM_THIN_MESSAGE_TRIM:
				/* FIXME: to be implemented */
				log_error(INTERNAL_ERROR "Sorry, not implemented yet.");
				return 0;
			default:
				log_error(INTERNAL_ERROR "Unsupported message.");
				return 0;
			}
		}

		log_debug("Thin pool set_transaction_id %" PRIu64 ".", seg->transaction_id);
		dmsg.type = DM_THIN_MESSAGE_SET_TRANSACTION_ID;
		dmsg.u.m_set_transaction_id.current_id = seg->transaction_id - 1;
		dmsg.u.m_set_transaction_id.new_id = seg->transaction_id;
		if (!dm_tree_node_add_thin_pool_message(node, &dmsg))
			return_0;
	}

	return 1;
}
#endif

static const char *_thin_name(const struct lv_segment *seg)
{
	return seg->segtype->name;
}

static int _thin_text_import(struct lv_segment *seg,
			     const struct dm_config_node *sn,
			     struct dm_hash_table *pv_hash __attribute__((unused)))
{
	const char *lv_name;
	struct logical_volume *pool_lv;

	if (!dm_config_get_str(sn, "thin_pool", &lv_name))
		return SEG_LOG_ERROR("Thin pool must be a string in");

	if (!(pool_lv = find_lv(seg->lv->vg, lv_name)))
		return SEG_LOG_ERROR("Unknown thin pool %s in", lv_name);

	if (!attach_pool_lv(seg, pool_lv))
		return_0;

	if (dm_config_has_node(sn, "origin")) {
		if (!dm_config_get_str(sn, "origin", &lv_name))
			return SEG_LOG_ERROR("Origin must be a string in");

		if (!(seg->origin = find_lv(seg->lv->vg, lv_name)))
			return SEG_LOG_ERROR("Unknown origin %s in", lv_name);
	}

	if (!dm_config_get_uint32(sn, "device_id", &seg->device_id))
		return SEG_LOG_ERROR("Could not read device_id for");

	if (seg->device_id > DM_THIN_MAX_DEVICE_ID)
		return SEG_LOG_ERROR("Unsupported value %u for device_id",
				     seg->device_id);

	return 1;
}

static int _thin_text_export(const struct lv_segment *seg, struct formatter *f)
{
	outf(f, "thin_pool = \"%s\"", seg->pool_lv->name);
	outf(f, "device_id = %d", seg->device_id);

	if (seg->origin)
		outf(f, "origin = \"%s\"", seg->origin->name);

	return 1;
}

#ifdef DEVMAPPER_SUPPORT
static int _thin_add_target_line(struct dev_manager *dm,
				 struct dm_pool *mem __attribute__((unused)),
				 struct cmd_context *cmd __attribute__((unused)),
				 void **target_state __attribute__((unused)),
				 struct lv_segment *seg,
				 const struct lv_activate_opts *laopts __attribute__((unused)),
				 struct dm_tree_node *node, uint64_t len,
				 uint32_t *pvmove_mirror_count __attribute__((unused)))
{
	char *thin_pool_dlid;

	if (!(thin_pool_dlid = build_dm_uuid(mem, seg->pool_lv->lvid.s, NULL))) {
		log_error("Failed to build uuid for thin pool LV %s.", seg->pool_lv->name);
		return 0;
	}

	if (!dm_tree_node_add_thin_target(node, len, thin_pool_dlid, seg->device_id))
		return_0;

	return 1;
}

static int _thin_target_percent(void **target_state __attribute__((unused)),
				percent_t *percent,
				struct dm_pool *mem __attribute__((unused)),
				struct cmd_context *cmd __attribute__((unused)),
				struct lv_segment *seg __attribute__((unused)),
				char *params, uint64_t *total_numerator,
				uint64_t *total_denominator)
{
	return 1;
}

static int _thin_target_present(struct cmd_context *cmd,
				const struct lv_segment *seg,
				unsigned *attributes __attribute__((unused)))
{
	static int _checked = 0;
	static int _present = 0;

	if (!_checked) {
		_present = target_present(cmd, THIN_MODULE, 1);
		_checked = 1;
	}

	return _present;
}

#endif

static int _thin_modules_needed(struct dm_pool *mem,
				const struct lv_segment *seg __attribute__((unused)),
				struct dm_list *modules)
{
	if (!str_list_add(mem, modules, THIN_MODULE)) {
		log_error("thin string list allocation failed");
		return 0;
	}

	return 1;
}

static void _thin_destroy(struct segment_type *segtype)
{
	dm_free(segtype);
}

static struct segtype_handler _thin_pool_ops = {
	.name = _thin_pool_name,
	.text_import = _thin_pool_text_import,
	.text_import_area_count = _thin_pool_text_import_area_count,
	.text_export = _thin_pool_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _thin_pool_add_target_line,
	.target_present = _thin_target_present,
#endif
	.modules_needed = _thin_modules_needed,
	.destroy = _thin_destroy,
};

static struct segtype_handler _thin_ops = {
	.name = _thin_name,
	.text_import = _thin_text_import,
	.text_export = _thin_text_export,
#ifdef DEVMAPPER_SUPPORT
	.add_target_line = _thin_add_target_line,
	.target_percent = _thin_target_percent,
	.target_present = _thin_target_present,
#endif
	.modules_needed = _thin_modules_needed,
	.destroy = _thin_destroy,
};

#ifdef THIN_INTERNAL
int init_thin_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#else /* Shared */
int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib);
int init_multiple_segtypes(struct cmd_context *cmd, struct segtype_library *seglib)
#endif
{
	static const struct {
		struct segtype_handler *ops;
		const char name[16];
		uint32_t flags;
	} reg_segtypes[] = {
		{ &_thin_pool_ops, "thin_pool", SEG_THIN_POOL },
		/* FIXME Maybe use SEG_THIN_VOLUME instead of SEG_VIRTUAL */
		{ &_thin_ops, "thin", SEG_THIN_VOLUME | SEG_VIRTUAL }
	};

	struct segment_type *segtype;
	unsigned i;

	for (i = 0; i < sizeof(reg_segtypes)/sizeof(reg_segtypes[0]); ++i) {
		segtype = dm_zalloc(sizeof(*segtype));

		if (!segtype) {
			log_error("Failed to allocate memory for %s segtype",
				  reg_segtypes[i].name);
			return 0;
		}

		segtype->ops = reg_segtypes[i].ops;
		segtype->name = reg_segtypes[i].name;
		segtype->flags = reg_segtypes[i].flags;

#ifdef DEVMAPPER_SUPPORT
#  ifdef DMEVENTD
// FIXME		if (_get_thin_dso_path(cmd))
// FIXME			segtype->flags |= SEG_MONITORED;
#  endif	/* DMEVENTD */
#endif
		if (!lvm_register_segtype(seglib, segtype))
			return_0;

		log_very_verbose("Initialised segtype: %s", segtype->name);
	}

	return 1;
}
