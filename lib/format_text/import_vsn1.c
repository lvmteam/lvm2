/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "lib.h"
#include "metadata.h"
#include "import-export.h"
#include "pool.h"
#include "display.h"
#include "hash.h"
#include "toolcontext.h"
#include "lvmcache.h"

typedef int (*section_fn) (struct format_instance * fid, struct pool * mem,
			   struct volume_group * vg, struct config_node * pvn,
			   struct config_node * vgn,
			   struct hash_table * pv_hash);

#define _read_int32(root, path, result) \
	get_config_uint32(root, path, '/', result)

#define _read_uint32(root, path, result) \
	get_config_uint32(root, path, '/', result)

#define _read_int64(root, path, result) \
	get_config_uint64(root, path, '/', result)

/*
 * Logs an attempt to read an invalid format file.
 */
static void _invalid_format(const char *str)
{
	log_error("Can't process text format file - %s.", str);
}

/*
 * Checks that the config file contains vg metadata, and that it
 * we recognise the version number,
 */
static int _check_version(struct config_tree *cf)
{
	struct config_node *cn;
	struct config_value *cv;

	/*
	 * Check the contents field.
	 */
	if (!(cn = find_config_node(cf->root, CONTENTS_FIELD, '/'))) {
		_invalid_format("missing contents field");
		return 0;
	}

	cv = cn->v;
	if (!cv || cv->type != CFG_STRING || strcmp(cv->v.str, CONTENTS_VALUE)) {
		_invalid_format("unrecognised contents field");
		return 0;
	}

	/*
	 * Check the version number.
	 */
	if (!(cn = find_config_node(cf->root, FORMAT_VERSION_FIELD, '/'))) {
		_invalid_format("missing version number");
		return 0;
	}

	cv = cn->v;
	if (!cv || cv->type != CFG_INT || cv->v.i != FORMAT_VERSION_VALUE) {
		_invalid_format("unrecognised version number");
		return 0;
	}

	return 1;
}

static int _read_id(struct id *id, struct config_node *cn, const char *path)
{
	struct config_value *cv;

	if (!(cn = find_config_node(cn, path, '/'))) {
		log_error("Couldn't find uuid.");
		return 0;
	}

	cv = cn->v;
	if (!cv || !cv->v.str) {
		log_error("uuid must be a string.");
		return 0;
	}

	if (!id_read_format(id, cv->v.str)) {
		log_error("Invalid uuid.");
		return 0;
	}

	return 1;
}

static int _read_pv(struct format_instance *fid, struct pool *mem,
		    struct volume_group *vg, struct config_node *pvn,
		    struct config_node *vgn, struct hash_table *pv_hash)
{
	struct physical_volume *pv;
	struct pv_list *pvl;
	struct config_node *cn;

	if (!(pvl = pool_zalloc(mem, sizeof(*pvl))) ||
	    !(pvl->pv = pool_zalloc(mem, sizeof(*pvl->pv)))) {
		stack;
		return 0;
	}

	pv = pvl->pv;

	/*
	 * Add the pv to the pv hash for quick lookup when we read
	 * the lv segments.
	 */
	if (!hash_insert(pv_hash, pvn->key, pv)) {
		stack;
		return 0;
	}

	if (!(pvn = pvn->child)) {
		log_error("Empty pv section.");
		return 0;
	}

	if (!_read_id(&pv->id, pvn, "id")) {
		log_error("Couldn't read uuid for volume group.");
		return 0;
	}

	/*
	 * Convert the uuid into a device.
	 */
	if (!(pv->dev = device_from_pvid(fid->fmt->cmd, &pv->id))) {
		char buffer[64];

		if (!id_write_format(&pv->id, buffer, sizeof(buffer)))
			log_error("Couldn't find device.");
		else
			log_error("Couldn't find device with uuid '%s'.",
				  buffer);

		if (partial_mode())
			vg->status |= PARTIAL_VG;
		else
			return 0;
	}

	if (!(pv->vg_name = pool_strdup(mem, vg->name))) {
		stack;
		return 0;
	}

	if (!(cn = find_config_node(pvn, "status", '/'))) {
		log_error("Couldn't find status flags for physical volume.");
		return 0;
	}

	if (!(read_flags(&pv->status, PV_FLAGS, cn->v))) {
		log_error("Couldn't read status flags for physical volume.");
		return 0;
	}

	if (!_read_int64(pvn, "pe_start", &pv->pe_start)) {
		log_error("Couldn't read extent size for volume group.");
		return 0;
	}

