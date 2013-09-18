/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
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

/*
 * This is the representation of LVM metadata that is being adapted
 * for library export.
 */

#ifndef _LVM_METADATA_EXPORTED_H
#define _LVM_METADATA_EXPORTED_H

#include "uuid.h"
#include "pv.h"
#include "vg.h"
#include "lv.h"
#include "lvm-percent.h"

#define MAX_STRIPES 128U
#define SECTOR_SHIFT 9L
#define SECTOR_SIZE ( 1L << SECTOR_SHIFT )
#define STRIPE_SIZE_MIN ( (unsigned) lvm_getpagesize() >> SECTOR_SHIFT)	/* PAGESIZE in sectors */
#define STRIPE_SIZE_MAX ( 512L * 1024L >> SECTOR_SHIFT)	/* 512 KB in sectors */
#define STRIPE_SIZE_LIMIT ((UINT_MAX >> 2) + 1)
#define MAX_RESTRICTED_LVS 255	/* Used by FMT_RESTRICTED_LVIDS */
#define MAX_EXTENT_SIZE ((uint32_t) -1)

/* Layer suffix */
#define MIRROR_SYNC_LAYER "_mimagetmp"

/* Various flags */
/* Note that the bits no longer necessarily correspond to LVM1 disk format */

#define PARTIAL_VG		UINT64_C(0x00000001)	/* VG */
#define EXPORTED_VG		UINT64_C(0x00000002)	/* VG PV */
#define RESIZEABLE_VG		UINT64_C(0x00000004)	/* VG */

/* May any free extents on this PV be used or must they be left free? */
#define ALLOCATABLE_PV		UINT64_C(0x00000008)	/* PV */
#define ARCHIVED_VG		ALLOCATABLE_PV		/* VG, reuse same bit */

//#define SPINDOWN_LV		UINT64_C(0x00000010)	/* LV */
//#define BADBLOCK_ON		UINT64_C(0x00000020)	/* LV */
#define VISIBLE_LV		UINT64_C(0x00000040)	/* LV */
#define FIXED_MINOR		UINT64_C(0x00000080)	/* LV */

#define LVM_READ		UINT64_C(0x00000100)	/* LV, VG */
#define LVM_WRITE		UINT64_C(0x00000200)	/* LV, VG */

#define CLUSTERED		UINT64_C(0x00000400)	/* VG */
//#define SHARED		UINT64_C(0x00000800)	/* VG */

/* FIXME Remove when metadata restructuring is completed */
#define SNAPSHOT		UINT64_C(0x00001000)	/* LV - internal use only */
#define PVMOVE			UINT64_C(0x00002000)	/* VG LV SEG */
#define LOCKED			UINT64_C(0x00004000)	/* LV */
#define MIRRORED		UINT64_C(0x00008000)	/* LV - internal use only */
//#define VIRTUAL		UINT64_C(0x00010000)	/* LV - internal use only */
#define MIRROR_LOG		UINT64_C(0x00020000)	/* LV */
#define MIRROR_IMAGE		UINT64_C(0x00040000)	/* LV */

#define LV_NOTSYNCED		UINT64_C(0x00080000)	/* LV */
#define LV_REBUILD		UINT64_C(0x00100000)	/* LV */
//#define PRECOMMITTED		UINT64_C(0x00200000)	/* VG - internal use only */
#define CONVERTING		UINT64_C(0x00400000)	/* LV */

#define MISSING_PV		UINT64_C(0x00800000)	/* PV */
#define PARTIAL_LV		UINT64_C(0x01000000)	/* LV - derived flag, not
							   written out in metadata*/

//#define POSTORDER_FLAG	UINT64_C(0x02000000) /* Not real flags, reserved for
//#define POSTORDER_OPEN_FLAG	UINT64_C(0x04000000)    temporary use inside vg_read_internal. */
//#define VIRTUAL_ORIGIN	UINT64_C(0x08000000)	/* LV - internal use only */

#define MERGING			UINT64_C(0x10000000)	/* LV SEG */

#define REPLICATOR		UINT64_C(0x20000000)	/* LV -internal use only for replicator */
#define REPLICATOR_LOG		UINT64_C(0x40000000)	/* LV -internal use only for replicator-dev */
#define UNLABELLED_PV		UINT64_C(0x80000000)	/* PV -this PV had no label written yet */

#define RAID			UINT64_C(0x0000000100000000)	/* LV */
#define RAID_META		UINT64_C(0x0000000200000000)	/* LV */
#define RAID_IMAGE		UINT64_C(0x0000000400000000)	/* LV */

#define THIN_VOLUME		UINT64_C(0x0000001000000000)	/* LV */
#define THIN_POOL		UINT64_C(0x0000002000000000)	/* LV */
#define THIN_POOL_DATA		UINT64_C(0x0000004000000000)	/* LV */
#define THIN_POOL_METADATA	UINT64_C(0x0000008000000000)	/* LV */
#define POOL_METADATA_SPARE	UINT64_C(0x0000010000000000)	/* LV internal */

#define LV_WRITEMOSTLY		UINT64_C(0x0000020000000000)	/* LV (RAID1) */

#define LV_ACTIVATION_SKIP	UINT64_C(0x0000040000000000)	/* LV */

