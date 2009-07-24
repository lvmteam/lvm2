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
#include "libdevmapper.h"

#include <stdint.h>

/* Internal object structures */
struct volume_group;
struct physical_volume;
struct logical_volume;

/* liblvm handles to objects pv, vg, lv, pvseg, lvseg */
typedef struct volume_group vg_t;
typedef struct physical_volume pv_t;
typedef struct logical_volume lv_t;

typedef struct lvm_vg_list {
	struct dm_list list;
	vg_t *vg;
} vg_list_t;

typedef struct lvm_pv_list {
	struct dm_list list;
	pv_t *pv;
} pv_list_t;

typedef struct lvm_lv_list {
	struct dm_list list;
	lv_t *lv;
} lv_list_t;

struct lvm_str_list {
	struct dm_list list;
	const char *str;
};

/**
 * Return a list of LV handles for a given VG handle.
 *
 * \return  A list of lv_list_t structures containing lv handles for this vg.
 *          If no LVs exist on the given VG, NULL is returned.
 */
struct dm_list *lvm_vg_list_lvs(vg_t *vg);

struct lvm; /* internal data */

/**
 * The lvm handle.
 */
typedef struct lvm *lvm_t;

/**
 * Create a LVM handle.
 *
 * Once all LVM operations have been completed, use lvm_destroy to release
 * the handle and any associated resources.
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

/**
 * Return stored error no describing last LVM API error.
 *
 * Users of liblvm should use lvm_errno to determine success or failure
 * of the last call, unless the API indicates another method of determining
 * success or failure.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 *
 * \return  An errno value describing the last LVM error.
 */
int lvm_errno(lvm_t libh);

/**
 * Return stored error message describing last LVM error.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 *
 * \return  An error string describing the last LVM error.
 */
const char *lvm_errmsg(lvm_t libh);

/**
 * Create a VG with default parameters.
 *
 * This API requires calling lvm_vg_write to commit the change to disk.
 * Upon success, other APIs may be used to set non-default parameters.
 * For example, to set a non-default extent size, use lvm_vg_set_extent_size.
 * Next, to add physical storage devices to the volume group, use
 * lvm_vg_extend for each device.
 * Once all parameters are set appropriately and all devices are added to the
 * VG, use lvm_vg_write to commit the new VG to disk, and lvm_vg_close to
 * release the VG handle.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 *
 * \return  non-NULL vg handle (success) or NULL (failure)
 */
vg_t *lvm_vg_create(lvm_t libh, const char *vg_name);

/**
 * Extend a VG by adding a device.
 *
 * This API requires calling lvm_vg_write to commit the change to disk.
 * After successfully adding a device, use lvm_vg_write to commit the new VG
 * to disk.  Upon failure, retry the operation or release the VG handle with
 * lvm_vg_close.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create.
 *
 * \param   device
 *          Name of device to add to VG.
 *
 * \return  Status code of 1 (success) or 0 (failure).
 */
int lvm_vg_extend(vg_t *vg, const char *device);

/**
 * Set the extent size of a VG.
 *
 * This API requires calling lvm_vg_write to commit the change to disk.
 * After successfully setting a new extent size, use lvm_vg_write to commit
 * the new VG to disk.  Upon failure, retry the operation or release the VG
 * handle with lvm_vg_close.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create.
 *
 * \param   new_size
 *          New extent size to set (in sectors).
 *
 * \return  Status code of 1 (success) or 0 (failure).
 */
int lvm_vg_set_extent_size(vg_t *vg, uint32_t new_size);

/**
 * Write a VG to disk.
 *
 * This API commits the VG to disk.
 * Upon failure, retry the operation and/or release the VG handle with
 * lvm_vg_close.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create.
 *
 * \return  Status code of 1 (success) or 0 (failure).
 */
int lvm_vg_write(vg_t *vg);

/**
 * Remove a VG from the system.
 *
 * This API commits the change to disk and does not require calling
 * lvm_vg_write.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create.
 *
 * \return  Status code of 1 (success) or 0 (failure).
 */
int lvm_vg_remove(vg_t *vg);

/**
 * Get the current name or uuid of a PV, VG, or LV.
 *
 * Returns a copy of the current name or uuid for the given PV,
 * VG, or LV.
 *
 * Memory is allocated using malloc() and caller must free the memory
 * using free().
 */
char *lvm_pv_get_uuid(const pv_t *pv);
char *lvm_vg_get_uuid(const vg_t *vg);
char *lvm_lv_get_uuid(const lv_t *lv);
char *lvm_pv_get_name(const pv_t *pv);
char *lvm_vg_get_name(const vg_t *vg);
char *lvm_lv_get_name(const lv_t *lv);

/**
 * Close a VG opened with lvm_vg_create
 *
 * This API releases a VG handle and any resources associated with the handle.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create.
 *
 * \return  Status code of 1 (success) or 0 (failure).
 */
int lvm_vg_close(vg_t *vg);

/**
 * Open an existing VG.
 *
 * Open a VG for reading or writing.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 *
 * \param   vgname
 *          Name of the VG to open.
 *
 * \param   mode
 *          Open mode - either "r" (read) or "w" (read/write).
 *          Any other character results in an error with EINVAL set.
 *
 * \param   flags
 *          Open flags - currently ignored.
 *
 * \return  non-NULL VG handle (success) or NULL (failure).
 */
vg_t *lvm_vg_open(lvm_t libh, const char *vgname, const char *mode,
		  uint32_t flags);

/**
 * Return a list of PV handles for a given VG handle.
 *
 * \return  A list of pv_list_t structures containing pv handles for this vg.
 *          If no PVs exist on the given VG, NULL is returned.
 */
struct dm_list *lvm_vg_list_pvs(vg_t *vg);

/**
 * Return a list of VG names or VG uuids in the system.
 *
 * NOTE: This function will _NOT_ scan devices in the system for LVM metadata.
 * To process the list, use the dm_list iterator functions.  For example:
 *      vg_t *vg;
 *      struct dm_list *vgnames;
 *      struct lvm_str_list *strl;
 *
 *      vgnames = lvm_list_vg_names(libh);
 *	dm_list_iterate_items(strl, vgnames) {
 *		vgname = strl->str;
 *              vg = lvm_vg_open(libh, vgname, "r");
 *              // do something with vg
 *              lvm_vg_close(vg);
 *      }
 *
 *
 * \return  A list of struct lvm_str_list
 *          If no VGs exist on the system, NULL is returned.
 * FIXME: handle list memory cleanup
 */
struct dm_list *lvm_list_vg_names(lvm_t libh);
struct dm_list *lvm_list_vg_ids(lvm_t libh);


#endif /* _LIB_LVM_H */
