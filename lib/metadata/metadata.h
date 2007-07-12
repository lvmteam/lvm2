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
#include "lvm-string.h"
#include "uuid.h"

#define MAX_STRIPES 128U
#define SECTOR_SHIFT 9L
#define SECTOR_SIZE ( 1L << SECTOR_SHIFT )
#define STRIPE_SIZE_MIN ( (unsigned) lvm_getpagesize() >> SECTOR_SHIFT)	/* PAGESIZE in sectors */
#define STRIPE_SIZE_MAX ( 512L * 1024L >> SECTOR_SHIFT)	/* 512 KB in sectors */
#define STRIPE_SIZE_LIMIT ((UINT_MAX >> 2) + 1)
#define PV_MIN_SIZE ( 512L * 1024L >> SECTOR_SHIFT)	/* 512 KB in sectors */
#define MAX_RESTRICTED_LVS 255	/* Used by FMT_RESTRICTED_LVIDS */
#define MIRROR_LOG_SIZE 1	/* Extents */

/* Various flags */
/* Note that the bits no longer necessarily correspond to LVM1 disk format */

#define PARTIAL_VG		0x00000001U	/* VG */
#define EXPORTED_VG          	0x00000002U	/* VG PV */
#define RESIZEABLE_VG        	0x00000004U	/* VG */

/* May any free extents on this PV be used or must they be left free? */
#define ALLOCATABLE_PV         	0x00000008U	/* PV */

#define SPINDOWN_LV          	0x00000010U	/* LV */
#define BADBLOCK_ON       	0x00000020U	/* LV */
#define VISIBLE_LV		0x00000040U	/* LV */
#define FIXED_MINOR		0x00000080U	/* LV */
/* FIXME Remove when metadata restructuring is completed */
#define SNAPSHOT		0x00001000U	/* LV - internal use only */
#define PVMOVE			0x00002000U	/* VG LV SEG */
#define LOCKED			0x00004000U	/* LV */
#define MIRRORED		0x00008000U	/* LV - internal use only */
#define VIRTUAL			0x00010000U	/* LV - internal use only */
#define MIRROR_LOG		0x00020000U	/* LV */
#define MIRROR_IMAGE		0x00040000U	/* LV */
#define MIRROR_NOTSYNCED	0x00080000U	/* LV */
#define ACTIVATE_EXCL		0x00100000U	/* LV - internal use only */
#define PRECOMMITTED		0x00200000U	/* VG - internal use only */

#define LVM_READ              	0x00000100U	/* LV VG */
#define LVM_WRITE             	0x00000200U	/* LV VG */
#define CLUSTERED         	0x00000400U	/* VG */
#define SHARED            	0x00000800U	/* VG */

/* Format features flags */
#define FMT_SEGMENTS		0x00000001U	/* Arbitrary segment params? */
#define FMT_MDAS		0x00000002U	/* Proper metadata areas? */
#define FMT_TAGS		0x00000004U	/* Tagging? */
#define FMT_UNLIMITED_VOLS	0x00000008U	/* Unlimited PVs/LVs? */
#define FMT_RESTRICTED_LVIDS	0x00000010U	/* LVID <= 255 */
#define FMT_ORPHAN_ALLOCATABLE	0x00000020U	/* Orphan PV allocatable? */
#define FMT_PRECOMMIT		0x00000040U	/* Supports pre-commit? */
#define FMT_RESIZE_PV		0x00000080U	/* Supports pvresize? */
#define FMT_UNLIMITED_STRIPESIZE 0x00000100U	/* Unlimited stripe size? */

/* Ordered list - see lv_manip.c */
typedef enum {
	ALLOC_INVALID,
	ALLOC_CONTIGUOUS,
	ALLOC_CLING,
	ALLOC_NORMAL,
	ALLOC_ANYWHERE,
	ALLOC_INHERIT
} alloc_policy_t;

