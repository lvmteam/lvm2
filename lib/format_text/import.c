/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "metadata.h"
#include "import-export.h"
#include "pool.h"
#include "log.h"
#include "uuid.h"
#include "hash.h"
#include "toolcontext.h"

typedef int (*section_fn) (struct format_instance * fid, struct pool * mem,
			   struct volume_group * vg, struct config_node * pvn,
			   struct config_node * vgn,
			   struct hash_table * pv_hash, struct uuid_map * um);

#define _read_int32(root, path, result) \
	get_config_uint32(root, path, '/', result)

#define _read_uint32(root, path, result) \
	get_config_uint32(root, path, '/', result)

#define _read_int64(root, path, result) \
	get_config_uint64(root, path, '/', result)

static int _read_id(struct id *id, struct config_node *cn, const char *path)
{
	struct config_value *cv;

	if (!(cn = find_config_node(cn, path, '/'))) {
		log_err("Couldn't find uuid.");
		return 0;
	}

	cv = cn->v;
	if (!cv || !cv->v.str) {
		log_err("uuid must be a string.");
		return 0;
	}

	if (!id_read_format(id, cv->v.str)) {
		log_err("Invalid uuid.");
		return 0;
	}

	return 1;
}

static int _read_pv(struct format_instance *fid, struct pool *mem,
		    struct volume_group *vg, struct config_node *pvn,
		    struct config_node *vgn,
		    struct hash_table *pv_hash, struct uuid_map *um)
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
		log_err("Empty pv section.");
		return 0;
	}

	if (!_read_id(&pv->id, pvn, "id")) {
		log_err("Couldn't read uuid for volume group.");
		return 0;
	}

	/*
	 * Use the uuid map to convert the uuid into a device.
	 */
	if (!(pv->dev = uuid_map_lookup(um, &pv->id))) {
		char buffer[64];

		if (!id_write_format(&pv->id, buffer, sizeof(buffer)))
			log_err("Couldn't find device.");
		else
			log_err("Couldn't find device with uuid '%s'.", buffer);

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
		log_err("Couldn't find status flags for physical volume.");
		return 0;
	}

	if (!(read_flags(&pv->status, PV_FLAGS, cn->v))) {
		log_err("Couldn't read status flags for physical volume.");
		return 0;
	}

	if (!_read_int64(pvn, "pe_start", &pv->pe_start)) {
		log_err("Couldn't read extent size for volume group.");
		return 0;
	}

	if (!_read_int32(pvn, "pe_count", &pv->pe_count)) {
		log_err("Couldn't find extent count (pe_count) for "
			"physical volume.");
		return 0;
	}

	/* adjust the volume group. */
	vg->extent_count += pv->pe_count;
	vg->free_count += pv->pe_count;

	pv->pe_size = vg->extent_size;
	pv->size = pv->pe_size * (uint64_t) pv->pe_count;
	pv->pe_alloc_count = 0;
	pv->fid = fid;

	vg->pv_count++;
	list_add(&vg->pvs, &pvl->list);

	return 1;
}

static void _insert_segment(struct logical_volume *lv,
			    struct stripe_segment *seg)
{
	struct list *segh;
	struct stripe_segment *comp;

