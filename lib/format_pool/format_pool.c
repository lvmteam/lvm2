/*
 * Copyright (C) 1997-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
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
#include "label.h"
#include "metadata.h"
#include "limits.h"
#include "display.h"
#include "toolcontext.h"
#include "lvmcache.h"
#include "disk_rep.h"
#include "format_pool.h"
#include "pool_label.h"

/* Must be called after pvs are imported */
static struct user_subpool *_build_usp(struct dm_list *pls, struct dm_pool *mem,
				       int *sps)
{
	struct pool_list *pl;
	struct user_subpool *usp = NULL, *cur_sp = NULL;
	struct user_device *cur_dev = NULL;

	/*
	 * FIXME: Need to do some checks here - I'm tempted to add a
	 * user_pool structure and build the entire thing to check against.
	 */
	dm_list_iterate_items(pl, pls) {
		*sps = pl->pd.pl_subpools;
		if (!usp && (!(usp = dm_pool_zalloc(mem, sizeof(*usp) * (*sps))))) {
			log_error("Unable to allocate %d subpool structures",
				  *sps);
			return 0;
		}

		if (cur_sp != &usp[pl->pd.pl_sp_id]) {
			cur_sp = &usp[pl->pd.pl_sp_id];

			cur_sp->id = pl->pd.pl_sp_id;
			cur_sp->striping = pl->pd.pl_striping;
			cur_sp->num_devs = pl->pd.pl_sp_devs;
			cur_sp->type = pl->pd.pl_sp_type;
			cur_sp->initialized = 1;
		}

		if (!cur_sp->devs &&
		    (!(cur_sp->devs =
		       dm_pool_zalloc(mem,
				   sizeof(*usp->devs) * pl->pd.pl_sp_devs)))) {

			log_error("Unable to allocate %d pool_device "
				  "structures", pl->pd.pl_sp_devs);
			return 0;
		}

		cur_dev = &cur_sp->devs[pl->pd.pl_sp_devid];
		cur_dev->sp_id = cur_sp->id;
		cur_dev->devid = pl->pd.pl_sp_id;
		cur_dev->blocks = pl->pd.pl_blocks;
		cur_dev->pv = pl->pv;
		cur_dev->initialized = 1;
	}

	return usp;
}

static int _check_usp(const char *vgname, struct user_subpool *usp, int sp_count)
{
	int i;
	unsigned j;

	for (i = 0; i < sp_count; i++) {
		if (!usp[i].initialized) {
			log_error("Missing subpool %d in pool %s", i, vgname);
			return 0;
		}
		for (j = 0; j < usp[i].num_devs; j++) {
			if (!usp[i].devs[j].initialized) {
				log_error("Missing device %u for subpool %d"
					  " in pool %s", j, i, vgname);
				return 0;
			}

		}
	}

	return 1;
}

static struct volume_group *_pool_vg_read(struct format_instance *fid,
					  const char *vg_name,
					  struct metadata_area *mda __attribute__((unused)))
{
	struct volume_group *vg;
	struct user_subpool *usp;
	int sp_count;
	DM_LIST_INIT(pds);

	/* We can safely ignore the mda passed in */

	/* Strip dev_dir if present */
	vg_name = strip_dir(vg_name, fid->fmt->cmd->dev_dir);

	/* Set vg_name through read_pool_pds() */
	if (!(vg = alloc_vg("pool_vg_read", fid->fmt->cmd, NULL)))
		return_NULL;

	/* Read all the pvs in the vg */
	if (!read_pool_pds(fid->fmt, vg_name, vg->vgmem, &pds))
		goto_bad;

	vg_set_fid(vg, fid);

	/* Setting pool seqno to 1 because the code always did this,
	 * although we don't think it's needed. */
	vg->seqno = 1;

	if (!import_pool_vg(vg, vg->vgmem, &pds))
		goto_bad;

	if (!import_pool_pvs(fid->fmt, vg, vg->vgmem, &pds))
		goto_bad;

	if (!import_pool_lvs(vg, vg->vgmem, &pds))
		goto_bad;

	/*
	 * I need an intermediate subpool structure that contains all the
	 * relevant info for this.  Then i can iterate through the subpool
	 * structures for checking, and create the segments
	 */
	if (!(usp = _build_usp(&pds, vg->vgmem, &sp_count)))
		goto_bad;

	/*
	 * check the subpool structures - we can't handle partial VGs in
	 * the pool format, so this will error out if we're missing PVs
	 */
	if (!_check_usp(vg->name, usp, sp_count))
		goto_bad;

	if (!import_pool_segments(&vg->lvs, vg->vgmem, usp, sp_count))
		goto_bad;

	return vg;

bad:
	release_vg(vg);

	return NULL;
}

static int _pool_pv_initialise(const struct format_type *fmt __attribute__((unused)),
			       int64_t label_sector __attribute__((unused)),
			       uint64_t pe_start __attribute__((unused)),
			       uint32_t extent_count __attribute__((unused)),
			       uint32_t extent_size __attribute__((unused)),
			       unsigned long data_alignment __attribute__((unused)),
			       unsigned long data_alignment_offset __attribute__((unused)),
			       struct physical_volume *pv __attribute__((unused)))
{
	return 1;
}