	if (!_read_int32(pvn, "pe_count", &pv->pe_count)) {
		log_error("Couldn't find extent count (pe_count) for "
			  "physical volume.");
		return 0;
	}

	/* adjust the volume group. */
	vg->extent_count += pv->pe_count;
	vg->free_count += pv->pe_count;

	pv->pe_size = vg->extent_size;
	pv->size = vg->extent_size * (uint64_t) pv->pe_count;
	pv->pe_alloc_count = 0;
	pv->fmt = fid->fmt;

	vg->pv_count++;
	list_add(&vg->pvs, &pvl->list);

	return 1;
}

static void _insert_segment(struct logical_volume *lv, struct lv_segment *seg)
{
	struct list *segh;
	struct lv_segment *comp;

	list_iterate(segh, &lv->segments) {
		comp = list_item(segh, struct lv_segment);

		if (comp->le > seg->le) {
			list_add(&comp->list, &seg->list);
			return;
		}
	}

	lv->le_count += seg->len;
	list_add(&lv->segments, &seg->list);
}

static int _read_segment(struct pool *mem, struct volume_group *vg,
			 struct logical_volume *lv, struct config_node *sn,
			 struct hash_table *pv_hash)
{
	unsigned int s;
	uint32_t area_count = 0;
	struct lv_segment *seg;
	struct config_node *cn;
	struct config_value *cv;
	const char *seg_name = sn->key;
	uint32_t start_extent, extent_count;
	uint32_t chunk_size, extents_moved = 0u, seg_status = 0u;
	const char *org_name, *cow_name;
	struct logical_volume *org, *cow, *lv1;
	segment_type_t segtype;

	if (!(sn = sn->child)) {
		log_error("Empty segment section.");
		return 0;
	}

	if (!_read_int32(sn, "start_extent", &start_extent)) {
		log_error("Couldn't read 'start_extent' for segment '%s'.",
			  sn->key);
		return 0;
	}

	if (!_read_int32(sn, "extent_count", &extent_count)) {
		log_error("Couldn't read 'extent_count' for segment '%s'.",
			  sn->key);
		return 0;
	}

	segtype = SEG_STRIPED;	/* Default */
	if ((cn = find_config_node(sn, "type", '/'))) {
		cv = cn->v;
		if (!cv || !cv->v.str) {
			log_error("Segment type must be a string.");
			return 0;
		}
		segtype = get_segtype_from_string(cv->v.str);
	}

	if (segtype == SEG_STRIPED) {
		if (!_read_int32(sn, "stripe_count", &area_count)) {
			log_error("Couldn't read 'stripe_count' for "
				  "segment '%s'.", sn->key);
			return 0;
		}
	}

	if (segtype == SEG_MIRRORED) {
		if (!_read_int32(sn, "mirror_count", &area_count)) {
			log_error("Couldn't read 'mirror_count' for "
				  "segment '%s'.", sn->key);
			return 0;
		}

		if (find_config_node(sn, "extents_moved", '/')) {
			if (_read_uint32(sn, "extents_moved", &extents_moved))
				seg_status |= PVMOVE;
			else {
				log_error("Couldn't read 'extents_moved' for "
					  "segment '%s'.", sn->key);
				return 0;
			}
		}
	}

	if (!(seg = pool_zalloc(mem, sizeof(*seg) +
				(sizeof(seg->area[0]) * area_count)))) {
		stack;
		return 0;
	}

	seg->lv = lv;
	seg->le = start_extent;
	seg->len = extent_count;
	seg->area_len = extent_count;
	seg->type = segtype;
	seg->status = seg_status;
	seg->extents_moved = extents_moved;

