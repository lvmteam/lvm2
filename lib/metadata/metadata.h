/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * This is the in core representation of a volume group and its
 * associated physical and logical volumes.
 */

#ifndef _LVM_METADATA_H
#define _LVM_METADATA_H

#include "ctype.h"
#include "dev-cache.h"
#include "uuid.h"

#define NAME_LEN 128
#define MAX_STRIPES 128
#define SECTOR_SHIFT 9L
#define SECTOR_SIZE ( 1L << SECTOR_SHIFT )
#define STRIPE_SIZE_MIN ( getpagesize() >> SECTOR_SHIFT)	/* PAGESIZE in sectors */
#define STRIPE_SIZE_MAX ( 512L * 1024L >> SECTOR_SHIFT)	/* 512 KB in sectors */
#define PV_MIN_SIZE ( 512L * 1024L >> SECTOR_SHIFT)	/* 512 KB in sectors */
#define PE_ALIGN (65536UL >> SECTOR_SHIFT)	/* PE alignment */
#define MAX_RESTRICTED_LVS 255	/* Used by FMT_RESTRICTED_LVIDS */

/* Various flags */
/* Note that the bits no longer necessarily correspond to LVM1 disk format */

#define PARTIAL_VG		0x00000001	/* VG */
#define EXPORTED_VG          	0x00000002	/* VG PV */
#define RESIZEABLE_VG        	0x00000004	/* VG */

/* May any free extents on this PV be used or must they be left free? */
#define ALLOCATABLE_PV         	0x00000008	/* PV */

#define SPINDOWN_LV          	0x00000010	/* LV */
#define BADBLOCK_ON       	0x00000020	/* LV */
#define VISIBLE_LV		0x00000040	/* LV */
#define FIXED_MINOR		0x00000080	/* LV */
/* FIXME Remove when metadata restructuring is completed */
#define SNAPSHOT		0x00001000	/* LV - tmp internal use only */
#define PVMOVE			0x00002000	/* VG LV SEG */
#define LOCKED			0x00004000	/* LV */
#define MIRRORED		0x00008000	/* LV - internal use only */
#define VIRTUAL			0x00010000	/* LV - internal use only */

#define LVM_READ              	0x00000100	/* LV VG */
#define LVM_WRITE             	0x00000200	/* LV VG */
#define CLUSTERED         	0x00000400	/* VG */
#define SHARED            	0x00000800	/* VG */

/* Format features flags */
#define FMT_SEGMENTS		0x00000001	/* Arbitrary segment params? */
#define FMT_MDAS		0x00000002	/* Proper metadata areas? */
#define FMT_TAGS		0x00000004	/* Tagging? */
#define FMT_UNLIMITED_VOLS	0x00000008	/* Unlimited PVs/LVs? */
#define FMT_RESTRICTED_LVIDS	0x00000010	/* LVID <= 255 */
#define FMT_ORPHAN_ALLOCATABLE	0x00000020	/* Orphan PV allocatable? */

typedef enum {
	ALLOC_INVALID,
	ALLOC_INHERIT,
	ALLOC_CONTIGUOUS,
	ALLOC_NORMAL,
	ALLOC_ANYWHERE
} alloc_policy_t;

typedef enum {
	AREA_PV,
	AREA_LV
} area_type_t;

struct cmd_context;
struct format_handler;
struct labeller;

struct format_type {
	struct list list;
	struct cmd_context *cmd;
	struct format_handler *ops;
	struct labeller *labeller;
	const char *name;
	const char *alias;
	uint32_t features;
	void *library;
	void *private;
};

struct physical_volume {
	struct id id;
	struct device *dev;
	const struct format_type *fmt;
	const char *vg_name;

	uint32_t status;
	uint64_t size;

	/* physical extents */
	uint32_t pe_size;
	uint64_t pe_start;
	uint32_t pe_count;
	uint32_t pe_alloc_count;

	struct list tags;
};