static int _pool_pv_setup(const struct format_type *fmt __attribute__((unused)),
			  struct physical_volume *pv __attribute__((unused)),
			  struct volume_group *vg __attribute__((unused)))
{
	return 1;
}

static int _pool_pv_read(const struct format_type *fmt, const char *pv_name,
			 struct physical_volume *pv,
			 int scan_label_only __attribute__((unused)))
{
	struct dm_pool *mem = dm_pool_create("pool pv_read", 1024);
	struct pool_list *pl;
	struct device *dev;
	int r = 0;

	log_very_verbose("Reading physical volume data %s from disk", pv_name);

	if (!mem)
		return_0;

	if (!(dev = dev_cache_get(pv_name, fmt->cmd->filter)))
		goto_out;

	/*
	 * I need to read the disk and populate a pv structure here
	 * I'll probably need to abstract some of this later for the
	 * vg_read code
	 */
	if (!(pl = read_pool_disk(fmt, dev, mem, NULL)))
		goto_out;

	if (!import_pool_pv(fmt, fmt->cmd->mem, NULL, pv, pl))
		goto_out;

	pv->fmt = fmt;

	r = 1;

      out:
	dm_pool_destroy(mem);
	return r;
}

/* *INDENT-OFF* */
static struct metadata_area_ops _metadata_format_pool_ops = {
	.vg_read = _pool_vg_read,
};
/* *INDENT-ON* */

static struct format_instance *_pool_create_instance(const struct format_type *fmt,
						     const struct format_instance_ctx *fic)
{
	struct format_instance *fid;
	struct metadata_area *mda;

	if (!(fid = alloc_fid(fmt, fic)))
		return_NULL;

	/* Define a NULL metadata area */
	if (!(mda = dm_pool_zalloc(fid->mem, sizeof(*mda)))) {
		log_error("Unable to allocate metadata area structure "
			  "for pool format");
		goto bad;
	}

	mda->ops = &_metadata_format_pool_ops;
	mda->metadata_locn = NULL;
	mda->status = 0;
	dm_list_add(&fid->metadata_areas_in_use, &mda->list);

	return fid;

bad:
	dm_pool_destroy(fid->mem);
	return NULL;
}

static void _pool_destroy_instance(struct format_instance *fid)
{
	if (--fid->ref_count <= 1)
		dm_pool_destroy(fid->mem);
}

static void _pool_destroy(struct format_type *fmt)
{
	/* FIXME out of place, but the main (cmd) pool has been already
	 * destroyed and touching the fid (also via release_vg) will crash the
	 * program */
	dm_hash_destroy(fmt->orphan_vg->hostnames);
	dm_pool_destroy(fmt->orphan_vg->fid->mem);
	dm_pool_destroy(fmt->orphan_vg->vgmem);

	dm_free(fmt);
}

/* *INDENT-OFF* */
static struct format_handler _format_pool_ops = {
	.pv_read = _pool_pv_read,
	.pv_initialise = _pool_pv_initialise,
	.pv_setup = _pool_pv_setup,
	.create_instance = _pool_create_instance,
	.destroy_instance = _pool_destroy_instance,
	.destroy = _pool_destroy,
};
/* *INDENT-ON */

#ifdef POOL_INTERNAL
struct format_type *init_pool_format(struct cmd_context *cmd)
#else				/* Shared */
struct format_type *init_format(struct cmd_context *cmd);
struct format_type *init_format(struct cmd_context *cmd)
#endif
{
	struct format_type *fmt = dm_malloc(sizeof(*fmt));
	struct format_instance_ctx fic;
	struct format_instance *fid;

	if (!fmt) {
		log_error("Unable to allocate format type structure for pool "
			  "format");
		return NULL;
	}

	fmt->cmd = cmd;
	fmt->ops = &_format_pool_ops;
	fmt->name = FMT_POOL_NAME;
	fmt->alias = NULL;
	fmt->orphan_vg_name = FMT_POOL_ORPHAN_VG_NAME;
	fmt->features = 0;
	fmt->private = NULL;

	if (!(fmt->labeller = pool_labeller_create(fmt))) {
		log_error("Couldn't create pool label handler.");
		dm_free(fmt);
		return NULL;
	}

	if (!(label_register_handler(FMT_POOL_NAME, fmt->labeller))) {
		log_error("Couldn't register pool label handler.");
		fmt->labeller->ops->destroy(fmt->labeller);
		dm_free(fmt);
		return NULL;
	}

	if (!(fmt->orphan_vg = alloc_vg("text_orphan", cmd, fmt->orphan_vg_name))) {
		log_error("Couldn't create lvm1 orphan VG.");
		return NULL;
	}
	fic.type = FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = fmt->orphan_vg_name;
	fic.context.vg_ref.vg_id = NULL;
	if (!(fid = _pool_create_instance(fmt, &fic))) {
		log_error("Couldn't create lvm1 orphan VG format instance.");
		return NULL;
	}
	vg_set_fid(fmt->orphan_vg, fid);

	log_very_verbose("Initialised format: %s", fmt->name);

	return fmt;
}
