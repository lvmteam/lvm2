/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef DISK_REP_FORMAT1_H
#define DISK_REP_FORMAT1_H

#include "lvm-types.h"
#include "metadata.h"
#include "pool.h"


#define SECTOR_SIZE 512

#define MAX_PV 256
#define MAX_LV 256
#define MAX_VG 99

#define MIN_PE_SIZE ( 8192L / SECTOR_SIZE)     /* 8 KB in sectors */
#define MAX_PE_SIZE ( 16L * 1024L * 1024L / SECTOR_SIZE * 1024)
#define PE_SIZE_PV_SIZE_REL 5   /* PV size must be at least 5 times PE size */

#define UNMAPPED_EXTENT 0

/* volume group */
#define	VG_ACTIVE            0x01	/* vg_status */
#define	VG_EXPORTED          0x02	/*     "     */
#define	VG_EXTENDABLE        0x04	/*     "     */

#define	VG_READ              0x01	/* vg_access */
#define	VG_WRITE             0x02	/*     "     */
#define	VG_CLUSTERED         0x04	/*     "     */
#define	VG_SHARED            0x08	/*     "     */

/* logical volume */
#define	LV_ACTIVE            0x01	/* lv_status */
#define	LV_SPINDOWN          0x02	/*     "     */

#define	LV_READ              0x01	/* lv_access */
#define	LV_WRITE             0x02	/*     "     */
#define	LV_SNAPSHOT          0x04	/*     "     */
#define	LV_SNAPSHOT_ORG      0x08	/*     "     */

#define	LV_BADBLOCK_ON       0x01	/* lv_badblock */

#define	LV_STRICT            0x01	/* lv_allocation */
#define	LV_CONTIGUOUS        0x02	/*       "       */

/* physical volume */
#define	PV_ACTIVE            0x01	/* pv_status */
#define	PV_ALLOCATABLE       0x02	/* pv_allocatable */


struct data_area {
	uint32_t base;
	uint32_t size;
};

struct pv_disk {
        uint8_t id[2];
        uint16_t version;               /* lvm version */
        struct data_area pv_on_disk;
        struct data_area vg_on_disk;
        struct data_area pv_uuidlist_on_disk;
        struct data_area lv_on_disk;
        struct data_area pe_on_disk;
        uint8_t pv_uuid[NAME_LEN];
        uint8_t vg_name[NAME_LEN];
        uint8_t system_id[NAME_LEN];    /* for vgexport/vgimport */
        uint32_t pv_major;
        uint32_t pv_number;
        uint32_t pv_status;
        uint32_t pv_allocatable;
        uint32_t pv_size;
        uint32_t lv_cur;
        uint32_t pe_size;
        uint32_t pe_total;
        uint32_t pe_allocated;

	/* only present on version == 2 pv's */
	uint32_t pe_start;
};

struct lv_disk {
        uint8_t lv_name[NAME_LEN];
        uint8_t vg_name[NAME_LEN];
        uint32_t lv_access;
        uint32_t lv_status;
        uint32_t lv_open;
        uint32_t lv_dev;
        uint32_t lv_number;
        uint32_t lv_mirror_copies; /* for future use */
        uint32_t lv_recovery;      /*       "        */
        uint32_t lv_schedule;      /*       "        */
        uint32_t lv_size;
        uint32_t lv_snapshot_minor; /* minor number of original */
        uint16_t lv_chunk_size;     /* chunk size of snapshot */
        uint16_t dummy;
        uint32_t lv_allocated_le;
        uint32_t lv_stripes;
        uint32_t lv_stripesize;
        uint32_t lv_badblock;   /* for future use */
        uint32_t lv_allocation;
        uint32_t lv_io_timeout; /* for future use */
        uint32_t lv_read_ahead;
};

struct vg_disk {
        uint8_t vg_uuid[ID_LEN]; /* volume group UUID */
        uint8_t vg_name_dummy[NAME_LEN - ID_LEN]; /* rest of v1 VG name */
        uint32_t vg_number;     /* volume group number */
        uint32_t vg_access;     /* read/write */
        uint32_t vg_status;     /* active or not */
        uint32_t lv_max;	/* maximum logical volumes */
        uint32_t lv_cur;	/* current logical volumes */
        uint32_t lv_open;	/* open logical volumes */
        uint32_t pv_max;	/* maximum physical volumes */
        uint32_t pv_cur;	/* current physical volumes FU */
        uint32_t pv_act;	/* active physical volumes */
        uint32_t dummy;
        uint32_t vgda;          /* volume group descriptor arrays FU */
        uint32_t pe_size;	/* physical extent size in sectors */
        uint32_t pe_total;	/* total of physical extents */
        uint32_t pe_allocated;  /* allocated physical extents */
        uint32_t pvg_total;     /* physical volume groups FU */
};

struct pe_disk {
	uint16_t lv_num;
	uint16_t le_num;
};


struct uuid_list {
	struct list_head list;
	char uuid[NAME_LEN];
};

struct lvd_list {
	struct list_head list;
	struct lv_disk lv;
};

struct disk_list {
	struct pool *mem;
	struct device *dev;
	struct list_head list;

	struct pv_disk pv;
	struct vg_disk vg;
	struct list_head uuids;
	struct list_head lvs;
	struct pe_disk *extents;
};


/*
 * Layout constants.
 */
#define METADATA_ALIGN 4096UL
#define	PE_ALIGN (65536UL / SECTOR_SIZE)

#define	METADATA_BASE 0UL
#define	PV_SIZE 1024UL
#define	VG_SIZE 4096UL


/*
 * Functions to calculate layout info.
 */
int calculate_layout(struct disk_list *dl);
int calculate_extent_count(struct physical_volume *pv);


/*
 * Low level io routines which read/write
 * disk_lists.
 */
struct disk_list *read_pv(struct device *dev, struct pool *mem,
			  const char *vg_name);

int read_pvs_in_vg(const char *vg_name, struct dev_filter *filter,
		   struct pool *mem, struct list_head *results);

int write_pvs(struct list_head *pvs);


/*
 * Functions to translate to between disk and in
 * core structures.
 */
int import_pv(struct pool *mem, struct device *dev,
	      struct physical_volume *pv, struct pv_disk *pvd);
int export_pv(struct pv_disk *pvd, struct physical_volume *pv);

int import_vg(struct pool *mem,
	      struct volume_group *vg, struct disk_list *dl);
int export_vg(struct vg_disk *vgd, struct volume_group *vg);

int import_lv(struct pool *mem, struct logical_volume *lv,
	      struct lv_disk *lvd);
void export_lv(struct lv_disk *lvd, struct volume_group *vg,
	       struct logical_volume *lv, const char *prefix);

int import_extents(struct pool *mem, struct volume_group *vg,
		   struct list_head *pvs);
int export_extents(struct disk_list *dl, int lv_num,
		   struct logical_volume *lv,
		   struct physical_volume *pv);

int import_pvs(struct pool *mem, struct list_head *pvs,
	       struct list_head *results, int *count);

int import_lvs(struct pool *mem, struct volume_group *vg,
	       struct list_head *pvs);
int export_lvs(struct disk_list *dl, struct volume_group *vg,
	       struct physical_volume *pv, const char *prefix);

int export_uuids(struct disk_list *dl, struct volume_group *vg);

void export_numbers(struct list_head *pvs, struct volume_group *vg);

void export_pv_act(struct list_head *pvs);

/* blech */
int get_free_vg_number(struct dev_filter *filter, const char *candidate_vg,
		       int *result);
int export_vg_number(struct list_head *pvs, const char *vg_name,
		     struct dev_filter *filter);


#endif