struct metadata_area;
struct format_instance;

/* Per-format per-metadata area operations */
struct metadata_area_ops {
	struct volume_group *(*vg_read) (struct format_instance * fi,
					 const char *vg_name,
					 struct metadata_area * mda);
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
	 * vg_write() should not read or write from any PVs not included
	 * in the volume_group structure it is handed.
	 * (format1 currently breaks this rule.)
	 */
	int (*vg_write) (struct format_instance * fid, struct volume_group * vg,
			 struct metadata_area * mda);
	int (*vg_commit) (struct format_instance * fid,
			  struct volume_group * vg, struct metadata_area * mda);
	int (*vg_revert) (struct format_instance * fid,
			  struct volume_group * vg, struct metadata_area * mda);
	int (*vg_remove) (struct format_instance * fi, struct volume_group * vg,
			  struct metadata_area * mda);
};

struct metadata_area {
	struct list list;
	struct metadata_area_ops *ops;
	void *metadata_locn;
};

struct format_instance {
	const struct format_type *fmt;
	struct list metadata_areas;	/* e.g. metadata locations */
};

struct volume_group {
	struct cmd_context *cmd;
	struct format_instance *fid;
	uint32_t seqno;		/* Metadata sequence number */

	struct id id;
	char *name;
	char *system_id;

	uint32_t status;
	alloc_policy_t alloc;

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

	struct list tags;
};

struct segment_type;
struct lv_segment {
	struct list list;
	struct logical_volume *lv;

	struct segment_type *segtype;
	uint32_t le;
	uint32_t len;

	uint32_t status;

	/* FIXME Fields depend on segment type */
	uint32_t stripe_size;
	uint32_t area_count;
	uint32_t area_len;
	struct logical_volume *origin;
	struct logical_volume *cow;
	uint32_t chunk_size;
	uint32_t extents_copied;

	struct list tags;

	/* There will be one area for each stripe */
	struct {
		area_type_t type;
		union {
			struct {
				struct physical_volume *pv;
				uint32_t pe;
			} pv;
			struct {
				struct logical_volume *lv;
				uint32_t le;
			} lv;
		} u;
	} area[0];
};

struct logical_volume {
	union lvid lvid;
	char *name;

	struct volume_group *vg;

	uint32_t status;
	alloc_policy_t alloc;
	uint32_t read_ahead;
	int32_t major;
	int32_t minor;

	uint64_t size;
	uint32_t le_count;

	struct list segments;
	struct list tags;
};

struct snapshot {
	struct id id;

	int persistent;		/* boolean */
	uint32_t chunk_size;	/* in 512 byte sectors */

	struct logical_volume *origin;
	struct logical_volume *cow;
};

struct name_list {
	struct list list;
	char *name;
};

struct pe_range {
	struct list list;
	uint32_t start;		/* PEs */
	uint32_t count;		/* PEs */
};

struct pv_list {
	struct list list;
	struct physical_volume *pv;
	struct list *mdas;	/* Metadata areas */
	struct list *pe_ranges;	/* Ranges of PEs e.g. for allocation */
};

struct lv_list {
	struct list list;
	struct logical_volume *lv;
};

struct snapshot_list {
	struct list list;

	struct snapshot *snapshot;
};

struct mda_list {
	struct list list;
	struct device_area mda;
};

/*
 * Ownership of objects passes to caller.
 */
struct format_handler {
	/*
	 * Scan any metadata areas that aren't referenced in PV labels
	 */
	int (*scan) (const struct format_type * fmt);

	/*
	 * Return PV with given path.
	 */
	int (*pv_read) (const struct format_type * fmt, const char *pv_name,
			struct physical_volume * pv, struct list * mdas);

	/*
	 * Tweak an already filled out a pv ready for importing into a
	 * vg.  eg. pe_count is format specific.
	 */
	int (*pv_setup) (const struct format_type * fmt,
			 uint64_t pe_start, uint32_t extent_count,
			 uint32_t extent_size,
			 int pvmetadatacopies,
			 uint64_t pvmetadatasize, struct list * mdas,
			 struct physical_volume * pv, struct volume_group * vg);

