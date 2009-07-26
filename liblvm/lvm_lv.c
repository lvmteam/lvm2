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

#include "lib.h"
#include "lvm.h"
#include "metadata-exported.h"
#include "lvm-string.h"
#include "defaults.h"
#include "segtype.h"
#include <string.h>

/* FIXME: have lib/report/report.c _disp function call lv_size()? */
uint64_t lvm_lv_get_size(const lv_t *lv)
{
	return lv_size(lv);
}

char *lvm_lv_get_uuid(const lv_t *lv)
{
	char uuid[64] __attribute((aligned(8)));

	if (!id_write_format(&lv->lvid.id[1], uuid, sizeof(uuid))) {
		log_error("Internal error converting uuid");
		return NULL;
	}
	return strndup((const char *)uuid, 64);
}

char *lvm_lv_get_name(const lv_t *lv)
{
	char *name;

	name = malloc(NAME_LEN + 1);
	strncpy(name, (const char *)lv->name, NAME_LEN);
	name[NAME_LEN] = '\0';
	return name;
}

/* Set defaults for non-segment specific LV parameters */
static void _lv_set_default_params(struct lvcreate_params *lp,
				   vg_t *vg, const char *lvname,
				   uint64_t extents)
{
	lp->zero = 1;
	lp->major = -1;
	lp->minor = -1;
	lp->vg_name = vg->name;
	lp->lv_name = lvname; /* FIXME: check this for safety */
	lp->pvh = &vg->pvs;

	lp->extents = extents;
	lp->permission = LVM_READ | LVM_WRITE;
	lp->read_ahead = DM_READ_AHEAD_NONE;
	lp->alloc = ALLOC_INHERIT;
	lp->tag = NULL;
}

/* Set default for linear segment specific LV parameters */
static void _lv_set_default_linear_params(struct cmd_context *cmd,
					  struct lvcreate_params *lp)
{
	lp->segtype = get_segtype_from_string(cmd, "striped");
	lp->stripes = 1;
	lp->stripe_size = DEFAULT_STRIPESIZE * 2;
}

lv_t *lvm_vg_create_lv_linear(vg_t *vg, const char *name, uint64_t size)
{
	struct lvcreate_params lp;
	uint64_t extents;
	struct lv_list *lvl;

	/* FIXME: check for proper VG access */
	if (vg_read_error(vg))
		return NULL;
	memset(&lp, 0, sizeof(lp));
	extents = extents_from_size(vg->cmd, size, vg->extent_size);
	_lv_set_default_params(&lp, vg, name, extents);
	_lv_set_default_linear_params(vg->cmd, &lp);
	if (!lv_create_single(vg, &lp))
		return NULL;
	lvl = find_lv_in_vg(vg, name);
	if (!lvl)
		return NULL;
	return lvl->lv;
}

int lvm_vg_remove_lv(lv_t *lv)
{
	if (!lv || !lv->vg || vg_read_error(lv->vg))
		return -1;
	if (!lv_remove_single(lv->vg->cmd, lv, DONT_PROMPT))
		return -1;
	return 0;
}