	switch (segtype) {
	case SEG_SNAPSHOT:
		lv->status |= SNAPSHOT;

		if (!_read_uint32(sn, "chunk_size", &chunk_size)) {
			log_error("Couldn't read chunk size for snapshot.");
			return 0;
		}

		log_suppress(1);

		if (!(cow_name = find_config_str(sn, "cow_store", '/', NULL))) {
			log_suppress(0);
			log_error("Snapshot cow storage not specified.");
			return 0;
		}

		if (!(org_name = find_config_str(sn, "origin", '/', NULL))) {
			log_suppress(0);
			log_error("Snapshot origin not specified.");
			return 0;
		}

		log_suppress(0);

		if (!(cow = find_lv(vg, cow_name))) {
			log_error("Unknown logical volume specified for "
				  "snapshot cow store.");
			return 0;
		}

		if (!(org = find_lv(vg, org_name))) {
			log_error("Unknown logical volume specified for "
				  "snapshot origin.");
			return 0;
		}

		if (!vg_add_snapshot(org, cow, 1, &lv->lvid.id[1], chunk_size)) {
			stack;
			return 0;
		}
		break;

	case SEG_STRIPED:
		if ((area_count != 1) &&
		    !_read_int32(sn, "stripe_size", &seg->stripe_size)) {
			log_error("Couldn't read stripe_size for segment '%s'.",
				  sn->key);
			return 0;
		}

		if (!(cn = find_config_node(sn, "stripes", '/'))) {
			log_error("Couldn't find stripes array for segment "
				  "'%s'.", sn->key);
			return 0;
		}

		seg->area_len /= area_count;

	case SEG_MIRRORED:
		seg->area_count = area_count;

		if (!seg->area_count) {
			log_error("Zero areas not allowed for segment '%s'",
				  sn->key);
			return 0;
		}

		if ((seg->type == SEG_MIRRORED) &&
		    !(cn = find_config_node(sn, "mirrors", '/'))) {
			log_error("Couldn't find mirrors array for segment "
				  "'%s'.", sn->key);
			return 0;
		}

		for (cv = cn->v, s = 0; cv && s < seg->area_count;
		     s++, cv = cv->next) {

			/* first we read the pv */
			const char *bad = "Badly formed areas array for "
			    "segment '%s'.";
			struct physical_volume *pv;

			if (cv->type != CFG_STRING) {
				log_error(bad, sn->key);
				return 0;
			}

			if (!cv->next) {
				log_error(bad, sn->key);
				return 0;
			}

			if (cv->next->type != CFG_INT) {
				log_error(bad, sn->key);
				return 0;
			}

			/* FIXME Cope if LV not yet read in */
			if ((pv = hash_lookup(pv_hash, cv->v.str))) {
				seg->area[s].type = AREA_PV;
				seg->area[s].u.pv.pv = pv;
				seg->area[s].u.pv.pe = cv->next->v.i;
				/*
				 * Adjust extent counts in the pv and vg.
				 */
				pv->pe_alloc_count += seg->area_len;
				vg->free_count -= seg->area_len;

			} else if ((lv1 = find_lv(vg, cv->v.str))) {
				seg->area[s].type = AREA_LV;
				seg->area[s].u.lv.lv = lv1;
				seg->area[s].u.lv.le = cv->next->v.i;
			} else {
				log_error("Couldn't find volume '%s' "
					  "for segment '%s'.",
					  cv->v.str ? cv->v.str : "NULL",
					  seg_name);
				return 0;
			}

			cv = cv->next;
		}

		/*
		 * Check we read the correct number of stripes.
		 */
		if (cv || (s < seg->area_count)) {
			log_error("Incorrect number of areas in area array "
				  "for segment '%s'.", seg_name);
			return 0;
		}

	}

	/*
	 * Insert into correct part of segment list.
	 */
	_insert_segment(lv, seg);
	return 1;
}

static int _read_segments(struct pool *mem, struct volume_group *vg,
			  struct logical_volume *lv, struct config_node *lvn,
			  struct hash_table *pv_hash)
{
	struct config_node *sn;
	int count = 0, seg_count;

	for (sn = lvn; sn; sn = sn->sib) {

		/*
		 * All sub-sections are assumed to be segments.
		 */
		if (!sn->v) {
			if (!_read_segment(mem, vg, lv, sn, pv_hash)) {
				stack;
				return 0;
			}

			count++;
		}
		/* FIXME Remove this restriction */
		if ((lv->status & SNAPSHOT) && count > 1) {
			log_error("Only one segment permitted for snapshot");
			return 0;
		}
	}

	if (!_read_int32(lvn, "segment_count", &seg_count)) {
		log_error("Couldn't read segment count for logical volume.");
		return 0;
	}

	if (seg_count != count) {
		log_error("segment_count and actual number of segments "
			  "disagree.");
		return 0;
	}

	/*
	 * Check there are no gaps or overlaps in the lv.
	 */
	if (!lv_check_segments(lv)) {
		stack;
		return 0;
	}

	/*
	 * Merge segments in case someones been editing things by hand.
	 */
	if (!lv_merge_segments(lv)) {
		stack;
		return 0;
	}

	return 1;
}