/* Format features flags */
#define FMT_SEGMENTS		0x00000001U	/* Arbitrary segment params? */
#define FMT_MDAS		0x00000002U	/* Proper metadata areas? */
#define FMT_TAGS		0x00000004U	/* Tagging? */
#define FMT_UNLIMITED_VOLS	0x00000008U	/* Unlimited PVs/LVs? */
#define FMT_RESTRICTED_LVIDS	0x00000010U	/* LVID <= 255 */
#define FMT_ORPHAN_ALLOCATABLE	0x00000020U	/* Orphan PV allocatable? */
//#define FMT_PRECOMMIT		0x00000040U	/* Supports pre-commit? */
#define FMT_RESIZE_PV		0x00000080U	/* Supports pvresize? */
#define FMT_UNLIMITED_STRIPESIZE 0x00000100U	/* Unlimited stripe size? */
#define FMT_RESTRICTED_READAHEAD 0x00000200U	/* Readahead restricted to 2-120? */
#define FMT_BAS			0x000000400U	/* Supports bootloader areas? */
#define FMT_CONFIG_PROFILE	0x000000800U	/* Supports configuration profiles? */

/* Mirror conversion type flags */
#define MIRROR_BY_SEG		0x00000001U	/* segment-by-segment mirror */
#define MIRROR_BY_LV		0x00000002U	/* mirror using whole mimage LVs */
#define MIRROR_SKIP_INIT_SYNC	0x00000010U	/* skip initial sync */

/* vg_read and vg_read_for_update flags */
#define READ_ALLOW_INCONSISTENT	0x00010000U
#define READ_ALLOW_EXPORTED	0x00020000U
#define READ_WITHOUT_LOCK	0x00040000U

/* A meta-flag, useful with toollib for_each_* functions. */
#define READ_FOR_UPDATE		0x00100000U

/* vg's "read_status" field */
#define FAILED_INCONSISTENT	0x00000001U
#define FAILED_LOCKING		0x00000002U
#define FAILED_NOTFOUND		0x00000004U
#define FAILED_READ_ONLY	0x00000008U
#define FAILED_EXPORTED		0x00000010U
#define FAILED_RESIZEABLE	0x00000020U
#define FAILED_CLUSTERED	0x00000040U
#define FAILED_ALLOCATION	0x00000080U
#define FAILED_EXIST		0x00000100U
#define SUCCESS			0x00000000U

#define VGMETADATACOPIES_ALL UINT32_MAX
#define VGMETADATACOPIES_UNMANAGED 0

#define vg_is_archived(vg)	(((vg)->status & ARCHIVED_VG) ? 1 : 0)

#define lv_is_external_origin(lv)	(((lv)->external_count > 0) ? 1 : 0)
#define lv_is_thin_volume(lv)	((lv)->status & THIN_VOLUME ? 1 : 0)
#define lv_is_thin_pool(lv)	((lv)->status & THIN_POOL ? 1 : 0)
#define lv_is_used_thin_pool(lv)	(lv_is_thin_pool(lv) && !dm_list_empty(&(lv)->segs_using_this_lv))
#define lv_is_thin_pool_data(lv)	((lv)->status & THIN_POOL_DATA ? 1 : 0)
#define lv_is_thin_pool_metadata(lv)	((lv)->status & THIN_POOL_METADATA ? 1 : 0)
#define lv_is_mirrored(lv)	((lv)->status & MIRRORED ? 1 : 0)
#define lv_is_rlog(lv)		((lv)->status & REPLICATOR_LOG ? 1 : 0)

#define lv_is_thin_type(lv)	((lv)->status & (THIN_POOL | THIN_VOLUME | THIN_POOL_DATA | THIN_POOL_METADATA) ? 1 : 0)
#define lv_is_mirror_type(lv)	((lv)->status & (MIRROR_LOG | MIRROR_IMAGE | MIRRORED | PVMOVE) ? 1 : 0)
#define lv_is_raid(lv)		(((lv)->status & (RAID)) ? 1 : 0)
#define lv_is_raid_type(lv)	(((lv)->status & (RAID | RAID_IMAGE | RAID_META)) ? 1 : 0)

#define lv_is_virtual(lv)	(((lv)->status & VIRTUAL) ? 1 : 0)
#define lv_is_pool_metadata_spare(lv)	(((lv)->status & POOL_METADATA_SPARE) ? 1 : 0)

/* Ordered list - see lv_manip.c */
typedef enum {
	AREA_UNASSIGNED,
	AREA_PV,
	AREA_LV
} area_type_t;

/*
 * Whether or not to force an operation.
 */
typedef enum {
	PROMPT = 0, /* Issue yes/no prompt to confirm operation */
	DONT_PROMPT = 1, /* Skip yes/no prompt */
	DONT_PROMPT_OVERRIDE = 2 /* Skip prompt + override a second condition */
} force_t;

typedef enum {
	THIN_DISCARDS_IGNORE,
	THIN_DISCARDS_NO_PASSDOWN,
	THIN_DISCARDS_PASSDOWN,
} thin_discards_t;

struct cmd_context;
struct format_handler;
struct labeller;

struct format_type {
	struct dm_list list;
	struct cmd_context *cmd;
	struct format_handler *ops;
	struct dm_list mda_ops; /* List of permissible mda ops. */
	struct labeller *labeller;
	const char *name;
	const char *alias;
	const char *orphan_vg_name;
	struct volume_group *orphan_vg; /* Only one ever exists. */
	uint32_t features;
	void *library;
	void *private;
};

struct pv_segment {
	struct dm_list list;	/* Member of pv->segments: ordered list
				 * covering entire data area on this PV */

	struct physical_volume *pv;
	uint32_t pe;
	uint32_t len;

	struct lv_segment *lvseg;	/* NULL if free space */
	uint32_t lv_area;	/* Index to area in LV segment */
};

