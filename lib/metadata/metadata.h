/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 * This is the in core representation of a volume group and its
 * associated physical and logical volumes.
 */

#ifndef _LVM_METADATA_H
#define _LVM_METADATA_H

#include <sys/types.h>
#include <asm/page.h>
#include "dev-cache.h"
#include "list.h"
#include "uuid.h"

#define NAME_LEN 128
#define MAX_STRIPES 128
#define SECTOR_SIZE 512
#define STRIPE_SIZE_DEFAULT 16    /* 16KB */
#define STRIPE_SIZE_MIN ( PAGE_SIZE/SECTOR_SIZE)     /* PAGESIZE in sectors */
#define STRIPE_SIZE_MAX ( 512L * 1024 / SECTOR_SIZE) /* 512 KB in sectors */


/* Various flags */
/* Note that the bits no longer necessarily correspond to LVM1 disk format */

#define ACTIVE               	0x00000001  /* PV VG LV */
#define EXPORTED_VG          	0x00000002  /* VG */  /* And PV too perhaps? */
#define EXTENDABLE_VG        	0x00000004  /* VG */

/* FIXME: What does this mean ? */
#define ALLOCATED_PV         	0x00000008  /* PV */

#define SPINDOWN_LV          	0x00000010  /* LV */
#define BADBLOCK_ON       	0x00000020  /* LV */

/* FIXME: do we really set read/write for a whole vg ? */
#define LVM_READ              	0x00000100  /* LV VG */
#define LVM_WRITE             	0x00000200  /* LV VG */
#define CLUSTERED         	0x00000400  /* VG */
#define SHARED            	0x00000800  /* VG */

/* FIXME: This should be an enum rather than a bitset,
   remove from status - EJT */
#define ALLOC_SIMPLE		0x00001000  /* LV */
#define ALLOC_STRICT		0x00002000  /* LV */
#define ALLOC_CONTIGUOUS	0x00004000  /* LV */

#define SNAPSHOT          	0x00010000  /* LV */
#define SNAPSHOT_ORG      	0x00020000  /* LV */


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
        uint32_t pe_allocated;	/* FIXME: change the name to alloc_count ? */
};

struct cmd_context;

struct volume_group {
	struct cmd_context *cmd;

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

struct stripe_segment {
	struct list list;

	struct logical_volume *lv;
	uint32_t le;
	uint32_t len;
	uint32_t stripe_size;
	uint32_t stripes;

	/* There will be one area for each stripe */
        struct {
		struct physical_volume *pv;
		uint32_t pe;
	} area[0];
};

struct logical_volume {
	struct id id;
        char *name;

	struct volume_group *vg;

        uint32_t status;
	uint32_t read_ahead;

        uint64_t size;
        uint32_t le_count;

	struct list segments;
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

struct cmd_context {
	/* format handler allocates all objects from here */
	struct pool *mem;

	/* misc. vars needed by format handler */
	char *dev_dir;
	struct dev_filter *filter;
	struct config_file *cf;
};

struct format_instance {
	struct cmd_context *cmd;
	struct format_handler *ops;
	void *private;
};


/*
 * Ownership of objects passes to caller.
 */
struct format_handler {
	/*
	 * Returns a name_list of vg's.
	 */
	struct list *(*get_vgs)(struct format_instance *fi);

	/*
	 * Returns pv_list of fully-populated pv structures.
	 */
	struct list *(*get_pvs)(struct format_instance *fi);

	/*
	 * Return PV with given path.
	 */
	struct physical_volume *(*pv_read)(struct format_instance *fi,
					   const char *pv_name);

	/*
	 * Tweak an already filled out a pv ready for importing into a
	 * vg.  eg. pe_count is format specific.
	 */
	int (*pv_setup)(struct format_instance *fi, struct physical_volume *pv,
			struct volume_group *vg);

	/*
	 * Write a PV structure to disk. Fails if the PV is in a VG ie
	 * pv->vg_name must be null.
	 */
	int (*pv_write)(struct format_instance *fi,
			struct physical_volume *pv);

	/*
	 * Tweak an already filled out vg.  eg, max_pv is format
	 * specific.
	 */
	int (*vg_setup)(struct format_instance *fi, struct volume_group *vg);

	/*
	 * The name may be prefixed with the dev_dir from the
	 * job_context.
	 */
	struct volume_group *(*vg_read)(struct format_instance *fi,
					const char *vg_name);

	/*
	 * Write out complete VG metadata.  You must ensure internal
	 * consistency before calling. eg. PEs can't refer to PVs not
	 * part of the VG.
	 *
	 * It is also the responsibility of the caller to ensure external
	 * consistency, eg by calling pv_write() if removing PVs from
	 * a VG or calling vg_write() a second time if splitting a VG
	 * into two.
	 *
	 * vg_write() must not read or write from any PVs not included
	 * in the volume_group structure it is handed. Note: format1
	 * does read all pv's currently.
	 */
	int (*vg_write)(struct format_instance *fi, struct volume_group *vg);

	/*
	 * Destructor for this object.
	 */
	void (*destroy)(struct format_instance *fi);
};

/*
 * Utility functions
 */
struct physical_volume *pv_create(struct format_instance *fi,
				  const char *name);

struct volume_group *vg_create(struct format_instance *fi, const char *name,
			       uint32_t extent_size, int max_pv, int max_lv,
			       int pv_count, char **pv_names);

/*
 * This needs the format instance to check the
 * pv's are orphaned.
 */
int vg_extend(struct format_instance *fi,
	      struct volume_group *vg, int pv_count, char **pv_names);


/*
 * Create a new LV within a given volume group.
 *
 */
struct logical_volume *lv_create(const char *name,
				 uint32_t status,
				 uint32_t stripes,
				 uint32_t stripe_size,
				 uint32_t extents,
				 struct volume_group *vg,
				 struct list *acceptable_pvs);

int lv_reduce(struct logical_volume *lv, uint32_t extents);

int lv_extend(struct logical_volume *lv,
	      uint32_t stripes,
	      uint32_t stripe_size,
	      uint32_t extents, 
	      struct list *allocatable_pvs);

/* lv must be part of vg->lvs */
int lv_remove(struct volume_group *vg, struct logical_volume *lv);


/* FIXME: Move to other files */
int id_eq(struct id *op1, struct id *op2);

/* Manipulate PV structures */
int pv_add(struct volume_group *vg, struct physical_volume *pv);
int pv_remove(struct volume_group *vg, struct physical_volume *pv);
struct physical_volume *pv_find(struct volume_group *vg,
				const char *pv_name);


/* Find a PV within a given VG */
struct list *find_pv_in_vg(struct volume_group *vg, const char *pv_name);

/* Find an LV within a given VG */
struct list *find_lv_in_vg(struct volume_group *vg, const char *lv_name);

/* Return the VG that contains a given LV (based on path given in lv_name) */
/* or environment var */
struct volume_group *find_vg_with_lv(const char *lv_name);


/* FIXME Merge these functions with ones above */
struct physical_volume *find_pv(struct volume_group *vg, struct device *dev);
struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name);

/*
 * Remove a dev_dir if present.
 */
const char *strip_dir(const char *vg_name, const char *dir);

#endif