	/*
	 * Write a PV structure to disk. Fails if the PV is in a VG ie
	 * pv->vg_name must be null.
	 */
	int (*pv_write) (const struct format_type * fmt,
			 struct physical_volume * pv, struct list * mdas,
			 int64_t label_sector);

	/*
	 * Tweak an already filled out a lv eg, check there
	 * aren't too many extents.
	 */
	int (*lv_setup) (struct format_instance * fi,
			 struct logical_volume * lv);

	/*
	 * Tweak an already filled out vg.  eg, max_pv is format
	 * specific.
	 */
	int (*vg_setup) (struct format_instance * fi, struct volume_group * vg);

	/*
	 * Check whether particular segment type is supported.
	 */
	int (*segtype_supported) (struct format_instance *fid,
				  struct segment_type *segtype);

	/*
	 * Create format instance with a particular metadata area
	 */
	struct format_instance *(*create_instance) (const struct format_type *
						    fmt, const char *vgname,
						    void *context);

	/*
	 * Destructor for format instance
	 */
	void (*destroy_instance) (struct format_instance * fid);

	/*
	 * Destructor for format type
	 */
	void (*destroy) (const struct format_type * fmt);
};

/*
 * Utility functions
 */
int vg_write(struct volume_group *vg);
int vg_commit(struct volume_group *vg);
int vg_revert(struct volume_group *vg);
struct volume_group *vg_read(struct cmd_context *cmd, const char *vg_name,
			     int *consistent);
struct volume_group *vg_read_by_vgid(struct cmd_context *cmd, const char *vgid);
struct physical_volume *pv_read(struct cmd_context *cmd, const char *pv_name,
				struct list *mdas, uint64_t *label_sector,
				int warnings);
struct list *get_pvs(struct cmd_context *cmd);

/* Set full_scan to 1 to re-read every (filtered) device label */
struct list *get_vgs(struct cmd_context *cmd, int full_scan);

int pv_write(struct cmd_context *cmd, struct physical_volume *pv,
	     struct list *mdas, int64_t label_sector);

/* pe_start and pe_end relate to any existing data so that new metadata
 * areas can avoid overlap */
struct physical_volume *pv_create(const struct format_type *fmt,
				  struct device *dev,
				  struct id *id,
				  uint64_t size,
				  uint64_t pe_start,
				  uint32_t existing_extent_count,
				  uint32_t existing_extent_size,
				  int pvmetadatacopies,
				  uint64_t pvmetadatasize, struct list *mdas);

struct volume_group *vg_create(struct cmd_context *cmd, const char *name,
			       uint32_t extent_size, uint32_t max_pv,
			       uint32_t max_lv, alloc_policy_t alloc,
			       int pv_count, char **pv_names);
int vg_remove(struct volume_group *vg);
int vg_rename(struct cmd_context *cmd, struct volume_group *vg,
	      const char *new_name);
int vg_extend(struct format_instance *fi, struct volume_group *vg,
	      int pv_count, char **pv_names);

/* Manipulate LVs */
struct logical_volume *lv_create_empty(struct format_instance *fi,
				       const char *name,
				       const char *name_format,
				       uint32_t status,
				       alloc_policy_t alloc,
				       struct volume_group *vg);

int lv_reduce(struct format_instance *fi,
	      struct logical_volume *lv, uint32_t extents);

int lv_extend(struct format_instance *fid,
	      struct logical_volume *lv,
	      struct segment_type *segtype,
	      uint32_t stripes, uint32_t stripe_size,
	      uint32_t mirrors, uint32_t extents,
	      struct physical_volume *mirrored_pv, uint32_t mirrored_pe,
	      uint32_t status, struct list *allocatable_pvs,
	      alloc_policy_t alloc);

