/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2008 Red Hat, Inc. All rights reserved.
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

#include "lib.h"
#include "metadata.h"
#include "import-export.h"
#include "toolcontext.h"

/* FIXME Use tidier inclusion method */
static struct text_vg_version_ops *(_text_vsn_list[2]);

static int _text_import_initialised = 0;

static void _init_text_import(void)
{
	if (_text_import_initialised)
		return;

	_text_vsn_list[0] = text_vg_vsn1_init();
	_text_vsn_list[1] = NULL;
	_text_import_initialised = 1;
}

struct import_vgsummary_params {
	const struct format_type *fmt;
	struct dm_config_tree *cft;
	int checksum_only;
	struct lvmcache_vgsummary *vgsummary;
};

static int _import_vgsummary(struct import_vgsummary_params *ivsp)
{
	struct text_vg_version_ops **vsn;
	int r = 0;

	if (ivsp->checksum_only) {
		/* Checksum matches already-cached content - no need to reparse. */
		r = 1;
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(ivsp->cft))
			continue;

		if (!(*vsn)->read_vgsummary(ivsp->fmt, ivsp->cft, ivsp->vgsummary))
			goto_out;

		r = 1;
		break;
	}

out:
	config_destroy(ivsp->cft);
	return r;
}

/*
 * Find out vgname on a given device.
 */
int text_vgsummary_import(const struct format_type *fmt,
		       struct device *dev, dev_io_reason_t reason,
		       off_t offset, uint32_t size,
		       off_t offset2, uint32_t size2,
		       checksum_fn_t checksum_fn,
		       int checksum_only,
		       struct lvmcache_vgsummary *vgsummary)
{
	struct import_vgsummary_params *ivsp;

	_init_text_import();

	if (!(ivsp = dm_pool_zalloc(fmt->cmd->mem, sizeof(*ivsp)))) {
		log_error("Failed to allocate import_vgsummary_params struct.");
		return 0;
	}

	if (!(ivsp->cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0)))
		return_0;

	ivsp->fmt = fmt;
	ivsp->checksum_only = checksum_only;
	ivsp->vgsummary = vgsummary;

	if (!dev && !config_file_read(fmt->cmd->mem, ivsp->cft)) {
		log_error("Couldn't read volume group metadata.");
		config_destroy(ivsp->cft);
		return 0;
	}

	if (dev && !config_file_read_fd(fmt->cmd->mem, ivsp->cft, dev, reason, offset, size,
					offset2, size2, checksum_fn,
					vgsummary->mda_checksum,
					checksum_only, 1)) {
		log_error("Couldn't read volume group metadata.");
		config_destroy(ivsp->cft);
		return 0;
	}

	return _import_vgsummary(ivsp);
}

struct cached_vg_fmtdata {
        uint32_t cached_mda_checksum;
        size_t cached_mda_size;
};

struct import_vg_params {
	struct format_instance *fid;
	struct dm_config_tree *cft;
	int single_device;
	int skip_parse;
	unsigned *use_previous_vg;
	struct volume_group *vg;
	uint32_t checksum;
	uint32_t total_size;
	time_t *when;
	struct cached_vg_fmtdata **vg_fmtdata;
	char **desc;
};

static void _import_vg(struct import_vg_params *ivp)
{
	struct text_vg_version_ops **vsn;

	ivp->vg = NULL;

	if (ivp->skip_parse) {
		if (ivp->use_previous_vg)
			*ivp->use_previous_vg = 1;
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(ivp->cft))
			continue;

		if (!(ivp->vg = (*vsn)->read_vg(ivp->fid, ivp->cft, ivp->single_device, 0)))
			goto_out;

		(*vsn)->read_desc(ivp->vg->vgmem, ivp->cft, ivp->when, ivp->desc);
		break;
	}

	if (ivp->vg && ivp->vg_fmtdata && *ivp->vg_fmtdata) {
		(*ivp->vg_fmtdata)->cached_mda_size = ivp->total_size;
		(*ivp->vg_fmtdata)->cached_mda_checksum = ivp->checksum;
	}

	if (ivp->use_previous_vg)
		*ivp->use_previous_vg = 0;

