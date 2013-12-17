/*
 * Copyright (C) 2008-2013 Red Hat, Inc. All rights reserved.
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
#ifndef _LVM2APP_MISC_H
#define _LVM2APP_MISC_H

#include "libdevmapper.h"
#include "lvm2app.h"
#include "metadata-exported.h"
#include "toolcontext.h"

#include <sys/types.h>
#include <sys/stat.h>

struct saved_env
{
	mode_t user_umask;
};

struct saved_env store_user_env(struct cmd_context *cmd);
void restore_user_env(const struct saved_env *env);

struct dm_list *tag_list_copy(struct dm_pool *p, struct dm_list *tag_list);
struct lvm_property_value get_property(const pv_t pv, const vg_t vg,
				       const lv_t lv, const lvseg_t lvseg,
				       const pvseg_t pvseg,
				       const struct lvcreate_params *lvcp,
				       const struct pvcreate_params *pvcp,
				       const char *name);
int set_property(const pv_t pv, const vg_t vg, const lv_t lv,
			struct lvcreate_params *lvcp,
			struct pvcreate_params *pvcp,
			const char *name,
			struct lvm_property_value *value);

#endif