#define pvseg_is_allocated(pvseg) ((pvseg)->lvseg)

/*
 * Properties of each format instance type.
 * The primary role of the format_instance is to temporarily store metadata
 * area information we are working with.
 */

/* Include any existing PV ("on-disk") mdas during format_instance initialisation. */
#define FMT_INSTANCE_MDAS		0x00000002U

/*
 * Include any auxiliary mdas during format_instance intialisation.
 * Currently, this includes metadata areas as defined by
 * metadata/dirs and metadata/raws setting.
 */
#define FMT_INSTANCE_AUX_MDAS		0x00000004U

/*
 * Include any other format-specific mdas during format_instance initialisation.
 * For example metadata areas used during backup/restore/archive handling.
 */
#define FMT_INSTANCE_PRIVATE_MDAS	0x00000008U

struct format_instance {
	unsigned ref_count;	/* Refs to this fid from VG and PV structs */
	struct dm_pool *mem;

	uint32_t type;
	const struct format_type *fmt;

	/*
	 * Each mda in a vg is on exactly one of the below lists.
	 * MDAs on the 'in_use' list will be read from / written to
	 * disk, while MDAs on the 'ignored' list will not be read
	 * or written to.
	 */
	/* FIXME: Try to use the index only. Remove these lists. */
	struct dm_list metadata_areas_in_use;
	struct dm_list metadata_areas_ignored;
	struct dm_hash_table *metadata_areas_index;

	void *private;
};

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

struct lv_thin_message {
	struct dm_list list;		/* Chained list of messages */
	dm_thin_message_t type;		/* Use dm thin message datatype */
	union {
		struct logical_volume *lv; /* For: create_thin, create_snap, trim */
		uint32_t delete_id;	/* For delete, needs device_id */
	} u;
};

struct segment_type;

/* List with vg_name, vgid and flags */
struct cmd_vg {
	struct dm_list list;
	const char *vg_name;
	const char *vgid;
	uint32_t flags;
	struct volume_group *vg;
};

/* ++ Replicator datatypes */
typedef enum {
	REPLICATOR_STATE_PASSIVE,
	REPLICATOR_STATE_ACTIVE,
	NUM_REPLICATOR_STATE
} replicator_state_t;

struct replicator_site {
	struct dm_list list;		/* Chained list of sites */
	struct dm_list rdevices;	/* Device list */

	struct logical_volume *replicator; /* Reference to replicator */

	const char *name;		/* Site name */
	const char *vg_name;		/* VG name */
	struct volume_group *vg;	/* resolved vg  (activate/deactive) */
	unsigned site_index;
	replicator_state_t state;	/* Active or pasive state of site */
	dm_replicator_mode_t op_mode;	/* Operation mode sync or async fail|warn|drop|stall */
	uint64_t fall_behind_data;	/* Bytes */
	uint32_t fall_behind_ios;	/* IO operations */
	uint32_t fall_behind_timeout;	/* Seconds */
};

struct replicator_device {
	struct dm_list list;		/* Chained list of devices from same site */

	struct lv_segment *replicator_dev; /* Reference to replicator-dev segment */
	struct replicator_site *rsite;	/* Reference to site parameters */

	uint64_t device_index;
	const char *name;		/* Device LV name */
	struct logical_volume *lv;	/* LV from replicator site's VG */
	struct logical_volume *slog;	/* Synclog lv from VG  */
	const char *slog_name;		/* Debug - specify size of core synclog */
};
/* -- Replicator datatypes */

struct lv_segment {
	struct dm_list list;
	struct logical_volume *lv;

	const struct segment_type *segtype;
	uint32_t le;
	uint32_t len;

	uint64_t status;

	/* FIXME Fields depend on segment type */
	uint32_t stripe_size;	/* For stripe and RAID - in sectors */
	uint32_t writebehind;   /* For RAID (RAID1 only) */
	uint32_t min_recovery_rate; /* For RAID */
	uint32_t max_recovery_rate; /* For RAID */
	uint32_t area_count;
	uint32_t area_len;
	uint32_t chunk_size;	/* For snapshots/thin_pool.  In sectors. */
				/* For thin_pool, 128..2097152. */
	struct logical_volume *origin;	/* snap and thin */
	struct logical_volume *cow;
	struct dm_list origin_list;
	uint32_t region_size;	/* For mirrors, replicators - in sectors */
	uint32_t extents_copied;
	struct logical_volume *log_lv;
	struct lv_segment *pvmove_source_seg;
	void *segtype_private;

	struct dm_list tags;

	struct lv_segment_area *areas;
	struct lv_segment_area *meta_areas;	/* For RAID */
	struct logical_volume *metadata_lv;	/* For thin_pool */
	uint64_t transaction_id;		/* For thin_pool, thin */
	uint64_t low_water_mark;		/* For thin_pool */
	unsigned zero_new_blocks;		/* For thin_pool */
	thin_discards_t discards;		/* For thin_pool */
	struct dm_list thin_messages;		/* For thin_pool */
	struct logical_volume *external_lv;	/* For thin */
	struct logical_volume *pool_lv;		/* For thin */
	uint32_t device_id;			/* For thin, 24bit */

	struct logical_volume *replicator;/* For replicator-devs - link to replicator LV */
	struct logical_volume *rlog_lv;	/* For replicators */
	const char *rlog_type;		/* For replicators */
	uint64_t rdevice_index_highest;	/* For replicators */
	unsigned rsite_index_highest;	/* For replicators */
};

