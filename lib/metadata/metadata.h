/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 *
 * This is the in core representation of a volume group and its
 * associated physical and logical volumes.
 */

#ifndef _LVM_METADATA_H
#define _LVM_METADATA_H

#include <sys/types.h>
#include "dev-cache.h"
#include "list.h"

#define ID_LEN 32
#define NAME_LEN 128

/* Various flags */
/* Note that the bits no longer necessarily correspond to LVM1 disk format */

/* Status bits */
#define ST_ACTIVE               0x01  /* PV VG LV */
#define ST_EXPORTED_VG          0x02  /* VG */  /* And PV too perhaps? */
#define ST_EXTENDABLE_VG        0x04  /* VG */
#define ST_ALLOCATED_PV         0x08  /* PV */
#define ST_SPINDOWN_LV          0x10  /* LV */

/* Access bits */
#define AC_READ              0x01  /* LV VG */
#define AC_WRITE             0x02  /* LV VG */
#define AC_CLUSTERED         0x04  /* VG */
#define AC_SHARED            0x08  /* VG */

/* LV Flags */
#define LV_ALLOC_STRICT      0x01  /* LV */
#define LV_ALLOC_CONTIGUOUS  0x02  /* LV */
#define LV_SNAPSHOT          0x04  /* LV */
#define LV_SNAPSHOT_ORG      0x08  /* LV */
#define LV_BADBLOCK_ON       0x10  /* LV */



#define EXPORTED_TAG "PV_EXP"  /* Identifier of exported PV */
#define IMPORTED_TAG "PV_IMP"  /* Identifier of imported PV */



struct id {
	uint8_t uuid[ID_LEN];
};

struct physical_volume {
        struct id *id;
	struct device *dev;
	char *vg_name;
	char *exported;

        uint32_t status;
        uint64_t size;

        /* physical extents */
        uint64_t pe_size;
        uint64_t pe_start;
        uint32_t pe_count;
        uint32_t pe_allocated;
};

struct pe_specifier {
        struct physical_volume *pv;
        uint32_t pe;
};

struct logical_volume {
        /* disk */
	struct id *id;
        char *name;

        uint32_t status;
        uint32_t access;
        uint32_t flags;
        uint32_t open;

        uint64_t size;
        uint32_t le_count;

        /* le -> pe mapping array */
        struct pe_specifier *map;
};

struct volume_group {
	struct id *id;
	char *name;

        uint32_t status;
        uint32_t access;

        uint64_t extent_size;
        uint32_t extent_count;
        uint32_t free_count;

        uint32_t max_lv;
        uint32_t max_pv;

        /* physical volumes */
        uint32_t pv_count;
        struct physical_volume **pv;

        /* logical volumes */
        uint32_t lv_count;
        struct logical_volume **lv;
};

struct name_list {
	struct list_head list;
	char * name;
};

struct pv_list {
	struct list_head list;
	struct physical_volume pv;
};

/* ownership of returned objects passes */
struct io_space {
	/* Returns list of names of all vgs */
	struct name_list *(*get_vgs)(struct io_space *is);

	/* Returns list of fully-populated pv structures */
	struct pv_list *(*get_pvs)(struct io_space *is);

	/* Return PV with given name (may be full or relative path) */
	struct physical_volume *(*pv_read)(struct io_space *is,
					const char *pv_name);

	/* Write a PV structure to disk. */
	/* Fails if the PV is in a VG ie vg_name filled on the disk or in *pv */
	int (*pv_write)(struct io_space *is, struct physical_volume *pv);

	/* vg_name may contain slash(es) - if not, this function adds prefix */
	/* Default prefix is '/dev/' but can be changed from config file? */
	/* (via a prefix_set() ?) */
	struct volume_group *(*vg_read)(struct io_space *is,
					const char *vg_name);

	/* Write out complete VG metadata. */
	/* Ensure (& impose?) consistency before writing anything. 
	 *   eg. PEs can't refer to PVs not part of the VG 
	 * Order write sequence to aid recovery if process is aborted 
	 *   (eg flush entire set of changes to each disk in turn?)
	 * If overwriting existing VG data, needs to check for any PVs 
	 * removed from the VG and update those PVs too. If those PVs 
	 * are no longer in use, blank out vg_name on them.  Otherwise 
	 * set vg_name to something temporary and unique - this must be 
	 * a vgsplit with another vg_write() about to follow (or set a new
	 * status flag?)
	 *    OR  Should all consistency checks on the *_write* 
	 * functions here be handled by a wrapper around them, so that they
	 * *are* capable of leaving the system in an unusable state? 
	 *    OR  Should vgsplit set flags to modify vg_write behaviour,
	 * even specifying the new vg_name to insert?
	 */
	int (*vg_write)(struct io_space *is, struct volume_group *vg);

	void (*destroy)(struct io_space *is);

	struct dev_filter *filter;
	void *private;
	/* Something here to allow repair tools & --force options to */
	/* set flags to override certain consistency checks */
	/*   eg. in _write functions to allow restoration of metadata */
	/*       & in _read functions to allow "gaps" and specify which of */
	/*       conflicting copies of metadata to use (device restriction?) */
};

/* FIXME: Move to other files */
struct io_space *create_text_format(struct dev_filter *filter,
				    const char *text_file);
struct io_space *create_lvm_v1_format(struct dev_filter *filter);

inline int write_backup(struct io_space *orig, struct io_space *text)
{

}


int id_eq(struct id *op1, struct id *op2);

/* Create consistent new empty structures, populated with defaults */
struct volume_group *vg_create();
struct physical_volume *pv_create();

int vg_destroy(struct volume_group *vg);

/* Manipulate PV structures */
int pv_add(struct volume_group *vg, struct physical_volume *pv);
int pv_remove(struct volume_group *vg, struct physical_volume *pv);
struct physical_volume *pv_find(struct volume_group *vg,
				const char *pv_name);

/* Add an LV to a given VG */
int lv_add(struct volume_group *vg, struct logical_volume *lv);

/* Remove an LV from a given VG */
int lv_remove(struct volume_group *vg, struct logical_volume *lv);

/* Return the VG that contains a given LV (based on path given in lv_name) */
/* (or environment var?) */
struct volume_group *vg_find(const char *lv_name);

/* Find an LV within a given VG */
struct logical_volume *lv_find(struct volume_group *vg, const char *lv_name);

#endif
