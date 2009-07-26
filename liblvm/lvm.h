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

/******************************** structures ********************************/

/* Internal object structures - do not use directly */
struct lvm;
struct physical_volume;
struct volume_group;
struct logical_volume;

/**
 * lvm handle.
 *
 * This is the base handle that is needed to open and create objects. Also
 * error handling is bound to this handle.
 */
typedef struct lvm *lvm_t;

/**
 * Physical volume object.
 *
 * This object can be either a read-only object or a read-write object
 * depending on the mode it was returned by a function. This object can not be
 * written to disk independently, it is bound to a volume group and changes
 * will be written to disk when the volume group gets committed to disk. The
 * open mode is the same as the volume group object is was created of.
 */
typedef struct physical_volume pv_t;

/**
 * Volume group object.
 *
 * This object can be either a read-only object or a read-write object
 * depending on the mode it was returned by a function. Create functions
 * return a read-write object, but open functions have the argument mode to
 * define if the object can be modified or not.
 */
typedef struct volume_group vg_t;

/**
 * Logical Volume object.
 *
 * This object can be either a read-only object or a read-write object
 * depending on the mode it was returned by a function. This object can not be
 * written to disk independently, it is bound to a volume group and changes
 * will be written to disk when the volume group gets committed to disk. The
 * open mode is the same as the volume group object is was created of.
 */
typedef struct logical_volume lv_t;

/**
 * Physical volume object list.
 *
 * The properties of physical volume objects also applies to the list of
 * physical volumes.
 */
typedef struct lvm_pv_list {
	struct dm_list list;
	pv_t *pv;
} pv_list_t;

/**
 * Volume group object list.
 *
 * The properties of volume group objects also applies to the list of
 * volume groups.
 */
typedef struct lvm_vg_list {
	struct dm_list list;
	vg_t *vg;
} vg_list_t;

/**
 * Logical Volume object list.
 *
 * The properties of logical volume objects also applies to the list of
 * logical volumes.
 */
typedef struct lvm_lv_list {
	struct dm_list list;
	lv_t *lv;
} lv_list_t;

/**
 * String list.
 *
 * This string list contains read-only strings.
 */
struct lvm_str_list {
	struct dm_list list;
	const char *str;
};

/*************************** generic lvm handling ***************************/

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
 * This function should be used when any system configuration changes,
 * and the change is needed by the application.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 * \return  0 (success) or -1 (failure).
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
 * Scan all devices on the system for VGs and LVM metadata.
 *
 * \return  0 (success) or -1 (failure).
 */
int lvm_scan(lvm_t libh);

/*************************** volume group handling **************************/

/**
 * Return the list of volume group names.
 *
 * NOTE: This function will _NOT_ scan devices in the system for LVM metadata.
 * To scan the system, use lvm_scan.
 *
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
 *
 * FIXME: handle list memory cleanup
 */
struct dm_list *lvm_list_vg_names(lvm_t libh);

/**
 * Return the list of volume group uuids.
 *
 * NOTE: This function will _NOT_ scan devices in the system for LVM metadata.
 * To scan the system, use lvm_scan.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 *
 * \return  List of copied uuid strings.
 *          If no VGs exist on the system, NULL is returned.
 */
struct dm_list *lvm_list_vg_uuids(lvm_t libh);

/**
 * Open an existing VG.
 *
 * Open a VG for reading or writing.
 *
 * \param   libh
 *          Handle obtained from lvm_create.
 * \param   vgname
 *          Name of the VG to open.
 * \param   mode
 *          Open mode - either "r" (read) or "w" (read/write).
 *          Any other character results in an error with EINVAL set.
 * \param   flags
 *          Open flags - currently ignored.
 * \return  non-NULL VG handle (success) or NULL (failure).
 */
vg_t *lvm_vg_open(lvm_t libh, const char *vgname, const char *mode,
		  uint32_t flags);

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
 * \param   vg_name
 *          Name of the VG to open.
 * \return  non-NULL vg handle (success) or NULL (failure)
 */
vg_t *lvm_vg_create(lvm_t libh, const char *vg_name);

 /**
 * Write a VG to disk.
 *
 * This API commits the VG to disk.
 * Upon failure, retry the operation and/or release the VG handle with
 * lvm_vg_close.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  0 (success) or -1 (failure).
 */
int lvm_vg_write(vg_t *vg);

/**
 * Remove a VG from the system.
 *
 * This API commits the change to disk and does not require calling
 * lvm_vg_write.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  0 (success) or -1 (failure).
 */
int lvm_vg_remove(vg_t *vg);

/**
 * Close a VG opened with lvm_vg_create
 *
 * This API releases a VG handle and any resources associated with the handle.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  0 (success) or -1 (failure).
 */
int lvm_vg_close(vg_t *vg);

/**
 * Extend a VG by adding a device.
 *
 * This API requires calling lvm_vg_write to commit the change to disk.
 * After successfully adding a device, use lvm_vg_write to commit the new VG
 * to disk.  Upon failure, retry the operation or release the VG handle with
 * lvm_vg_close.
 * If the device is not initialized for LVM use, it will be initialized
 * before adding to the VG.  Although some internal checks are done,
 * the caller should be sure the device is not in use by other subsystems
 * before calling lvm_vg_extend.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \param   device
 *          Name of device to add to VG.
 * \return  0 (success) or -1 (failure).
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
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \param   new_size
 *          New extent size in bytes.
 * \return  0 (success) or -1 (failure).
 */
int lvm_vg_set_extent_size(vg_t *vg, uint32_t new_size);