#define seg_type(seg, s)	(seg)->areas[(s)].type
#define seg_pv(seg, s)		(seg)->areas[(s)].u.pv.pvseg->pv
#define seg_lv(seg, s)		(seg)->areas[(s)].u.lv.lv
#define seg_metalv(seg, s)	(seg)->meta_areas[(s)].u.lv.lv
#define seg_metatype(seg, s)	(seg)->meta_areas[(s)].type

struct pe_range {
	struct dm_list list;
	uint32_t start;		/* PEs */
	uint32_t count;		/* PEs */
};

struct pv_list {
	struct dm_list list;
	struct physical_volume *pv;
	struct dm_list *mdas;	/* Metadata areas */
	struct dm_list *pe_ranges;	/* Ranges of PEs e.g. for allocation */
};

struct lv_list {
	struct dm_list list;
	struct logical_volume *lv;
};

struct vg_list {
	struct dm_list list;
	struct volume_group *vg;
};

#define PV_PE_START_CALC ((uint64_t) -1) /* Calculate pe_start value */

struct pvcreate_restorable_params {
	const char *restorefile; /* 0 if no --restorefile option */
	struct id id; /* FIXME: redundant */
	struct id *idp; /* 0 if no --uuid option */
	uint64_t ba_start;
	uint64_t ba_size;
	uint64_t pe_start;
	uint32_t extent_count;
	uint32_t extent_size;
};

struct pvcreate_params {
	int zero;
	uint64_t size;
	uint64_t data_alignment;
	uint64_t data_alignment_offset;
	int pvmetadatacopies;
	uint64_t pvmetadatasize;
	int64_t labelsector;
	force_t force;
	unsigned yes;
	unsigned metadataignore;
	struct pvcreate_restorable_params rp;
};

struct lvresize_params {
	const char *vg_name;
	const char *lv_name;

	uint32_t stripes;
	uint32_t stripe_size;
	uint32_t mirrors;

	const struct segment_type *segtype;

	/* size */
	uint32_t extents;
	uint64_t size;
	int sizeargs;
	sign_t sign;
	uint64_t poolmetadatasize;
	sign_t poolmetadatasign;
	uint32_t poolmetadataextents;
	percent_type_t percent;

	enum {
		LV_ANY = 0,
		LV_REDUCE = 1,
		LV_EXTEND = 2
	} resize;

	int resizefs;
	int nofsck;

	int argc;
	char **argv;

	/* FIXME Deal with meaningless 'ac' */
	/* Arg counts & values */
	unsigned ac_policy;
	unsigned ac_stripes;
	uint32_t ac_stripes_value;
	unsigned ac_mirrors;
	uint32_t ac_mirrors_value;
	unsigned ac_stripesize;
	uint64_t ac_stripesize_value;
	alloc_policy_t ac_alloc;
	unsigned ac_no_sync;
	unsigned ac_force;

	const char *ac_type;
};

int pvcreate_single(struct cmd_context *cmd, const char *pv_name,
		    struct pvcreate_params *pp);
void pvcreate_params_set_defaults(struct pvcreate_params *pp);

/*
* Utility functions
*/
int vg_write(struct volume_group *vg);
int vg_commit(struct volume_group *vg);
void vg_revert(struct volume_group *vg);
struct volume_group *vg_read_internal(struct cmd_context *cmd, const char *vg_name,
				      const char *vgid, int warnings, int *consistent);
struct physical_volume *pv_read(struct cmd_context *cmd, const char *pv_name,
				int warnings,
				int scan_label_only);

#define get_pvs( cmd ) get_pvs_internal((cmd), NULL, NULL)
#define get_pvs_perserve_vg( cmd, pv_list, vg_list ) get_pvs_internal((cmd), (pv_list), (vg_list))

struct dm_list *get_pvs_internal(struct cmd_context *cmd,
		struct dm_list *pvslist, struct dm_list *vgslist);

/*
 * Add/remove LV to/from volume group
 */
int link_lv_to_vg(struct volume_group *vg, struct logical_volume *lv);
int unlink_lv_from_vg(struct logical_volume *lv);
void lv_set_visible(struct logical_volume *lv);
void lv_set_hidden(struct logical_volume *lv);

struct dm_list *get_vgnames(struct cmd_context *cmd, int include_internal);
struct dm_list *get_vgids(struct cmd_context *cmd, int include_internal);
int scan_vgs_for_pvs(struct cmd_context *cmd, int warnings);

int pv_write(struct cmd_context *cmd, struct physical_volume *pv, int allow_non_orphan);
int move_pv(struct volume_group *vg_from, struct volume_group *vg_to,
	    const char *pv_name);
int move_pvs_used_by_lv(struct volume_group *vg_from,
			struct volume_group *vg_to,
			const char *lv_name);
int is_global_vg(const char *vg_name);
int is_orphan_vg(const char *vg_name);
int is_real_vg(const char *vg_name);
int vg_missing_pv_count(const struct volume_group *vg);
int vgs_are_compatible(struct cmd_context *cmd,
		       struct volume_group *vg_from,
		       struct volume_group *vg_to);
uint32_t vg_lock_newname(struct cmd_context *cmd, const char *vgname);

int lv_resize_prepare(struct cmd_context *cmd, struct logical_volume *lv,
		      struct lvresize_params *lp, struct dm_list *pvh);
int lv_resize(struct cmd_context *cmd, struct logical_volume *lv,
	      struct lvresize_params *lp, struct dm_list *pvh);

/*
 * Return a handle to VG metadata.
 */