/* lv must be part of vg->lvs */
int lv_remove(struct volume_group *vg, struct logical_volume *lv);

/* Manipulate PV structures */
int pv_add(struct volume_group *vg, struct physical_volume *pv);
int pv_remove(struct volume_group *vg, struct physical_volume *pv);
struct physical_volume *pv_find(struct volume_group *vg, const char *pv_name);

/* Find a PV within a given VG */
struct pv_list *find_pv_in_vg(struct volume_group *vg, const char *pv_name);
struct physical_volume *find_pv_in_vg_by_uuid(struct volume_group *vg,
					      struct id *id);

/* Find an LV within a given VG */
struct lv_list *find_lv_in_vg(struct volume_group *vg, const char *lv_name);
struct lv_list *find_lv_in_vg_by_lvid(struct volume_group *vg,
				      const union lvid *lvid);

/* Return the VG that contains a given LV (based on path given in lv_name) */
/* or environment var */
struct volume_group *find_vg_with_lv(const char *lv_name);

/* Find LV with given lvid (used during activation) */
struct logical_volume *lv_from_lvid(struct cmd_context *cmd,
				    const char *lvid_s);

/* FIXME Merge these functions with ones above */
struct physical_volume *find_pv(struct volume_group *vg, struct device *dev);
struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name);
struct physical_volume *find_pv_by_name(struct cmd_context *cmd,
					const char *pv_name);

/* Find LV segment containing given LE */
struct lv_segment *find_seg_by_le(struct logical_volume *lv, uint32_t le);

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
 * Ensure there's a segment boundary at a given LE, splitting if necessary
 */
int lv_split_segment(struct logical_volume *lv, uint32_t le);

/*
 * Useful functions for managing snapshots.
 */
int lv_is_origin(const struct logical_volume *lv);
int lv_is_cow(const struct logical_volume *lv);

int pv_is_in_vg(struct volume_group *vg, struct physical_volume *pv);

struct snapshot *find_cow(const struct logical_volume *lv);
struct snapshot *find_origin(const struct logical_volume *lv);
struct list *find_snapshots(const struct logical_volume *lv);

int vg_add_snapshot(struct logical_volume *origin,
		    struct logical_volume *cow,
		    int persistent, struct id *id, uint32_t chunk_size);

int vg_remove_snapshot(struct volume_group *vg, struct logical_volume *cow);

/*
 * Mirroring functions
 */
int insert_pvmove_mirrors(struct cmd_context *cmd,
			  struct logical_volume *lv_mirr,
			  struct list *source_pvl,
			  struct logical_volume *lv,
			  struct list *allocatable_pvs,
			  struct list *lvs_changed);
int remove_pvmove_mirrors(struct volume_group *vg,
			  struct logical_volume *lv_mirr);
struct logical_volume *find_pvmove_lv(struct volume_group *vg,
				      struct device *dev, uint32_t lv_type);
struct logical_volume *find_pvmove_lv_from_pvname(struct cmd_context *cmd,
						  struct volume_group *vg,
						  const char *name,
						  uint32_t lv_type);
const char *get_pvmove_pvname_from_lv(struct logical_volume *lv);
const char *get_pvmove_pvname_from_lv_mirr(struct logical_volume *lv_mirr);
float copy_percent(struct logical_volume *lv_mirr);
struct list *lvs_using_lv(struct cmd_context *cmd, struct volume_group *vg,
			  struct logical_volume *lv);

uint32_t find_free_lvnum(struct logical_volume *lv);

static inline int validate_name(const char *n)
{
	register char c;
	register int len = 0;

	if (!n || !*n)
		return 0;

	/* Hyphen used as VG-LV separator - ambiguity if LV starts with it */
	if (*n == '-')
		return 0;

	while ((len++, c = *n++))
		if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+')
			return 0;

	if (len > NAME_LEN)
		return 0;

	return 1;
}

#endif
