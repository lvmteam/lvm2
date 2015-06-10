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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "metadata.h"
#include "import-export.h"

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

/*
 * Find out vgname on a given device.
 */
int text_vgname_import(const struct format_type *fmt,
		       struct device *dev,
		       off_t offset, uint32_t size,
		       off_t offset2, uint32_t size2,
		       checksum_fn_t checksum_fn,
		       int checksum_only,
		       struct lvmcache_vgsummary *vgsummary)
{
	struct dm_config_tree *cft;
	struct text_vg_version_ops **vsn;
	int r = 0;

	_init_text_import();

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, NULL, 0)))
		return_0;

	if ((!dev && !config_file_read(cft)) ||
	    (dev && !config_file_read_fd(cft, dev, offset, size,
					 offset2, size2, checksum_fn,
					 vgsummary->mda_checksum,
					 checksum_only))) {
		log_error("Couldn't read volume group metadata.");
		goto out;
	}

	if (checksum_only) {
		/* Checksum matches already-cached content - no need to reparse. */
		r = 1;
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(*vsn)->read_vgname(fmt, cft, vgsummary))
			goto_out;

		r = 1;
		break;
	}

      out:
	config_destroy(cft);
	return r;
}

struct cached_vg_fmtdata {
        uint32_t cached_mda_checksum;
        size_t cached_mda_size;
};

struct volume_group *text_vg_import_fd(struct format_instance *fid,
				       const char *file,
				       struct cached_vg_fmtdata **vg_fmtdata,
				       unsigned *use_previous_vg,
				       int single_device,
				       struct device *dev,
				       off_t offset, uint32_t size,
				       off_t offset2, uint32_t size2,
				       checksum_fn_t checksum_fn,
				       uint32_t checksum,
				       time_t *when, char **desc)
{
	struct volume_group *vg = NULL;
	struct dm_config_tree *cft;
	struct text_vg_version_ops **vsn;
	int skip_parse;

	if (vg_fmtdata && !*vg_fmtdata &&
	    !(*vg_fmtdata = dm_pool_zalloc(fid->mem, sizeof(**vg_fmtdata)))) {
		log_error("Failed to allocate VG fmtdata for text format.");
		return NULL;
	}

	_init_text_import();

	*desc = NULL;
	*when = 0;

	if (!(cft = config_open(CONFIG_FILE_SPECIAL, file, 0)))
		return_NULL;

	/* Does the metadata match the already-cached VG? */
	skip_parse = vg_fmtdata && 
		     ((*vg_fmtdata)->cached_mda_checksum == checksum) &&
		     ((*vg_fmtdata)->cached_mda_size == (size + size2));

	if ((!dev && !config_file_read(cft)) ||
	    (dev && !config_file_read_fd(cft, dev, offset, size,
					 offset2, size2, checksum_fn, checksum,
					 skip_parse)))
		goto_out;

	if (skip_parse) {
		if (use_previous_vg)
			*use_previous_vg = 1;
		goto out;
	}

	/*
	 * Find a set of version functions that can read this file
	 */
	for (vsn = &_text_vsn_list[0]; *vsn; vsn++) {
		if (!(*vsn)->check_version(cft))
			continue;

		if (!(vg = (*vsn)->read_vg(fid, cft, single_device, 0)))
			goto_out;

		(*vsn)->read_desc(vg->vgmem, cft, when, desc);
		break;
	}

	if (vg && vg_fmtdata && *vg_fmtdata) {
		(*vg_fmtdata)->cached_mda_size = (size + size2);
		(*vg_fmtdata)->cached_mda_checksum = checksum;
	}

	if (use_previous_vg)
		*use_previous_vg = 0;

      out:
	config_destroy(cft);
	return vg;
}

struct volume_group *text_vg_import_file(struct format_instance *fid,
					 const char *file,
					 time_t *when, char **desc)
{
	return text_vg_import_fd(fid, file, NULL, NULL, 0, NULL, (off_t)0, 0, (off_t)0, 0, NULL, 0,
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