/**
 * Get the current name of a volume group.
 *
 * Memory is allocated using malloc() and caller must free the memory
 * using free().
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Copy of the uuid string.
 */
char *lvm_vg_get_uuid(const vg_t *vg);

/**
 * Get the current uuid of a volume group.
 *
 * Memory is allocated using malloc() and caller must free the memory
 * using free().
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Copy of the name.
 */
char *lvm_vg_get_name(const vg_t *vg);

/**
 * Get the current size in bytes of a volume group.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Size in bytes.
 */
uint64_t lvm_vg_get_size(const vg_t *vg);

/**
 * Get the current unallocated space in bytes of a volume group.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Free size in bytes.
 */
uint64_t lvm_vg_get_free_size(const vg_t *vg);

/**
 * Get the current extent size in bytes of a volume group.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Extent size in bytes.
 */
uint64_t lvm_vg_get_extent_size(const vg_t *vg);

/**
 * Get the current number of total extents of a volume group.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Extent count.
 */
uint64_t lvm_vg_get_extent_count(const vg_t *vg);

/**
 * Get the current number of free extents of a volume group.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Free extent count.
 */
uint64_t lvm_vg_get_free_extent_count(const vg_t *vg);

/**
 * Get the current number of physical volumes of a volume group.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  Physical volume count.
 */
uint64_t lvm_vg_get_pv_count(const vg_t *vg);

/************************** logical volume handling *************************/

/**
 * Return a list of LV handles for a given VG handle.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  A list of lv_list_t structures containing lv handles for this vg.
 *          If no LVs exist on the given VG, NULL is returned.
 */
struct dm_list *lvm_vg_list_lvs(vg_t *vg);

/**
 * Create a linear logical volume.
 * This API commits the change to disk and does _not_ require calling
 * lvm_vg_write.
 * FIXME: This API should probably not commit to disk but require calling
 * lvm_vg_write.  However, this appears to be non-trivial change until
 * lv_create_single is refactored by segtype.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \param   name
 *          Name of logical volume to create.
 * \param   size
 *          Size of logical volume in extents.
 * \return  LV object
 *
 */
lv_t *lvm_vg_create_lv_linear(vg_t *vg, const char *name, uint64_t size);

/**
 * Activate a logical volume.
 *
 * This API is the equivalent of the lvm command "lvchange -ay".
 *
 * NOTE: This API cannot currently handle LVs with an in-progress pvmove or
 * lvconvert.
 *
 * \param   lv
 *          Logical volume handle.
 * \return  0 (success) or -1 (failure).
 */
int lvm_lv_activate(lv_t *lv);

/**
 * Deactivate a logical volume.
 *
 * This API is the equivalent of the lvm command "lvchange -an".
 *
 * \param   lv
 *          Logical volume handle.
 * \return  0 (success) or -1 (failure).
 */
int lvm_lv_deactivate(lv_t *lv);

/**
 * Remove a logical volume from a volume group.
 *
 * This function commits the change to disk and does _not_ require calling
 * lvm_vg_write.
 * Currently only removing linear LVs are possible.
 *
 * FIXME: This API should probably not commit to disk but require calling
 * lvm_vg_write.
 *
 * \param   lv
 *          Logical volume handle.
 * \return  0 (success) or -1 (failure).
 */
int lvm_vg_remove_lv(lv_t *lv);

/**
 * Get the current name of a logical volume.
 *
 * Memory is allocated using malloc() and caller must free the memory
 * using free().
 *
 * \param   lv
 *          Logical volume handle.
 * \return  Copy of the uuid string.
 */
char *lvm_lv_get_uuid(const lv_t *lv);

/**
 * Get the current uuid of a logical volume.
 *
 * Memory is allocated using malloc() and caller must free the memory
 * using free().
 *
 * \param   lv
 *          Logical volume handle.
 * \return  Copy of the name.
 */
char *lvm_lv_get_name(const lv_t *lv);

/**
 * Get the current size in bytes of a logical volume.
 *
 * \param   lv
 *          Logical volume handle.
 * \return  Size in bytes.
 */
uint64_t lvm_lv_get_size(const lv_t *lv);

/************************** physical volume handling ************************/

/**
 * Physical volume handling should not be needed anymore. Only physical volumes
 * bound to a vg contain useful information. Therefore the creation,
 * modification and the removal of orphan physical volumes is not suported.
 */

/**
 * Return a list of PV handles for a given VG handle.
 *
 * \param   vg
 *          VG handle obtained from lvm_vg_create or lvm_vg_open.
 * \return  A list of pv_list_t structures containing pv handles for this vg.
 *          If no PVs exist on the given VG, NULL is returned.
 */
struct dm_list *lvm_vg_list_pvs(vg_t *vg);

/**
 * Get the current uuid of a logical volume.
 *
 * Memory is allocated using malloc() and caller must free the memory
 * using free().
 *
 * \param   pv
 *          Physical volume handle.
 * \return  Copy of the uuid string.
 */
char *lvm_pv_get_uuid(const pv_t *pv);

/**
 * Get the current name of a logical volume.
 *
 * Memory is allocated using malloc() and caller must free the memory
 * using free().
 *
 * \param   pv
 *          Physical volume handle.
 * \return  Copy of the name.
 */
char *lvm_pv_get_name(const pv_t *pv);

/**
 * Get the current number of metadata areas in the physical volume.
 *
 * \param   pv
 *          Physical volume handle.
 * \return  metadata area count.
 */
uint64_t lvm_pv_get_mda_count(const pv_t *pv);

#endif /* _LIB_LVM_H */
