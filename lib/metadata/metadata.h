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

#define ACTIVE               	0x00000001  /* PV VG LV */
#define EXPORTED_VG          	0x00000002  /* VG */  /* And PV too perhaps? */
#define EXTENDABLE_VG        	0x00000004  /* VG */
#define ALLOCATED_PV         	0x00000008  /* PV */

#define SPINDOWN_LV          	0x00000010  /* LV */
#define BADBLOCK_ON       	0x00000020  /* LV */

#define LVM_READ              	0x00000100  /* LV VG */
#define LVM_WRITE             	0x00000200  /* LV VG */
#define CLUSTERED         	0x00000400  /* VG */
#define SHARED            	0x00000800  /* VG */

#define ALLOC_STRICT      	0x00001000  /* LV */
#define ALLOC_CONTIGUOUS  	0x00002000  /* LV */
#define SNAPSHOT          	0x00004000  /* LV */
#define SNAPSHOT_ORG      	0x00008000  /* LV */


#define EXPORTED_TAG "PV_EXP"  /* Identifier of exported PV */
#define IMPORTED_TAG "PV_IMP"  /* Identifier of imported PV */


struct id {
	uint8_t uuid[ID_LEN];
};

struct physical_volume {
        struct id id;
	struct device *dev;
	char *vg_name;		/* VG component of name only - not full path */
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
	struct id id;
        char *name;		/* LV component of name only - not full path */

        uint32_t status;
        uint32_t open;

        uint64_t size;
        uint32_t le_count;

        /* le -> pe mapping array */
        struct pe_specifier *map;
};

struct volume_group {
	struct id id;
	char *name;		/* VG component of name only - not full path */

        uint32_t status;

        uint64_t extent_size;
        uint32_t extent_count;
        uint32_t free_count;

        uint32_t max_lv;
        uint32_t max_pv;

        /* physical volumes */
        uint32_t pv_count;
	struct list_head pvs;

        /* logical volumes */
        uint32_t lv_count;
	struct list_head lvs;
};

struct name_list {
	struct list_head list;
	char *name;
};

struct pv_list {
	struct list_head list;
	struct physical_volume pv;
};

struct lv_list {
	struct list_head list;
	struct logical_volume lv;
};

/* ownership of returned objects passes */
struct io_space {
	/* Returns list of names of all vgs - vg
           component only, not full path*/
	struct list_head *(*get_vgs)(struct io_space *is);

	/* Returns list of fully-populated pv_list
           structures */
	struct list_head *(*get_pvs)(struct io_space *is);

	/* Return PV with given name (may be full
           or relative path) */
	struct physical_volume *(*pv_read)(struct io_space *is,
					   struct device *dev);

	/* Write a PV structure to disk. */
	/* Fails if the PV is in a VG ie
           pv->vg_name must be null */
	int (*pv_write)(struct io_space *is, struct physical_volume *pv);

	/* if vg_name doesn't contain any slash, this function adds prefix */
	struct volume_group *(*vg_read)(struct io_space *is,
					const char *vg_name);

	/* Write out complete VG metadata. */
	/* Ensure *internal* consistency before writing anything.
	 *   eg. PEs can't refer to PVs not part of the VG
	 * Order write sequence to aid recovery if process is aborted
	 *   (eg flush entire set of changes to each disk in turn)
	 * It is the responsibility of the caller to ensure external
 	 * consistency, eg by calling pv_write() if removing PVs from a VG
	 * or calling vg_write() a second time if splitting a VG into two.
	 * vg_write() must not read or write from any PVs not included
	 * in the volume_group structure it is handed.
	 */
	int (*vg_write)(struct io_space *is, struct volume_group *vg);

	void (*destroy)(struct io_space *is);

	/* Current volume group prefix. */
	/* Default to "/dev/" */
	char *prefix;
	struct pool *mem;
	struct dev_filter *filter;
	void *private;
};

/* FIXME: Move to other files */
struct io_space *create_text_format(struct dev_filter *filter,
				    const char *text_file);
struct io_space *create_lvm_v1_format(struct dev_filter *filter);

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

/* ? Return the VG that contains a given LV (based on path given in lv_name) */
/* (or environment var?) */
struct volume_group *vg_find(const char *lv_name);

/* Find an LV within a given VG */
struct logical_volume *lv_find(struct volume_group *vg, const char *lv_name);

#endif
