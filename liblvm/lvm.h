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
#ifndef _LIB_LVM_H
#define _LIB_LVM_H

#include "lvm-version.h"

#include <stdint.h>

/* Internal object structures */
struct volume_group;
struct physical_volume;
struct logical_volume;
struct lv_segment;
struct pv_segment;

/* liblvm handles to objects pv, vg, lv, pvseg, lvseg */
typedef struct volume_group vg_t;
typedef struct physical_volume pv_t;
typedef struct logical_volume lv_t;
typedef struct pv_segment pvseg_t;
typedef struct lv_segment lvseg_t;

struct lvm; /* internal data */

/**
 * The lvm handle.
 */
typedef struct lvm *lvm_t;

/**
 * Create a LVM handle.
 *
 * \param   system_dir
 *          Set an alternative LVM system directory. Use NULL to use the 
 *          default value. If the environment variable LVM_SYSTEM_DIR is set, 
 *          it will override any LVM system directory setting.
 * \return  A valid LVM handle is returned or NULL if there has been a
 *          memory allocation problem. You have to check if an error occured
 *          with the lvm_error function.
 */
lvm_t lvm_create(const char *system_dir);

/**
 * Destroy a LVM handle allocated with lvm_create.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 */
void lvm_destroy(lvm_t libh);

/**
 * Reload the original configuration from the system directory.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 */
int lvm_reload_config(lvm_t libh);


#endif /* _LIB_LVM_H */
