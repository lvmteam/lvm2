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

#include "lib.h"
#include "properties.h"
#include "lvm_misc.h"
#include "lvm2app.h"
#include "lvm_prop.h"

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

struct lvm_property_value get_property(const pv_t pv, const vg_t vg,
				       const lv_t lv,
				       const lvseg_t lvseg,
				       const pvseg_t pvseg,
				       const struct lvcreate_params *lvcp,
				       const char *name)
{
	struct lvm_property_type prop;
	struct lvm_property_value v = { 0 };

	prop.id = name;

	if (pv) {
		if (!pv_get_property(pv, &prop))
			return v;
	} else if (vg) {
		if (!vg_get_property(vg, &prop))
			return v;
	} else if (lv) {
		if (!lv_get_property(lv, &prop))
			return v;
	} else if (lvseg) {
		if (!lvseg_get_property(lvseg, &prop))
			return v;
	} else if (pvseg) {
		if (!pvseg_get_property(pvseg, &prop))
			return v;
	} else if (lvcp) {
		if (!lv_create_param_get_property(lvcp, &prop))
			return v;
	} else {
		log_errno(EINVAL, "Invalid NULL handle passed to library function.");
		return v;
	}

	v.is_settable = prop.is_settable;
	v.is_string = prop.is_string;
	v.is_integer = prop.is_integer;
	if (v.is_string)
		v.value.string = prop.value.string;
	if (v.is_integer)
		v.value.integer = prop.value.integer;
	v.is_valid = 1;
	return v;
}


int set_property(const pv_t pv, const vg_t vg, const lv_t lv,
		struct lvcreate_params *lvcp,
		const char *name,
		struct lvm_property_value *v)
{
	struct lvm_property_type prop;

	prop.id = name;
	if (v->is_string)
		prop.value.string = v->value.string;
	else
		prop.value.integer = v->value.integer;
	if (pv) {
		if (!pv_set_property(pv, &prop)) {
			v->is_valid = 0;
			return -1;
		}
	} else if (vg) {
		if (!vg_set_property(vg, &prop)) {
			v->is_valid = 0;
			return -1;
		}
	} else if (lv) {
		if (!lv_set_property(lv, &prop)) {
			v->is_valid = 0;
			return -1;
		}
	} else if (lvcp) {
		if (!lv_create_param_set_property(lvcp, &prop)) {
			v->is_valid = 0;
			return -1;
		}
	} else {
		return -1;
	}
	return 0;
}
