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
#define PV_MIN_SIZE ( 512L * 1024 / SECTOR_SIZE) /* 512 KB in sectors */
#define PE_ALIGN (65536UL / SECTOR_SIZE) /* PE alignment */


/* Various flags */
/* Note that the bits no longer necessarily correspond to LVM1 disk format */

#define EXPORTED_VG          	0x00000002  /* VG PV */
#define RESIZEABLE_VG        	0x00000004  /* VG */
#define PARTIAL_VG		0x00000040  /* VG */

/* May any free extents on this PV be used or must they be left free? */
#define ALLOCATABLE_PV         	0x00000008  /* PV */

#define SPINDOWN_LV          	0x00000010  /* LV */
#define BADBLOCK_ON       	0x00000020  /* LV */
#define FIXED_MINOR		0x00000080  /* LV */

/* FIXME: do we really set read/write for a whole vg ? */
#define LVM_READ              	0x00000100  /* LV VG */
#define LVM_WRITE             	0x00000200  /* LV VG */
#define CLUSTERED         	0x00000400  /* VG */
#define SHARED            	0x00000800  /* VG */

#define FMT_SEGMENTS		0x00000001 /* Arbitrary segment parameters? */

#define FMT_TEXT_NAME		"text"
#define FMT_LVM1_NAME		"lvm1"


typedef enum {
	ALLOC_NEXT_FREE,
	ALLOC_STRICT,
	ALLOC_CONTIGUOUS

} alloc_policy_t;

struct physical_volume {
        struct id id;
	struct device *dev;
	struct format_instance *fid;
	char *vg_name;

        uint32_t status;
        uint64_t size;

        /* physical extents */
        uint64_t pe_size;
        uint64_t pe_start;
        uint32_t pe_count;
        uint32_t pe_alloc_count;
};

struct cmd_context;

struct format_type {
	struct cmd_context *cmd;
	struct format_handler *ops;
	const char *name;
	uint32_t features;
	void *private;
};

struct metadata_area {
	struct list list;
	void *metadata_locn;
};

struct format_instance {
	struct format_type *fmt;
	struct list metadata_areas;	/* e.g. metadata locations */
};

struct volume_group {
	struct cmd_context *cmd;
	struct format_instance *fid;
	uint32_t seqno;			/* Metadata sequence number */

	struct id id;
	char *name;
	char *system_id;

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

	/* snapshots */
	uint32_t snapshot_count;
	struct list snapshots;
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
	union lvid lvid;
        char *name;

	struct volume_group *vg;

        uint32_t status;
	alloc_policy_t alloc;
	uint32_t read_ahead;
	int32_t minor;

        uint64_t size;
        uint32_t le_count;

	struct list segments;
};

struct snapshot {
	int persistent;		/* boolean */
	uint32_t chunk_size;	/* in 512 byte sectors */

	struct logical_volume *origin;
	struct logical_volume *cow;
};

struct name_list {
	struct list list;
	char *name;
};

struct pv_list {
	struct list list;
	struct physical_volume *pv;
};

struct lv_list {
	struct list list;
	struct logical_volume *lv;
};

struct snapshot_list {
	struct list list;

	struct snapshot *snapshot;
};



/*
 * Ownership of objects passes to caller.
 */
struct format_handler {
	/*
	 * Returns a name_list of vg's.
	 */
	struct list *(*get_vgs)(struct format_type *fmt, struct list *names);

	/*
	 * Returns pv_list of fully-populated pv structures.
	 */
	struct list *(*get_pvs)(struct format_type *fmt, struct list *results);

	/*
	 * Return PV with given path.
	 */
	int (*pv_read)(struct format_type *fmt,
					   const char *pv_name,
					   struct physical_volume *pv);

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
	int (*pv_write)(struct format_instance *fi, struct physical_volume *pv,
			void *mdl);
	int (*pv_commit)(struct format_instance *fid,
			 struct physical_volume *pv, void *mdl);