static int _read_lvnames(struct format_instance *fid, struct pool *mem,
			 struct volume_group *vg, struct config_node *lvn,
			 struct config_node *vgn, struct hash_table *pv_hash)
{
	struct logical_volume *lv;
	struct lv_list *lvl;
	struct config_node *cn;

	if (!(lvl = pool_zalloc(mem, sizeof(*lvl))) ||
	    !(lvl->lv = pool_zalloc(mem, sizeof(*lvl->lv)))) {
		stack;
		return 0;
	}

	lv = lvl->lv;

	if (!(lv->name = pool_strdup(mem, lvn->key))) {
		stack;
		return 0;
	}

	if (!(lvn = lvn->child)) {
		log_error("Empty logical volume section.");
		return 0;
	}

	if (!(cn = find_config_node(lvn, "status", '/'))) {
		log_error("Couldn't find status flags for logical volume.");
		return 0;
	}

	if (!(read_flags(&lv->status, LV_FLAGS, cn->v))) {
		log_error("Couldn't read status flags for logical volume.");
		return 0;
	}

	lv->alloc = ALLOC_DEFAULT;
	if ((cn = find_config_node(lvn, "allocation_policy", '/'))) {
		struct config_value *cv = cn->v;
		if (!cv || !cv->v.str) {
			log_error("allocation_policy must be a string.");
			return 0;
		}

		lv->alloc = get_alloc_from_string(cv->v.str);
	}

	/* read_ahead defaults to 0 */
	if (!_read_int32(lvn, "read_ahead", &lv->read_ahead))
		lv->read_ahead = 0;

	list_init(&lv->segments);

	lv->vg = vg;
	vg->lv_count++;
	list_add(&vg->lvs, &lvl->list);

	return 1;
}

static int _read_lvsegs(struct format_instance *fid, struct pool *mem,
			struct volume_group *vg, struct config_node *lvn,
			struct config_node *vgn, struct hash_table *pv_hash)
{
	struct logical_volume *lv;
	struct lv_list *lvl;

	if (!(lvl = find_lv_in_vg(vg, lvn->key))) {
		log_error("Lost logical volume reference %s", lvn->key);
		return 0;
	}

	lv = lvl->lv;

	if (!(lvn = lvn->child)) {
		log_error("Empty logical volume section.");
		return 0;
	}

	/* FIXME: read full lvid */
	if (!_read_id(&lv->lvid.id[1], lvn, "id")) {
		log_error("Couldn't read uuid for logical volume %s.",
			  lv->name);
		return 0;
	}

	memcpy(&lv->lvid.id[0], &lv->vg->id, sizeof(lv->lvid.id[0]));

	if (!_read_segments(mem, vg, lv, lvn, pv_hash)) {
		stack;
		return 0;
	}

	lv->size = (uint64_t) lv->le_count * (uint64_t) vg->extent_size;

	/* Skip this for now for snapshots */
	if (!(lv->status & SNAPSHOT)) {
		lv->minor = -1;
		if ((lv->status & FIXED_MINOR) &&
		    !_read_int32(lvn, "minor", &lv->minor)) {
			log_error("Couldn't read minor number for logical "
				  "volume %s.", lv->name);
			return 0;
		}
		lv->major = -1;
		if ((lv->status & FIXED_MINOR) &&
		    !_read_int32(lvn, "major", &lv->major)) {
			log_error("Couldn't read major number for logical "
				  "volume %s.", lv->name);
		}
	} else {
		vg->lv_count--;
		list_del(&lvl->list);
	}

	return 1;
}

static int _read_sections(struct format_instance *fid,
			  const char *section, section_fn fn,
			  struct pool *mem,
			  struct volume_group *vg, struct config_node *vgn,
			  struct hash_table *pv_hash, int optional)
{
	struct config_node *n;

	if (!(n = find_config_node(vgn, section, '/'))) {
		if (!optional) {
			log_error("Couldn't find section '%s'.", section);
			return 0;
		}

		return 1;
	}

	for (n = n->child; n; n = n->sib) {
		if (!fn(fid, mem, vg, n, vgn, pv_hash)) {
			stack;
			return 0;
		}
	}

	return 1;
}

static struct volume_group *_read_vg(struct format_instance *fid,
				     struct config_tree *cf)
{
	struct config_node *vgn, *cn;
	struct volume_group *vg;
	struct hash_table *pv_hash = NULL;
	struct pool *mem = fid->fmt->cmd->mem;

	/* skip any top-level values */
	for (vgn = cf->root; (vgn && vgn->v); vgn = vgn->sib) ;

