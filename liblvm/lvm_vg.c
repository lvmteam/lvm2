/*
 * Copyright (C) 2008,2009 Red Hat, Inc. All rights reserved.
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

#include <errno.h>
#include <string.h>

#include "lib.h"
#include "lvm.h"
#include "toolcontext.h"
#include "metadata-exported.h"
#include "archiver.h"
#include "locking.h"

vg_t *lvm_vg_create(lvm_t libh, const char *vg_name)
{
	vg_t *vg;

	vg = vg_create((struct cmd_context *)libh, vg_name);
	/* FIXME: error handling is still TBD */
	if (vg_read_error(vg)) {
		vg_release(vg);
		return NULL;
	}
	return vg;
}

int lvm_vg_extend(vg_t *vg, const char *device)
{
	if (vg_read_error(vg))
		goto_bad;

	return vg_extend(vg, 1, (char **) &device);
bad:
	return 0;
}

int lvm_vg_set_extent_size(vg_t *vg, uint32_t new_size)
{
	if (vg_read_error(vg))
		goto_bad;

	return vg_set_extent_size(vg, new_size);
bad:
	return 0;
}

int lvm_vg_write(vg_t *vg)
{
	if (vg_read_error(vg))
		goto_bad;

	if (!archive(vg)) {
		goto_bad;
	}

	/* Store VG on disk(s) */
	if (!vg_write(vg) || !vg_commit(vg)) {
		goto_bad;
	}
	return 1;
bad:
	return 0;
}

int lvm_vg_close(vg_t *vg)
{
	if (vg_read_error(vg) == FAILED_LOCKING)
		vg_release(vg);
	else
		unlock_and_release_vg(vg->cmd, vg, vg->name);
	return 1;
}

int lvm_vg_remove(vg_t *vg)
{
	if (vg_read_error(vg))
		goto_bad;

	return vg_remove_single(vg);
bad:
	return 0;
}

vg_t *lvm_vg_open(lvm_t libh, const char *vgname, const char *mode,
		  uint32_t flags)
{
	uint32_t internal_flags = 0;
	vg_t *vg;

	if (!strncmp(mode, "w", 1))
		internal_flags |= READ_FOR_UPDATE;
	else if (strncmp(mode, "r", 1)) {
		log_errno(EINVAL, "Invalid VG open mode");
		return NULL;
	}

	vg = vg_read((struct cmd_context *)libh, vgname, NULL, internal_flags);
	if (vg_read_error(vg)) {
		/* FIXME: use log_errno either here in inside vg_read */
		vg_release(vg);
		return NULL;
	}

	return vg;
}