struct volume_group *vg_read(struct cmd_context *cmd, const char *vg_name,
			     const char *vgid, uint32_t flags);
struct volume_group *vg_read_for_update(struct cmd_context *cmd, const char *vg_name,
			 const char *vgid, uint32_t flags);

/* 
 * Test validity of a VG handle.
 */
uint32_t vg_read_error(struct volume_group *vg_handle);

/* pe_start and pe_end relate to any existing data so that new metadata
* areas can avoid overlap */
struct physical_volume *pv_create(const struct cmd_context *cmd,
				  struct device *dev,
				  uint64_t size,
				  unsigned long data_alignment,
				  unsigned long data_alignment_offset,
				  uint64_t label_sector,
				  unsigned pvmetadatacopies,
				  uint64_t pvmetadatasize,
				  unsigned metadataignore,
				  struct pvcreate_restorable_params *rp);

int pv_resize_single(struct cmd_context *cmd,
			     struct volume_group *vg,
			     struct physical_volume *pv,
			     const uint64_t new_size);

int pv_analyze(struct cmd_context *cmd, const char *pv_name,
	       uint64_t label_sector);

/* FIXME: move internal to library */
uint32_t pv_list_extents_free(const struct dm_list *pvh);

int vg_validate(struct volume_group *vg);
struct volume_group *vg_create(struct cmd_context *cmd, const char *vg_name);
int vg_remove_mdas(struct volume_group *vg);
int vg_remove_check(struct volume_group *vg);
void vg_remove_pvs(struct volume_group *vg);
int vg_remove(struct volume_group *vg);
int vg_rename(struct cmd_context *cmd, struct volume_group *vg,
	      const char *new_name);
int vg_extend(struct volume_group *vg, int pv_count, const char *const *pv_names,
	      struct pvcreate_params *pp);
int vg_reduce(struct volume_group *vg, const char *pv_name);
int vg_change_tag(struct volume_group *vg, const char *tag, int add_tag);
int vg_split_mdas(struct cmd_context *cmd, struct volume_group *vg_from,
		  struct volume_group *vg_to);
/* FIXME: Investigate refactoring these functions to take a pv ISO pv_list */
void add_pvl_to_vgs(struct volume_group *vg, struct pv_list *pvl);
void del_pvl_from_vgs(struct volume_group *vg, struct pv_list *pvl);

/* FIXME: refactor / unexport when lvremove liblvm refactoring dones */
int remove_lvs_in_vg(struct cmd_context *cmd,
		     struct volume_group *vg,
		     force_t force);

/*
 * free_pv_fid() must be called on every struct physical_volume allocated
 * by pv_create, pv_read, find_pv_by_name or to free it when no longer required.
 */
void free_pv_fid(struct physical_volume *pv);

/* Manipulate LVs */
struct logical_volume *lv_create_empty(const char *name,
				       union lvid *lvid,
				       uint64_t status,
				       alloc_policy_t alloc,
				       struct volume_group *vg);

/* Write out LV contents */
int set_lv(struct cmd_context *cmd, struct logical_volume *lv,
	   uint64_t sectors, int value);

int lv_change_tag(struct logical_volume *lv, const char *tag, int add_tag);

/* Reduce the size of an LV by extents */
int lv_reduce(struct logical_volume *lv, uint32_t extents);

/* Empty an LV prior to deleting it */
int lv_empty(struct logical_volume *lv);

/* Empty an LV and add error segment */
int replace_lv_with_error_segment(struct logical_volume *lv);

/* Entry point for all LV extent allocations */
int lv_extend(struct logical_volume *lv,
	      const struct segment_type *segtype,
	      uint32_t stripes, uint32_t stripe_size,
	      uint32_t mirrors, uint32_t region_size,
	      uint32_t extents, const char *thin_pool_name,
	      struct dm_list *allocatable_pvs, alloc_policy_t alloc);

/* lv must be part of lv->vg->lvs */
int lv_remove(struct logical_volume *lv);

int lv_remove_single(struct cmd_context *cmd, struct logical_volume *lv,
		     force_t force);

int lv_remove_with_dependencies(struct cmd_context *cmd, struct logical_volume *lv,
				force_t force, unsigned level);

int lv_rename(struct cmd_context *cmd, struct logical_volume *lv,
	      const char *new_name);
int lv_rename_update(struct cmd_context *cmd, struct logical_volume *lv,
		     const char *new_name, int update_mda);

uint64_t extents_from_size(struct cmd_context *cmd, uint64_t size,
			   uint32_t extent_size);

struct logical_volume *find_pool_lv(struct logical_volume *lv);
int pool_is_active(const struct logical_volume *pool_lv);
int pool_can_resize_metadata(const struct logical_volume *pool_lv);
int update_pool_lv(struct logical_volume *lv, int activate);
int update_profilable_pool_params(struct cmd_context *cmd, struct profile *profile,
				  int passed_args, uint32_t *chunk_size,
				  thin_discards_t *discards, int *zero);
int update_pool_params(struct volume_group *vg, unsigned attr, int passed_args,
		       uint32_t data_extents, uint32_t extent_size,
		       uint32_t *chunk_size, thin_discards_t *discards,
		       uint64_t *pool_metadata_size, int *zero);
int get_pool_discards(const char *str, thin_discards_t *discards);
const char *get_pool_discards_name(thin_discards_t discards);
struct logical_volume *alloc_pool_metadata(struct logical_volume *pool_lv,
					   const char *name, uint32_t read_ahead,
					   uint32_t stripes, uint32_t stripe_size,
					   uint64_t size, alloc_policy_t alloc,
					   struct dm_list *pvh);