	if (!vgn) {
		log_error("Couldn't find volume group in file.");
		return NULL;
	}

	if (!(vg = pool_zalloc(mem, sizeof(*vg)))) {
		stack;
		return NULL;
	}
	vg->cmd = fid->fmt->cmd;

	/* FIXME Determine format type from file contents */
	/* eg Set to instance of fmt1 here if reading a format1 backup? */
	vg->fid = fid;

	if (!(vg->name = pool_strdup(mem, vgn->key))) {
		stack;
		goto bad;
	}

	if (!(vg->system_id = pool_zalloc(mem, NAME_LEN))) {
		stack;
		goto bad;
	}

	vgn = vgn->child;

	if ((cn = find_config_node(vgn, "system_id", '/')) && cn->v) {
		if (!cn->v->v.str) {
			log_error("system_id must be a string");
			goto bad;
		}
		strncpy(vg->system_id, cn->v->v.str, NAME_LEN);
	}

	if (!_read_id(&vg->id, vgn, "id")) {
		log_error("Couldn't read uuid for volume group %s.", vg->name);
		goto bad;
	}

	if (!_read_int32(vgn, "seqno", &vg->seqno)) {
		log_error("Couldn't read 'seqno' for volume group %s.",
			  vg->name);
		goto bad;
	}

	if (!(cn = find_config_node(vgn, "status", '/'))) {
		log_error("Couldn't find status flags for volume group %s.",
			  vg->name);
		goto bad;
	}

	if (!(read_flags(&vg->status, VG_FLAGS, cn->v))) {
		log_error("Couldn't read status flags for volume group %s.",
			  vg->name);
		goto bad;
	}

	if (!_read_int32(vgn, "extent_size", &vg->extent_size)) {
		log_error("Couldn't read extent size for volume group %s.",
			  vg->name);
		goto bad;
	}

	/*
	 * 'extent_count' and 'free_count' get filled in
	 * implicitly when reading in the pv's and lv's.
	 */

	if (!_read_int32(vgn, "max_lv", &vg->max_lv)) {
		log_error("Couldn't read 'max_lv' for volume group %s.",
			  vg->name);
		goto bad;
	}

	if (!_read_int32(vgn, "max_pv", &vg->max_pv)) {
		log_error("Couldn't read 'max_pv' for volume group %s.",
			  vg->name);
		goto bad;
	}

	/*
	 * The pv hash memoises the pv section names -> pv
	 * structures.
	 */
	if (!(pv_hash = hash_create(32))) {
		log_error("Couldn't create hash table.");
		goto bad;
	}

	list_init(&vg->pvs);
	if (!_read_sections(fid, "physical_volumes", _read_pv, mem, vg,
			    vgn, pv_hash, 0)) {
		log_error("Couldn't find all physical volumes for volume "
			  "group %s.", vg->name);
		goto bad;
	}

	list_init(&vg->lvs);
	list_init(&vg->snapshots);

	if (!_read_sections(fid, "logical_volumes", _read_lvnames, mem, vg,
			    vgn, pv_hash, 1)) {
		log_error("Couldn't read all logical volume names for volume "
			  "group %s.", vg->name);
		goto bad;
	}

	if (!_read_sections(fid, "logical_volumes", _read_lvsegs, mem, vg,
			    vgn, pv_hash, 1)) {
		log_error("Couldn't read all logical volumes for "
			  "volume group %s.", vg->name);
		goto bad;
	}

	hash_destroy(pv_hash);

	if (vg->status & PARTIAL_VG) {
		vg->status &= ~LVM_WRITE;
		vg->status |= LVM_READ;
	}

	/*
	 * Finished.
	 */
	return vg;

      bad:
	if (pv_hash)
		hash_destroy(pv_hash);

	pool_free(mem, vg);
	return NULL;
}

static void _read_desc(struct pool *mem,
		       struct config_tree *cf, time_t *when, char **desc)
{
	const char *d;
	unsigned int u = 0u;

	log_suppress(1);
	d = find_config_str(cf->root, "description", '/', "");
	log_suppress(0);
	*desc = pool_strdup(mem, d);

	get_config_uint32(cf->root, "creation_time", '/', &u);
	*when = u;
}

static struct text_vg_version_ops _vsn1_ops = {
	check_version:_check_version,
	read_vg:_read_vg,
	read_desc:_read_desc
};

struct text_vg_version_ops *text_vg_vsn1_init(void)
{
	return &_vsn1_ops;
};
