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

#define MAX_PV 256
#define MAX_LV 256
#define MAX_VG 99

#define UNMAPPED_EXTENT ((uint16_t) -1)

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
	char uuid[NAME_LEN + 1];
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

struct disk_list *read_pv(struct device *dev, struct pool *mem,
			  const char *vg_name);

int read_pvs_in_vg(const char *vg_name, struct dev_filter *filter,
		   struct pool *mem, struct list_head *results);

int write_pvs(struct list_head *pvs);

#endif