	/*
	 * Tweak an already filled out a lv eg, check there
	 * aren't too many extents.
	 */
	int (*lv_setup)(struct format_instance *fi, struct logical_volume *lv);

	/*
	 * Tweak an already filled out vg.  eg, max_pv is format
	 * specific.
	 */
	int (*vg_setup)(struct format_instance *fi, struct volume_group *vg);
	int (*vg_remove)(struct format_instance *fi, struct volume_group *vg,
			 void *mdl);

	/*
	 * The name may be prefixed with the dev_dir from the
	 * job_context.
	 * mdl is the metadata location to use
	 */
	struct volume_group *(*vg_read)(struct format_instance *fi,
					const char *vg_name, void *mdl);

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
	int (*vg_write)(struct format_instance *fid, struct volume_group *vg,
			void *mdl);

	int (*vg_commit)(struct format_instance *fid, struct volume_group *vg,
			void *mdl);
	/*
	 * Create format instance with a particular metadata area
	 */
	struct format_instance *(*create_instance)(struct format_type *fmt,
						   const char *vgname,
						   void *context);

	/*
	 * Destructor for format instance
	 */
	void (*destroy_instance)(struct format_instance *fid);

	/*
	 * Destructor for format type
	 */
	void (*destroy)(struct format_type *fmt);
};

/*
 * Utility functions
 */
int vg_write(struct volume_group *vg);
struct volume_group *vg_read(struct cmd_context *cmd, const char *vg_name);
struct volume_group *vg_read_by_vgid(struct cmd_context *cmd, const char *vgid);
struct physical_volume *pv_read(struct cmd_context *cmd, const char *pv_name);
struct list *get_pvs(struct cmd_context *cmd);
struct list *get_vgs(struct cmd_context *cmd);
int pv_write(struct cmd_context *cmd, struct physical_volume *pv);


struct physical_volume *pv_create(struct format_instance *fi,
				  const char *name,
				  struct id *id,
				  uint64_t size);

struct volume_group *vg_create(struct cmd_context *cmd, const char *name,
			       uint32_t extent_size, int max_pv, int max_lv,
			       int pv_count, char **pv_names);
int vg_remove(struct volume_group *vg);

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
struct logical_volume *lv_create(struct format_instance *fi,
				 const char *name,
				 uint32_t status,
				 alloc_policy_t alloc,
				 uint32_t stripes,
				 uint32_t stripe_size,
				 uint32_t extents,
				 struct volume_group *vg,
				 struct list *acceptable_pvs);

int lv_reduce(struct format_instance *fi,
	      struct logical_volume *lv, uint32_t extents);

int lv_extend(struct format_instance *fi,
	      struct logical_volume *lv,
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
struct pv_list *find_pv_in_vg(struct volume_group *vg, const char *pv_name);

/* Find an LV within a given VG */
struct lv_list *find_lv_in_vg(struct volume_group *vg, const char *lv_name);
struct lv_list *find_lv_in_vg_by_lvid(struct volume_group *vg, 
				      union lvid *lvid);

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

/*
 * Checks that an lv has no gaps or overlapping segments.
 */
int lv_check_segments(struct logical_volume *lv);


/*
 * Sometimes (eg, after an lvextend), it is possible to merge two
 * adjacent segments into a single segment.  This function trys
 * to merge as many segments as possible.
 */
int lv_merge_segments(struct logical_volume *lv);


/*
 * Useful functions for managing snapshots.
 */
int lv_is_origin(struct logical_volume *lv);
int lv_is_cow(struct logical_volume *lv);

struct snapshot *find_cow(struct logical_volume *lv);
struct snapshot *find_origin(struct logical_volume *lv);
struct list *find_snapshots(struct logical_volume *lv);

int vg_add_snapshot(struct logical_volume *origin,
		    struct logical_volume *cow,
		    int persistent,
		    uint32_t chunk_size);

int vg_remove_snapshot(struct volume_group *vg, struct logical_volume *cow);


#endif
