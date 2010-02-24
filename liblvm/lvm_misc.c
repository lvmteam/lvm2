/*
 * Copyright (C) 2008,2010 Red Hat, Inc. All rights reserved.
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

#include "lvm_misc.h"

struct dm_list *tag_list_copy(struct dm_pool *p, struct dm_list *tag_list)
{
	struct dm_list *list;
	lvm_str_list_t *lsl;
	struct str_list *sl;

	if (!(list = dm_pool_zalloc(p, sizeof(*list)))) {
		log_errno(ENOMEM, "Memory allocation fail for dm_list.");
		return NULL;
	}
	dm_list_init(list);

	dm_list_iterate_items(sl, tag_list) {
		if (!(lsl = dm_pool_zalloc(p, sizeof(*lsl)))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_lv_list.");
			return NULL;
		}
		if (!(lsl->str = dm_pool_strdup(p, sl->str))) {
			log_errno(ENOMEM,
				"Memory allocation fail for lvm_lv_list->str.");
			return NULL;
		}
		dm_list_add(list, &lsl->list);
	}
	return list;
}