int handle_pool_metadata_spare(struct volume_group *vg, uint32_t extents,
			       struct dm_list *pvh, int poolmetadataspare);
int vg_set_pool_metadata_spare(struct logical_volume *lv);
int vg_remove_pool_metadata_spare(struct volume_group *vg);

int attach_thin_external_origin(struct lv_segment *seg,
				struct logical_volume *external_lv);
int detach_thin_external_origin(struct lv_segment *seg);
int attach_pool_metadata_lv(struct lv_segment *pool_seg,
                            struct logical_volume *pool_metadata_lv);
int detach_pool_metadata_lv(struct lv_segment *pool_seg,
                            struct logical_volume **pool_metadata_lv);
int attach_pool_data_lv(struct lv_segment *pool_seg,
                        struct logical_volume *pool_data_lv);
int is_mirror_image_removable(struct logical_volume *mimage_lv, void *baton);

/*
 * Activation options
 */
typedef enum activation_change {
	CHANGE_AY = 0,  /* activate */
	CHANGE_AN = 1,  /* deactivate */
	CHANGE_AE = 2,  /* activate exclusively */
	CHANGE_ALY = 3, /* activate locally */
	CHANGE_ALN = 4, /* deactivate locally */
	CHANGE_AAY = 5  /* automatic activation */
} activation_change_t;

/* Returns true, when change activates device */
static inline int is_change_activating(activation_change_t change)
{
        return ((change != CHANGE_AN) && (change != CHANGE_ALN));
}

/* FIXME: refactor and reduce the size of this struct! */
struct lvcreate_params {
	/* flags */
	int snapshot; /* snap */
	int thin; /* thin */
	int create_thin_pool; /* thin */
	int zero; /* all */
	int major; /* all */
	int minor; /* all */
	int log_count; /* mirror */
	int nosync; /* mirror */
	int poolmetadataspare; /* thin pool */
#define ACTIVATION_SKIP_SET		0x01 /* request to set LV activation skip flag state */
#define ACTIVATION_SKIP_SET_ENABLED	0x02 /* set the LV activation skip flag state to 'enabled' */
#define ACTIVATION_SKIP_IGNORE		0x04 /* request to ignore LV activation skip flag (if any) */
	int activation_skip; /* activation skip flags */
	activation_change_t activate; /* non-snapshot, non-mirror */
	thin_discards_t discards;     /* thin */

	const char *origin; /* snap */
	const char *pool;   /* thin */
	const char *vg_name; /* all */
	const char *lv_name; /* all */

	/* Keep args given by the user on command line */
	/* FIXME: create some more universal solution here */
#define PASS_ARG_CHUNK_SIZE		0x01
#define PASS_ARG_DISCARDS		0x02
#define PASS_ARG_POOL_METADATA_SIZE	0x04
#define PASS_ARG_ZERO			0x08
	int passed_args;

	uint32_t stripes; /* striped */
	uint32_t stripe_size; /* striped */
	uint32_t chunk_size; /* snapshot */
	uint32_t region_size; /* mirror */

	uint32_t mirrors; /* mirror */

	uint32_t min_recovery_rate; /* RAID */
	uint32_t max_recovery_rate; /* RAID */

	const struct segment_type *segtype; /* all */
	unsigned target_attr; /* all */

	/* size */
	uint32_t extents; /* all */
	uint32_t voriginextents; /* snapshot */
	uint64_t voriginsize; /* snapshot */
	uint32_t poolmetadataextents; /* thin pool */
	uint64_t poolmetadatasize; /* thin pool */
	struct dm_list *pvh; /* all */

	uint32_t permission; /* all */
	uint32_t read_ahead; /* all */
	alloc_policy_t alloc; /* all */

	struct dm_list tags;	/* all */
};

struct logical_volume *lv_create_single(struct volume_group *vg,
					struct lvcreate_params *lp);

/*
 * The activation can be skipped for selected LVs. Some LVs are skipped
 * by default (e.g. thin snapshots), others can be skipped on demand by
 * overriding the default behaviour. The flag that causes the activation
 * skip on next activations is stored directly in metadata for each LV
 * as ACTIVATION_SKIP flag.
 */
void lv_set_activation_skip(struct logical_volume *lv, int override_default, int add_skip_flag);
int lv_activation_skip(struct logical_volume *lv, activation_change_t activate,
		       int override_lv_skip_flag, int skip);

/*
 * Functions for layer manipulation
 */
int insert_layer_for_segments_on_pv(struct cmd_context *cmd,
				    struct logical_volume *lv_where,
				    struct logical_volume *layer_lv,
				    uint64_t status,
				    struct pv_list *pv,
				    struct dm_list *lvs_changed);
int remove_layers_for_segments(struct cmd_context *cmd,
			       struct logical_volume *lv,
			       struct logical_volume *layer_lv,
			       uint64_t status_mask, struct dm_list *lvs_changed);
int remove_layers_for_segments_all(struct cmd_context *cmd,
				   struct logical_volume *layer_lv,
				   uint64_t status_mask,
				   struct dm_list *lvs_changed);
int split_parent_segments_for_layer(struct cmd_context *cmd,
				    struct logical_volume *layer_lv);
int remove_layer_from_lv(struct logical_volume *lv,
			 struct logical_volume *layer_lv);
struct logical_volume *insert_layer_for_lv(struct cmd_context *cmd,
					   struct logical_volume *lv_where,
					   uint64_t status,
					   const char *layer_suffix);