typedef enum {
	AREA_UNASSIGNED,
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

struct pv_segment {
	struct list list;	/* Member of pv->segments: ordered list
				 * covering entire data area on this PV */

	struct physical_volume *pv;
	uint32_t pe;
	uint32_t len;

	struct lv_segment *lvseg;	/* NULL if free space */
	uint32_t lv_area;	/* Index to area in LV segment */
};

struct physical_volume {
	struct id id;
	struct device *dev;
	const struct format_type *fmt;
	const char *vg_name;
	struct id vgid;

	uint32_t status;
	uint64_t size;

	/* physical extents */
	uint32_t pe_size;
	uint64_t pe_start;
	uint32_t pe_count;
	uint32_t pe_alloc_count;

	struct list segments;	/* Ordered pv_segments covering complete PV */
	struct list tags;
};

typedef struct physical_volume pv_t;
struct metadata_area;
struct format_instance;

/* Per-format per-metadata area operations */
struct metadata_area_ops {
	struct volume_group *(*vg_read) (struct format_instance * fi,
					 const char *vg_name,
					 struct metadata_area * mda);
	struct volume_group *(*vg_read_precommit) (struct format_instance * fi,
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
	int (*vg_precommit) (struct format_instance * fid,
			     struct volume_group * vg,
			     struct metadata_area * mda);
	int (*vg_commit) (struct format_instance * fid,
			  struct volume_group * vg, struct metadata_area * mda);
	int (*vg_revert) (struct format_instance * fid,
			  struct volume_group * vg, struct metadata_area * mda);
	int (*vg_remove) (struct format_instance * fi, struct volume_group * vg,
			  struct metadata_area * mda);
	/*
	 * Check if metadata area belongs to vg
	 */
	int (*mda_in_vg) (struct format_instance * fi,
			    struct volume_group * vg, struct metadata_area *mda);
	/*
	 * Analyze a metadata area on a PV.
	 */
	int (*pv_analyze_mda) (const struct format_type * fmt,
			       struct metadata_area *mda);

};

struct metadata_area {
	struct list list;
	struct metadata_area_ops *ops;
	void *metadata_locn;
};

struct format_instance {
	const struct format_type *fmt;
	struct list metadata_areas;	/* e.g. metadata locations */
	void *private;
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
	uint32_t snapshot_count;
	struct list lvs;

	struct list tags;
};

typedef struct volume_group vg_t;

/* There will be one area for each stripe */
struct lv_segment_area {
	area_type_t type;
	union {
		struct {
			struct pv_segment *pvseg;
		} pv;
		struct {
			struct logical_volume *lv;
			uint32_t le;
		} lv;
	} u;
};

struct segment_type;
struct lv_segment {
	struct list list;
	struct logical_volume *lv;

	const struct segment_type *segtype;
	uint32_t le;
	uint32_t len;

	uint32_t status;

	/* FIXME Fields depend on segment type */
	uint32_t stripe_size;
	uint32_t area_count;
	uint32_t area_len;
	struct logical_volume *origin;
	struct logical_volume *cow;
	struct list origin_list;
	uint32_t chunk_size;	/* For snapshots - in sectors */
	uint32_t region_size;	/* For mirrors - in sectors */
	uint32_t extents_copied;
	struct logical_volume *log_lv;
	struct lv_segment *mirror_seg;

	struct list tags;

	struct lv_segment_area *areas;
};

#define seg_type(seg, s)	(seg)->areas[(s)].type
#define seg_pvseg(seg, s)	(seg)->areas[(s)].u.pv.pvseg
#define seg_pv(seg, s)		(seg)->areas[(s)].u.pv.pvseg->pv
#define seg_dev(seg, s)		(seg)->areas[(s)].u.pv.pvseg->pv->dev
#define seg_pe(seg, s)		(seg)->areas[(s)].u.pv.pvseg->pe
#define seg_lv(seg, s)		(seg)->areas[(s)].u.lv.lv
#define seg_le(seg, s)		(seg)->areas[(s)].u.lv.le

struct logical_volume {
	union lvid lvid;
	char *name;

	struct volume_group *vg;

	uint32_t status;
	alloc_policy_t alloc;
	uint32_t read_ahead;
	int32_t major;
	int32_t minor;

	uint64_t size;		/* Sectors */
	uint32_t le_count;

	uint32_t origin_count;
	struct list snapshot_segs;
	struct lv_segment *snapshot;

	struct list segments;
	struct list tags;
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

struct mda_list {
	struct list list;
	struct device_area mda;
};

struct peg_list {
	struct list list;
	struct pv_segment *peg;
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
				  const struct segment_type *segtype);

	/*
	 * Create format instance with a particular metadata area
	 */
	struct format_instance *(*create_instance) (const struct format_type *
						    fmt, const char *vgname,
						    const char *vgid,
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
unsigned long pe_align(void);
int vg_validate(struct volume_group *vg);
int vg_write(struct volume_group *vg);
int vg_commit(struct volume_group *vg);
int vg_revert(struct volume_group *vg);
struct volume_group *vg_read(struct cmd_context *cmd, const char *vg_name,
			     const char *vgid, int *consistent);
struct physical_volume *pv_read(struct cmd_context *cmd, const char *pv_name,
				struct list *mdas, uint64_t *label_sector,
				int warnings);
struct list *get_pvs(struct cmd_context *cmd);

/* Set full_scan to 1 to re-read every (filtered) device label */
struct list *get_vgs(struct cmd_context *cmd, int full_scan);
struct list *get_vgids(struct cmd_context *cmd, int full_scan);

int pv_write(struct cmd_context *cmd, struct physical_volume *pv,
	     struct list *mdas, int64_t label_sector);
int pv_write_orphan(struct cmd_context *cmd, struct physical_volume *pv);
int is_orphan(pv_t *pv);

/* pe_start and pe_end relate to any existing data so that new metadata
 * areas can avoid overlap */
pv_t *pv_create(const struct format_type *fmt,
		      struct device *dev,
		      struct id *id,
		      uint64_t size,
		      uint64_t pe_start,
		      uint32_t existing_extent_count,
		      uint32_t existing_extent_size,
		      int pvmetadatacopies,
		      uint64_t pvmetadatasize, struct list *mdas);
int pv_resize(struct physical_volume *pv, struct volume_group *vg,
              uint32_t new_pe_count);
int pv_analyze(struct cmd_context *cmd, const char *pv_name,
	       int64_t label_sector);

struct volume_group *vg_create(struct cmd_context *cmd, const char *name,
			       uint32_t extent_size, uint32_t max_pv,
			       uint32_t max_lv, alloc_policy_t alloc,
			       int pv_count, char **pv_names);
int vg_remove(struct volume_group *vg);
int vg_rename(struct cmd_context *cmd, struct volume_group *vg,
	      const char *new_name);
int vg_extend(struct volume_group *vg, int pv_count, char **pv_names);
int vg_change_pesize(struct cmd_context *cmd, struct volume_group *vg,
		     uint32_t new_extent_size);
int vg_split_mdas(struct cmd_context *cmd, struct volume_group *vg_from,
		  struct volume_group *vg_to);

/* Manipulate LVs */
struct logical_volume *lv_create_empty(struct format_instance *fi,
				       const char *name,
				       union lvid *lvid,
				       uint32_t status,
				       alloc_policy_t alloc,
				       int import,
				       struct volume_group *vg);

/* Reduce the size of an LV by extents */
int lv_reduce(struct logical_volume *lv, uint32_t extents);

/* Empty an LV prior to deleting it */
int lv_empty(struct logical_volume *lv);

/* Entry point for all LV extent allocations */
int lv_extend(struct logical_volume *lv,
	      const struct segment_type *segtype,
	      uint32_t stripes, uint32_t stripe_size,
	      uint32_t mirrors, uint32_t extents,
	      struct physical_volume *mirrored_pv, uint32_t mirrored_pe,
	      uint32_t status, struct list *allocatable_pvs,
	      alloc_policy_t alloc);

/* lv must be part of lv->vg->lvs */
int lv_remove(struct logical_volume *lv);

/* Manipulate PV structures */
int pv_add(struct volume_group *vg, struct physical_volume *pv);
int pv_remove(struct volume_group *vg, struct physical_volume *pv);
struct physical_volume *pv_find(struct volume_group *vg, const char *pv_name);

/* Find a PV within a given VG */
struct pv_list *find_pv_in_vg(struct volume_group *vg, const char *pv_name);
pv_t *find_pv_in_vg_by_uuid(struct volume_group *vg, struct id *id);
int get_pv_from_vg_by_id(const struct format_type *fmt, const char *vg_name,
			 const char *vgid, const char *pvid,
			 struct physical_volume *pv);

/* Find an LV within a given VG */
struct lv_list *find_lv_in_vg(struct volume_group *vg, const char *lv_name);
struct lv_list *find_lv_in_vg_by_lvid(struct volume_group *vg,
				      const union lvid *lvid);

/* Return the VG that contains a given LV (based on path given in lv_name) */
/* or environment var */
struct volume_group *find_vg_with_lv(const char *lv_name);

/* Find LV with given lvid (used during activation) */
struct logical_volume *lv_from_lvid(struct cmd_context *cmd,
				    const char *lvid_s,
				    int precommitted);

/* FIXME Merge these functions with ones above */
struct physical_volume *find_pv(struct volume_group *vg, struct device *dev);
struct logical_volume *find_lv(struct volume_group *vg, const char *lv_name);
struct physical_volume *find_pv_by_name(struct cmd_context *cmd,
					const char *pv_name);

/* Find LV segment containing given LE */
struct lv_segment *find_seg_by_le(struct logical_volume *lv, uint32_t le);
struct lv_segment *first_seg(struct logical_volume *lv);

/* Find PV segment containing given LE */
struct pv_segment *find_peg_by_pe(struct physical_volume *pv, uint32_t pe);

/*
 * Remove a dev_dir if present.
 */
const char *strip_dir(const char *vg_name, const char *dir);

/*
 * Checks that an lv has no gaps or overlapping segments.
 * Set complete_vg to perform additional VG level checks.
 */
int check_lv_segments(struct logical_volume *lv, int complete_vg);

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
int lv_is_visible(const struct logical_volume *lv);

int pv_is_in_vg(struct volume_group *vg, struct physical_volume *pv);

/* Given a cow LV, return return the snapshot lv_segment that uses it */
struct lv_segment *find_cow(const struct logical_volume *lv);

/* Given a cow LV, return its origin */
struct logical_volume *origin_from_cow(const struct logical_volume *lv);

int vg_add_snapshot(struct format_instance *fid, const char *name,
		    struct logical_volume *origin, struct logical_volume *cow,
		    union lvid *lvid, uint32_t extent_count,
		    uint32_t chunk_size);

int vg_remove_snapshot(struct logical_volume *cow);

int vg_check_status(struct volume_group *vg, uint32_t status);

/*
 * Mirroring functions
 */
struct alloc_handle;
uint32_t adjusted_mirror_region_size(uint32_t extent_size, uint32_t extents,
                                     uint32_t region_size);
int create_mirror_layers(struct alloc_handle *ah,
			 uint32_t first_area,
			 uint32_t num_mirrors,
			 struct logical_volume *lv,
			 const struct segment_type *segtype,
			 uint32_t status,
			 uint32_t region_size,
			 struct logical_volume *log_lv);
int add_mirror_layers(struct alloc_handle *ah,
		      uint32_t num_mirrors,
		      uint32_t existing_mirrors,
		      struct logical_volume *lv,
		      const struct segment_type *segtype);

int remove_mirror_images(struct lv_segment *mirrored_seg, uint32_t num_mirrors,
			 struct list *removable_pvs, int remove_log);
int reconfigure_mirror_images(struct lv_segment *mirrored_seg, uint32_t num_mirrors,
			      struct list *removable_pvs, int remove_log);
/*
 * Given mirror image or mirror log segment, find corresponding mirror segment 
 */
struct lv_segment *find_mirror_seg(struct lv_segment *seg);
int fixup_imported_mirrors(struct volume_group *vg);

int insert_pvmove_mirrors(struct cmd_context *cmd,
			  struct logical_volume *lv_mirr,
			  struct list *source_pvl,
			  struct logical_volume *lv,
			  struct list *allocatable_pvs,
			  alloc_policy_t alloc,
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
char *generate_lv_name(struct volume_group *vg, const char *format,
		       char *buffer, size_t len);

/*
 * Begin skeleton for external LVM library
 */
struct id pv_id(pv_t *pv);
const struct format_type *pv_format_type(pv_t *pv);
struct id pv_vgid(pv_t *pv);
struct device *pv_dev(pv_t *pv);
const char *pv_vg_name(pv_t *pv);
uint64_t pv_size(pv_t *pv);
uint32_t pv_status(pv_t *pv);
uint32_t pv_pe_size(pv_t *pv);
uint64_t pv_pe_start(pv_t *pv);
uint32_t pv_pe_count(pv_t *pv);
uint32_t pv_pe_alloc_count(pv_t *pv);

uint32_t vg_status(vg_t *vg);

pv_t *pv_by_path(struct cmd_context *cmd, const char *pv_name);
int add_pv_to_vg(struct volume_group *vg, const char *pv_name,
		 struct physical_volume *pv);

#endif