	list_iterate(segh, &lv->segments) {
		comp = list_item(segh, struct stripe_segment);

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
	int s;
	uint32_t stripes;
	struct stripe_segment *seg;
	struct config_node *cn;
	struct config_value *cv;
	const char *seg_name = sn->key;

	if (!(sn = sn->child)) {
		log_err("Empty segment section.");
		return 0;
	}

	if (!_read_int32(sn, "stripes", &stripes)) {
		log_err("Couldn't read 'stripes' for segment '%s'.", sn->key);
		return 0;
	}

	if (!(seg = pool_zalloc(mem, sizeof(*seg) +
				(sizeof(seg->area[0]) * stripes)))) {
		stack;
		return 0;
	}
	seg->stripes = stripes;
	seg->lv = lv;

	if (!_read_int32(sn, "start_extent", &seg->le)) {
		log_err("Couldn't read 'start_extent' for segment '%s'.",
			sn->key);
		return 0;
	}

	if (!_read_int32(sn, "extent_count", &seg->len)) {
		log_err("Couldn't read 'extent_count' for segment '%s'.",
			sn->key);
		return 0;
	}

	if (seg->stripes == 0) {
		log_err("Zero stripes is *not* allowed for segment '%s'.",
			sn->key);
		return 0;
	}

	if ((seg->stripes != 1) &&
	    !_read_int32(sn, "stripe_size", &seg->stripe_size)) {
		log_err("Couldn't read 'stripe_size' for segment '%s'.",
			sn->key);
		return 0;
	}

	if (!(cn = find_config_node(sn, "areas", '/'))) {
		log_err("Couldn't find 'areas' array for segment '%s'.",
			sn->key);
		return 0;
	}

	/*
	 * Read the stripes from the 'areas' array.
	 * FIXME: we could move this to a separate function.
	 */
	for (cv = cn->v, s = 0; cv && s < seg->stripes; s++, cv = cv->next) {

		/* first we read the pv */
		const char *bad = "Badly formed areas array for segment '%s'.";
		struct physical_volume *pv;
		uint32_t allocated;

		if (cv->type != CFG_STRING) {
			log_err(bad, sn->key);
			return 0;
		}

		if (!(pv = hash_lookup(pv_hash, cv->v.str))) {
			log_err("Couldn't find physical volume '%s' for "
				"segment '%s'.",
				cn->v->v.str ? cn->v->v.str : "NULL", seg_name);
			return 0;
		}

		seg->area[s].pv = pv;

		if (!(cv = cv->next)) {
			log_err(bad, sn->key);
			return 0;
		}

		if (cv->type != CFG_INT) {
			log_err(bad, sn->key);
			return 0;
		}

		seg->area[s].pe = cv->v.i;

		/*
		 * Adjust the extent counts in the pv and vg.
		 */
		allocated = seg->len / seg->stripes;
		pv->pe_alloc_count += allocated;
		vg->free_count -= allocated;
	}

	/*
	 * Check we read the correct number of stripes.
	 */
	if (cv || (s < seg->stripes)) {
		log_err("Incorrect number of stripes in 'area' array "
			"for segment '%s'.", seg_name);
		return 0;
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
	}

	if (!_read_int32(lvn, "segment_count", &seg_count)) {
		log_err("Couldn't read segment count for logical volume.");
		return 0;
	}

	if (seg_count != count) {
		log_err("segment_count and actual number of segments "
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

static int _read_lv(struct format_instance *fid, struct pool *mem,
		    struct volume_group *vg, struct config_node *lvn,
		    struct config_node *vgn, struct hash_table *pv_hash,
		    struct uuid_map *um)
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
		log_err("Empty logical volume section.");
		return 0;
	}

	lv->vg = vg;

	/* FIXME: read full lvid */
	if (!_read_id(&lv->lvid.id[1], lvn, "id")) {
		log_err("Couldn't read uuid for logical volume %s.", lv->name);
		return 0;
	}

	memcpy(&lv->lvid.id[0], &lv->vg->id, sizeof(lv->lvid.id[0]));

	if (!(cn = find_config_node(lvn, "status", '/'))) {
		log_err("Couldn't find status flags for logical volume.");
		return 0;
	}

	if (!(read_flags(&lv->status, LV_FLAGS, cn->v))) {
		log_err("Couldn't read status flags for logical volume.");
		return 0;
	}

	lv->minor = -1;
	if ((lv->status & FIXED_MINOR) &&
	    !_read_int32(lvn, "minor", &lv->minor)) {
		log_error("Couldn't read 'minor' value for logical volume.");
		return 0;
	}

	if (!_read_int32(lvn, "read_ahead", &lv->read_ahead)) {
		log_err("Couldn't read 'read_ahead' value for "
			"logical volume.");
		return 0;
	}

	list_init(&lv->segments);
	if (!_read_segments(mem, vg, lv, lvn, pv_hash)) {
		stack;
		return 0;
	}
	lv->size = (uint64_t) lv->le_count * (uint64_t) vg->extent_size;

	vg->lv_count++;
	list_add(&vg->lvs, &lvl->list);

	return 1;
}

static int _read_snapshot(struct format_instance *fid, struct pool *mem,
			  struct volume_group *vg, struct config_node *sn,
			  struct config_node *vgn, struct hash_table *pv_hash,
			  struct uuid_map *um)
{
	uint32_t chunk_size;
	const char *org_name, *cow_name;
	struct logical_volume *org, *cow;

	if (!(sn = sn->child)) {
		log_err("Empty snapshot section.");
		return 0;
	}

	if (!_read_uint32(sn, "chunk_size", &chunk_size)) {
		log_err("Couldn't read chunk size for snapshot.");
		return 0;
	}

	if (!(cow_name = find_config_str(sn, "cow_store", '/', NULL))) {
		log_err("Snapshot cow storage not specified.");
		return 0;
	}

	if (!(org_name = find_config_str(sn, "origin", '/', NULL))) {
		log_err("Snapshot origin not specified.");
		return 0;
	}

	if (!(cow = find_lv(vg, cow_name))) {
		log_err("Unknown logical volume specified for "
			"snapshot cow store.");
		return 0;
	}

	if (!(org = find_lv(vg, org_name))) {
		log_err("Unknown logical volume specified for "
			"snapshot origin.");
		return 0;
	}

	if (!vg_add_snapshot(org, cow, 1, chunk_size)) {
		stack;
		return 0;
	}

	return 1;
}

static int _read_sections(struct format_instance *fid,
			  const char *section, section_fn fn,
			  struct pool *mem,
			  struct volume_group *vg, struct config_node *vgn,
			  struct hash_table *pv_hash,
			  struct uuid_map *um, int optional)
{
	struct config_node *n;

	if (!(n = find_config_node(vgn, section, '/'))) {
		if (!optional) {
			log_err("Couldn't find section '%s'.", section);
			return 0;
		}

		return 1;
	}

	for (n = n->child; n; n = n->sib) {
		if (!fn(fid, mem, vg, n, vgn, pv_hash, um)) {
			stack;
			return 0;
		}
	}

	return 1;
}

static struct volume_group *_read_vg(struct format_instance *fid,
				     struct config_file *cf,
				     struct uuid_map *um)
{
	struct config_node *vgn, *cn;
	struct volume_group *vg;
	struct hash_table *pv_hash = NULL;
	struct pool *mem = fid->fmt->cmd->mem;

	/* skip any top-level values */
	for (vgn = cf->root; (vgn && vgn->v); vgn = vgn->sib) ;

	if (!vgn) {
		log_err("Couldn't find volume group in file.");
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
		log_err("Couldn't read uuid for volume group %s.", vg->name);
		goto bad;
	}

	if (!_read_int32(vgn, "seqno", &vg->seqno)) {
		log_err("Couldn't read 'seqno' for volume group %s.", vg->name);
		goto bad;
	}

	if (!(cn = find_config_node(vgn, "status", '/'))) {
		log_err("Couldn't find status flags for volume group %s.",
			vg->name);
		goto bad;
	}

	if (!(read_flags(&vg->status, VG_FLAGS, cn->v))) {
		log_err("Couldn't read status flags for volume group %s.",
			vg->name);
		goto bad;
	}

	if (!_read_int32(vgn, "extent_size", &vg->extent_size)) {
		log_err("Couldn't read extent size for volume group %s.",
			vg->name);
		goto bad;
	}

	/*
	 * 'extent_count' and 'free_count' get filled in
	 * implicitly when reading in the pv's and lv's.
	 */

	if (!_read_int32(vgn, "max_lv", &vg->max_lv)) {
		log_err("Couldn't read 'max_lv' for volume group %s.",
			vg->name);
		goto bad;
	}

	if (!_read_int32(vgn, "max_pv", &vg->max_pv)) {
		log_err("Couldn't read 'max_pv' for volume group %s.",
			vg->name);
		goto bad;
	}

	/*
	 * The pv hash memoises the pv section names -> pv
	 * structures.
	 */
	if (!(pv_hash = hash_create(32))) {
		log_err("Couldn't create hash table.");
		goto bad;
	}

	list_init(&vg->pvs);
	if (!_read_sections(fid, "physical_volumes", _read_pv, mem, vg,
			    vgn, pv_hash, um, 0)) {
		log_err("Couldn't find all physical volumes for volume "
			"group %s.", vg->name);
		goto bad;
	}

	list_init(&vg->lvs);
	if (!_read_sections(fid, "logical_volumes", _read_lv, mem, vg,
			    vgn, pv_hash, um, 1)) {
		log_err("Couldn't read all logical volumes for volume "
			"group %s.", vg->name);
		goto bad;
	}

	list_init(&vg->snapshots);
	if (!_read_sections(fid, "snapshots", _read_snapshot, mem, vg,
			    vgn, pv_hash, um, 1)) {
		log_err("Couldn't read all snapshots for volume group %s.",
			vg->name);
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
		       struct config_file *cf, time_t * when, char **desc)
{
	const char *d;
	unsigned int u = 0u;

	d = find_config_str(cf->root, "description", '/', "");
	*desc = pool_strdup(mem, d);

	get_config_uint32(cf->root, "creation_time", '/', &u);
	*when = u;
}

struct volume_group *text_vg_import(struct format_instance *fid,
				    const char *file,
				    struct uuid_map *um,
				    time_t * when, char **desc)
{
	struct volume_group *vg = NULL;
	struct config_file *cf;

	*desc = NULL;
	*when = 0;

	if (!(cf = create_config_file())) {
		stack;
		goto out;
	}

	if (!read_config(cf, file)) {
		log_error("Couldn't read volume group file.");
		goto out;
	}

	if (!(vg = _read_vg(fid, cf, um))) {
		stack;
		goto out;
	}

	_read_desc(fid->fmt->cmd->mem, cf, when, desc);

      out:
	destroy_config_file(cf);
	return vg;
}
