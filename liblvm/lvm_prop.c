/*
 * Copyright (C) 2013 Red Hat, Inc. All rights reserved.
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

#include "lvm_prop.h"
#include "libdevmapper.h"
#include "lvm-types.h"
#include "metadata.h"

/* lv create parameters */
GET_LVCREATEPARAMS_NUM_PROPERTY_FN(skip_zero, lvcp->zero)
SET_LVCREATEPARAMS_NUM_PROPERTY_FN(skip_zero, lvcp->zero)

struct lvm_property_type _lib_properties[] = {
#include "lvm_prop_fields.h"
	{ 0, "", 0, 0, 0, { .integer = 0 }, prop_not_implemented_get,
			prop_not_implemented_set },
};

#undef STR
#undef NUM
#undef FIELD

int lv_create_param_get_property(const struct lvcreate_params *lvcp,
		struct lvm_property_type *prop)
{
	return prop_get_property(_lib_properties, lvcp, prop, LV_CREATE_PARAMS);
}

int lv_create_param_set_property(struct lvcreate_params *lvcp,
		    struct lvm_property_type *prop)
{
	return prop_set_property(_lib_properties, lvcp, prop, LV_CREATE_PARAMS);
}