out:
	config_destroy(ivp->cft);
}

struct volume_group *text_vg_import_fd(struct format_instance *fid,
				       const char *file,
				       struct cached_vg_fmtdata **vg_fmtdata,
				       unsigned *use_previous_vg,
				       int single_device,
				       struct device *dev, int primary_mda,
				       off_t offset, uint32_t size,
				       off_t offset2, uint32_t size2,
				       checksum_fn_t checksum_fn,
				       uint32_t checksum,
				       time_t *when, char **desc)
{
	struct import_vg_params *ivp;

	if (vg_fmtdata && !*vg_fmtdata &&
	    !(*vg_fmtdata = dm_pool_zalloc(fid->mem, sizeof(**vg_fmtdata)))) {
		log_error("Failed to allocate VG fmtdata for text format.");
		return NULL;
	}

	if (!(ivp = dm_pool_zalloc(fid->fmt->cmd->mem, sizeof(*ivp)))) {
		log_error("Failed to allocate import_vgsummary_params struct.");
		return NULL;
	}

	_init_text_import();

	ivp->fid = fid;
	ivp->when = when;
	*ivp->when = 0;
	ivp->desc = desc;
	*ivp->desc = NULL;
	ivp->single_device = single_device;
	ivp->use_previous_vg = use_previous_vg;
	ivp->checksum = checksum;
	ivp->total_size = size + size2;
	ivp->vg_fmtdata = vg_fmtdata;

	if (!(ivp->cft = config_open(CONFIG_FILE_SPECIAL, file, 0)))
		return_NULL;

	/* Does the metadata match the already-cached VG? */
	ivp->skip_parse = vg_fmtdata && 
			  ((*vg_fmtdata)->cached_mda_checksum == checksum) &&
			  ((*vg_fmtdata)->cached_mda_size == ivp->total_size);

	if (!dev && !config_file_read(fid->mem, ivp->cft)) {
		config_destroy(ivp->cft);
		return_NULL;
	}

	if (dev && !config_file_read_fd(fid->mem, ivp->cft, dev, MDA_CONTENT_REASON(primary_mda), offset, size,
					offset2, size2, checksum_fn, checksum,
					ivp->skip_parse, 1)) {
		config_destroy(ivp->cft);
		return_NULL;
	}

	_import_vg(ivp);

	return ivp->vg;
}

struct volume_group *text_vg_import_file(struct format_instance *fid,
					 const char *file,
					 time_t *when, char **desc)
{
	return text_vg_import_fd(fid, file, NULL, NULL, 0, NULL, 0, (off_t)0, 0, (off_t)0, 0, NULL, 0,
				 when, desc);
}

static struct volume_group *_import_vg_from_config_tree(const struct dm_config_tree *cft,
							struct format_instance *fid,
							unsigned allow_lvmetad_extensions)
{
	struct volume_group *vg = NULL;
	struct text_vg_version_ops **vsn;
	int vg_missing;

	_init_text_import();

	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;
		/*
		 * The only path to this point uses cached vgmetadata,
		 * so it can use cached PV state too.
		 */
		if (!(vg = (*vsn)->read_vg(fid, cft, 1, allow_lvmetad_extensions)))
			stack;
		else if ((vg_missing = vg_missing_pv_count(vg))) {
			log_verbose("There are %d physical volumes missing.",
				    vg_missing);
			vg_mark_partial_lvs(vg, 1);
			/* FIXME: move this code inside read_vg() */
		}
		break;
	}

	return vg;
}

struct volume_group *import_vg_from_config_tree(const struct dm_config_tree *cft,
						struct format_instance *fid)
{
	return _import_vg_from_config_tree(cft, fid, 0);
}

struct volume_group *import_vg_from_lvmetad_config_tree(const struct dm_config_tree *cft,
							struct format_instance *fid)
{
	return _import_vg_from_config_tree(cft, fid, 1);
}
