/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

#include "tools.h"
#include "lib/format_text/format-text.h"

/*
 * TODO: we cannot yet repair corruption in label_header, pv_header/locations,
 * or corruption of some mda_header fields.
 */

static int _update_metadata_single(struct cmd_context *cmd __attribute__((unused)),
		       const char *vg_name,
		       struct volume_group *vg,
		       struct processing_handle *handle __attribute__((unused)))
{

	/*
	 * Simply calling vg_write can correct or clean up various things:
	 * . some mda's have old versions of metdadata 
	 * . wipe outdated PVs
	 * . fix pv_header used flag and version
	 * . strip historical lvs
	 * . clear missing pv flag on unused PV
	 */
	if (!vg_write(vg)) {
		log_error("Failed to write VG.");
		return 0;
	}

	/*
	 * Prevent vg_commit from freeing the metadata
	 * buffer that vg_write wrote to disk so that
	 * vg_write_commit_bad_mdas() can use the same
	 * metadata buffer to write to the bad mdas.
	 */
	preserve_text_fidtc(vg);

	if (!vg_commit(vg)) {
		log_error("Failed to commit VG.");
		return 0;
	}

	/*
	 * vg_write does not write to "bad" mdas (where "bad" is corrupt, can't
	 * be processed when reading).  bad mdas are not kept in
	 * fid->metadata_areas_in_use so vg_read and vg_write ignore them, but
	 * they are saved in lvmcache.  this gets them from lvmcache and tries
	 * to write this metadata to them.
	 */
	vg_write_commit_bad_mdas(cmd, vg);

	/*
	 * Now free the metadata buffer that was
	 * preserved above.
	 */
	free_text_fidtc(vg);

	return 1;
}

static int _update_metadata(struct cmd_context *cmd, int argc, char **argv)
{
	cmd->handles_missing_pvs = 1;
	cmd->wipe_outdated_pvs = 1;
	cmd->handles_unknown_segments = 1;

	return process_each_vg(cmd, argc, argv, NULL, NULL, READ_FOR_UPDATE, 0, NULL,
			       &_update_metadata_single);
}

static int vgck_single(struct cmd_context *cmd __attribute__((unused)),
		       const char *vg_name,
		       struct volume_group *vg,
		       struct processing_handle *handle __attribute__((unused)))
{
	if (!vg_validate(vg))
		return_ECMD_FAILED;

	if (vg_missing_pv_count(vg)) {
		log_error("The volume group is missing %d physical volumes.",
			  vg_missing_pv_count(vg));
		return ECMD_FAILED;
	}

	return ECMD_PROCESSED;
}

int vgck(struct cmd_context *cmd, int argc, char **argv)
{
	if (arg_is_set(cmd, updatemetadata_ARG))
		return _update_metadata(cmd, argc, argv);

	return process_each_vg(cmd, argc, argv, NULL, NULL, 0, 0, NULL,
			       &vgck_single);
}