/* Find a PV within a given VG */
struct pv_list *find_pv_in_vg(const struct volume_group *vg,
			      const char *pv_name);
struct pv_list *find_pv_in_vg_by_uuid(const struct volume_group *vg,
				      const struct id *id);

/* Find an LV within a given VG */
struct lv_list *find_lv_in_vg(const struct volume_group *vg,
			      const char *lv_name);

/* FIXME Merge these functions with ones above */
struct logical_volume *find_lv(const struct volume_group *vg,
			       const char *lv_name);
struct physical_volume *find_pv_by_name(struct cmd_context *cmd,
					const char *pv_name,
					int allow_orphan);

const char *find_vgname_from_pvname(struct cmd_context *cmd,
				    const char *pvname);
const char *find_vgname_from_pvid(struct cmd_context *cmd,
				  const char *pvid);

int lv_is_on_pv(struct logical_volume *lv, struct physical_volume *pv);
int lv_is_on_pvs(struct logical_volume *lv, struct dm_list *pvs);
int get_pv_list_for_lv(struct dm_pool *mem,
		       struct logical_volume *lv, struct dm_list *pvs);


/* Find LV segment containing given LE */
struct lv_segment *first_seg(const struct logical_volume *lv);
struct lv_segment *last_seg(const struct logical_volume *lv);


/*
* Useful functions for managing snapshots.
*/
int lv_is_origin(const struct logical_volume *lv);
int lv_is_virtual_origin(const struct logical_volume *lv);
int lv_is_cow(const struct logical_volume *lv);
int lv_is_merging_origin(const struct logical_volume *origin);
int lv_is_merging_cow(const struct logical_volume *snapshot);
uint32_t cow_max_extents(const struct logical_volume *origin, uint32_t chunk_size);
int lv_is_cow_covering_origin(const struct logical_volume *lv);

/* Test if given LV is visible from user's perspective */
int lv_is_visible(const struct logical_volume *lv);

int pv_is_in_vg(struct volume_group *vg, struct physical_volume *pv);

struct lv_segment *find_merging_snapshot(const struct logical_volume *origin);

/* Given a cow LV, return return the snapshot lv_segment that uses it */
struct lv_segment *find_snapshot(const struct logical_volume *lv);

/* Given a cow LV, return its origin */
struct logical_volume *origin_from_cow(const struct logical_volume *lv);

void init_snapshot_seg(struct lv_segment *seg, struct logical_volume *origin,
		       struct logical_volume *cow, uint32_t chunk_size, int merge);

int init_snapshot_merge(struct lv_segment *snap_seg, struct logical_volume *origin);

void clear_snapshot_merge(struct logical_volume *origin);

int vg_add_snapshot(struct logical_volume *origin, struct logical_volume *cow,
		    union lvid *lvid, uint32_t extent_count,
		    uint32_t chunk_size);

int vg_remove_snapshot(struct logical_volume *cow);

int vg_check_status(const struct volume_group *vg, uint64_t status);


/*
 * Check if the VG reached maximal LVs count (if set)
 */
int vg_max_lv_reached(struct volume_group *vg);

/*
* Mirroring functions
*/
int get_default_region_size(struct cmd_context *cmd);  /* in lv_manip.c */
struct lv_segment *find_mirror_seg(struct lv_segment *seg);
int lv_add_mirrors(struct cmd_context *cmd, struct logical_volume *lv,
		   uint32_t mirrors, uint32_t stripes, uint32_t stripe_size,
		   uint32_t region_size, uint32_t log_count,
		   struct dm_list *pvs, alloc_policy_t alloc, uint32_t flags);
int lv_split_mirror_images(struct logical_volume *lv, const char *split_lv_name,
			   uint32_t split_count, struct dm_list *removable_pvs);
int lv_remove_mirrors(struct cmd_context *cmd, struct logical_volume *lv,
		      uint32_t mirrors, uint32_t log_count,
		      int (*is_removable)(struct logical_volume *, void *),
		      void *removable_baton, uint64_t status_mask);

int is_temporary_mirror_layer(const struct logical_volume *lv);
struct logical_volume * find_temporary_mirror(const struct logical_volume *lv);
uint32_t lv_mirror_count(const struct logical_volume *lv);
uint32_t adjusted_mirror_region_size(uint32_t extent_size, uint32_t extents,
				    uint32_t region_size);
int remove_mirrors_from_segments(struct logical_volume *lv,
				 uint32_t new_mirrors, uint64_t status_mask);
int add_mirrors_to_segments(struct cmd_context *cmd, struct logical_volume *lv,
			    uint32_t mirrors, uint32_t region_size,
			    struct dm_list *allocatable_pvs, alloc_policy_t alloc);

int remove_mirror_images(struct logical_volume *lv, uint32_t num_mirrors,
			 int (*is_removable)(struct logical_volume *, void *),
			 void *removable_baton, unsigned remove_log);
int add_mirror_images(struct cmd_context *cmd, struct logical_volume *lv,
		      uint32_t mirrors, uint32_t stripes, uint32_t stripe_size, uint32_t region_size,
		      struct dm_list *allocatable_pvs, alloc_policy_t alloc,
		      uint32_t log_count);
struct logical_volume *detach_mirror_log(struct lv_segment *seg);
int attach_mirror_log(struct lv_segment *seg, struct logical_volume *lv);
int remove_mirror_log(struct cmd_context *cmd, struct logical_volume *lv,
		      struct dm_list *removable_pvs, int force);
int add_mirror_log(struct cmd_context *cmd, struct logical_volume *lv,
		   uint32_t log_count, uint32_t region_size,
		   struct dm_list *allocatable_pvs, alloc_policy_t alloc);

