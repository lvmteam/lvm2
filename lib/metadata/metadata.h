/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
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
#include "uuid.h"

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

#define ALLOC_STRICT		0x00001000  /* LV */
#define ALLOC_CONTIGUOUS  	0x00002000  /* LV */
#define SNAPSHOT          	0x00004000  /* LV */
#define SNAPSHOT_ORG      	0x00008000  /* LV */


#define EXPORTED_TAG "PV_EXP"  /* Identifier of exported PV */
#define IMPORTED_TAG "PV_IMP"  /* Identifier of imported PV */


struct physical_volume {
        struct id id;
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

struct volume_group {
	struct id id;
	char *name;

        uint32_t status;

        uint32_t extent_size;
        uint32_t extent_count;
        uint32_t free_count;

        uint32_t max_lv;
        uint32_t max_pv;

        /* physical volumes */
        uint32_t pv_count;
	struct list pvs;

        /* logical volumes */
        uint32_t lv_count;
	struct list lvs;
};

struct logical_volume {
        /* disk */
	struct id id;
        char *name;

	struct volume_group *vg;

        uint32_t status;
	uint32_t read_ahead;
	uint32_t stripes;

        uint64_t size;
        uint32_t le_count;

        /* le -> pe mapping array */
        struct pe_specifier *map;
};

struct name_list {
	struct list list;
	char *name;
};

struct pv_list {
	struct list list;
	struct physical_volume pv;
};

struct lv_list {
	struct list list;
	struct logical_volume lv;
};

/*
 * Ownership of objects passes to caller.
 */
struct io_space {
	/*
	 * Returns a name_list of vg's.
	 */
	struct list *(*get_vgs)(struct io_space *is);

	/*
	 * Returns pv_list of fully-populated pv structures.
	 */
	struct list *(*get_pvs)(struct io_space *is);

	/*
	 * Return PV with given path.
	 */
	struct physical_volume *(*pv_read)(struct io_space *is,
					   const char *pv_name);

	/*
	 * Tweak an already filled out a pv ready
	 * for importing into a vg.  eg. pe_count
	 * is format specific.
	 */
	int (*pv_setup)(struct io_space *is, struct physical_volume *pv,
			struct volume_group *vg);

	/*
	 * Write a PV structure to disk. Fails if
	 * the PV is in a VG ie pv->vg_name must
	 * be null.
	 */
	int (*pv_write)(struct io_space *is, struct physical_volume *pv);

	/*
	 * Tweak an already filled out vg.  eg,
	 * max_pv is format specific.
	 */
	int (*vg_setup)(struct io_space *is, struct volume_group *vg);

	/*
	 * If vg_name doesn't contain any slash,
	 * this function adds prefix.
	 */
	struct volume_group *(*vg_read)(struct io_space *is,
					const char *vg_name);

	/*
	 * Write out complete VG metadata.  Ensure
	 * *internal* consistency before writing
	 * anything.  eg. PEs can't refer to PVs
	 * not part of the VG.  Order write sequence
	 * to aid recovery if process is aborted
	 * (eg flush entire set of changes to each
	 * disk in turn) It is the responsibility
	 * of the caller to ensure external
	 * consistency, eg by calling pv_write()
	 * if removing PVs from a VG or calling
	 * vg_write() a second time if splitting a
	 * VG into two.  vg_write() must not read
	 * or write from any PVs not included in
	 * the volume_group structure it is
	 * handed.
	 */
	int (*vg_write)(struct io_space *is, struct volume_group *vg);

	/*
	 * Destructor for this object.
	 */
	void (*destroy)(struct io_space *is);

	/*
	 * Current volume group prefix.
	 */
	char *prefix;
	struct pool *mem;
	struct dev_filter *filter;
	void *private;
};

/*
 * Utility functions
 */
struct volume_group *vg_create(struct io_space *ios, const char *name,
			       uint32_t extent_size, int max_pv, int max_lv,
			       int pv_count, char **pv_names);
struct physical_volume *pv_create(struct io_space *ios, const char *name);

/*
 * Create a new LV within a given volume group.
 *
 */
struct logical_volume *lv_create(struct io_space *ios,
				 const char *name,
				 uint32_t status,
				 uint32_t stripes,
				 uint32_t stripe_size,
				 uint32_t extents,
				 struct volume_group *vg,
				 struct list *acceptable_pvs);

int lv_reduce(struct io_space *ios,
	      struct logical_volume *lv,
	      uint32_t extents);

int lv_extend(struct io_space *ios, struct logical_volume *lv,
	      uint32_t extents, struct list *allocatable_pvs);


int vg_extend(struct io_space *ios, struct volume_group *vg, int pv_count,
	      char **pv_names);



/* FIXME: Move to other files */
struct io_space *create_text_format(struct dev_filter *filter,
				    const char *text_file);

int id_eq(struct id *op1, struct id *op2);

/* Create consistent new empty structures, populated with defaults */
struct volume_group *vg_create();

int vg_destroy(struct volume_group *vg);

/* Manipulate PV structures */
int pv_add(struct volume_group *vg, struct physical_volume *pv);
int pv_remove(struct volume_group *vg, struct physical_volume *pv);
struct physical_volume *pv_find(struct volume_group *vg,
				const char *pv_name);

/* Remove an LV from a given VG */
int lv_remove(struct volume_group *vg, struct list *lvh);

/* Find a PV within a given VG */
struct list *find_pv_in_vg(struct volume_group *vg, const char *pv_name);

/* Find an LV within a given VG */
struct list *find_lv_in_vg(struct volume_group *vg, const char *lv_name);

/* Return the VG that contains a given LV (based on path given in lv_name) */
/* or environment var */
struct volume_group *find_vg_with_lv(const char *lv_name);


/* FIXME Merge these functions with ones above */
struct physical_volume *_find_pv(struct volume_group *vg, struct device *dev);
struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name);


#endif
