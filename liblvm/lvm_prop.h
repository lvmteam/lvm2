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
#include "prop_common.h"

#ifndef _LIB_LVM_PROP_H
#define _LIB_LVM_PROP_H

typedef struct lvcreate_params type_lvcreate_params;

#define LV_CREATE_PARAMS 1

#define GET_LVCREATEPARAMS_NUM_PROPERTY_FN(NAME, VALUE)\
	GET_NUM_PROPERTY_FN(NAME, VALUE, lvcreate_params, lvcp)

#define SET_LVCREATEPARAMS_NUM_PROPERTY_FN(NAME, VALUE) \
	SET_NUM_PROPERTY(NAME, VALUE, lvcreate_params, lvcp)

int lv_create_param_get_property(const struct lvcreate_params *lvcp,
		struct lvm_property_type *prop);

int lv_create_param_set_property(struct lvcreate_params *lvcp,
		    struct lvm_property_type *prop);

#endif