#if 0
/* FIXME: reconfigure_mirror_images: remove this code? */
int reconfigure_mirror_images(struct lv_segment *mirrored_seg, uint32_t num_mirrors,
			      struct dm_list *removable_pvs, unsigned remove_log);
#endif
int collapse_mirrored_lv(struct logical_volume *lv);
int shift_mirror_images(struct lv_segment *mirrored_seg, unsigned mimage);

/* ++  metadata/replicator_manip.c */
int replicator_add_replicator_dev(struct logical_volume *replicator_lv,
				  struct lv_segment *rdev_seg);
struct logical_volume *replicator_remove_replicator_dev(struct lv_segment *rdev_seg);
int replicator_add_rlog(struct lv_segment *replicator_seg, struct logical_volume *rlog_lv);
struct logical_volume *replicator_remove_rlog(struct lv_segment *replicator_seg);

int replicator_dev_add_slog(struct replicator_device *rdev, struct logical_volume *slog_lv);
struct logical_volume *replicator_dev_remove_slog(struct replicator_device *rdev);
int replicator_dev_add_rimage(struct replicator_device *rdev, struct logical_volume *lv);
struct logical_volume *replicator_dev_remove_rimage(struct replicator_device *rdev);

int lv_is_active_replicator_dev(const struct logical_volume *lv);
int lv_is_replicator(const struct logical_volume *lv);
int lv_is_replicator_dev(const struct logical_volume *lv);
int lv_is_rimage(const struct logical_volume *lv);
int lv_is_slog(const struct logical_volume *lv);
struct logical_volume *first_replicator_dev(const struct logical_volume *lv);
/* --  metadata/replicator_manip.c */

/* ++  metadata/raid_manip.c */
int lv_is_raid_with_tracking(const struct logical_volume *lv);
uint32_t lv_raid_image_count(const struct logical_volume *lv);
int lv_raid_change_image_count(struct logical_volume *lv,
			       uint32_t new_count, struct dm_list *pvs);
int lv_raid_split(struct logical_volume *lv, const char *split_name,
		  uint32_t new_count, struct dm_list *splittable_pvs);
int lv_raid_split_and_track(struct logical_volume *lv,
			    struct dm_list *splittable_pvs);
int lv_raid_merge(struct logical_volume *lv);
int lv_raid_reshape(struct logical_volume *lv,
		    const struct segment_type *new_segtype);
int lv_raid_replace(struct logical_volume *lv, struct dm_list *remove_pvs,
		    struct dm_list *allocate_pvs);
int lv_raid_remove_missing(struct logical_volume *lv);

/* --  metadata/raid_manip.c */

struct cmd_vg *cmd_vg_add(struct dm_pool *mem, struct dm_list *cmd_vgs,
			  const char *vg_name, const char *vgid,
			  uint32_t flags);
struct cmd_vg *cmd_vg_lookup(struct dm_list *cmd_vgs,
			     const char *vg_name, const char *vgid);
int cmd_vg_read(struct cmd_context *cmd, struct dm_list *cmd_vgs);
void free_cmd_vgs(struct dm_list *cmd_vgs);

int find_replicator_vgs(struct logical_volume *lv);

int lv_read_replicator_vgs(struct logical_volume *lv);
void lv_release_replicator_vgs(struct logical_volume *lv);

struct logical_volume *find_pvmove_lv(struct volume_group *vg,
				      struct device *dev, uint64_t lv_type);
struct logical_volume *find_pvmove_lv_from_pvname(struct cmd_context *cmd,
						  struct volume_group *vg,
						  const char *name,
						  const char *uuid,
						  uint64_t lv_type);
struct logical_volume *find_pvmove_lv_in_lv(struct logical_volume *lv);
const char *get_pvmove_pvname_from_lv(struct logical_volume *lv);
const char *get_pvmove_pvname_from_lv_mirr(struct logical_volume *lv_mirr);
struct dm_list *lvs_using_lv(struct cmd_context *cmd, struct volume_group *vg,
			  struct logical_volume *lv);

uint32_t find_free_lvnum(struct logical_volume *lv);
percent_t copy_percent(const struct logical_volume *lv_mirr);
char *generate_lv_name(struct volume_group *vg, const char *format,
		       char *buffer, size_t len);

/*
* Begin skeleton for external LVM library
*/
int pv_change_metadataignore(struct physical_volume *pv, uint32_t mda_ignore);


int vg_check_write_mode(struct volume_group *vg);
#define vg_is_clustered(vg) (vg_status((vg)) & CLUSTERED)
#define vg_is_exported(vg) (vg_status((vg)) & EXPORTED_VG)
#define vg_is_resizeable(vg) (vg_status((vg)) & RESIZEABLE_VG)

int lv_has_unknown_segments(const struct logical_volume *lv);
int vg_has_unknown_segments(const struct volume_group *vg);

int vg_mark_partial_lvs(struct volume_group *vg, int clear);

struct vgcreate_params {
	const char *vg_name;
	uint32_t extent_size;
	size_t max_pv;
	size_t max_lv;
	alloc_policy_t alloc;
	int clustered; /* FIXME: put this into a 'status' variable instead? */
	uint32_t vgmetadatacopies;
};

int vgcreate_params_validate(struct cmd_context *cmd,
			     struct vgcreate_params *vp);

int validate_vg_rename_params(struct cmd_context *cmd,
			      const char *vg_name_old,
			      const char *vg_name_new);
#endif
