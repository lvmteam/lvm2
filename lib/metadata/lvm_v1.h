/*
 * Copyright (C) 2001 Sistina Software
 *
 * lvm is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * lvm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU CC; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#ifndef _LVM_V1_H_INCLUDE
#define _LVM_V1_H_INCLUDE

#ifdef __KERNEL__
#include <linux/kdev_t.h>
#include <linux/list.h>
#else
#define __KERNEL__
#include <linux/kdev_t.h>
#include <linux/list.h>
#undef __KERNEL__
#endif                          /* #ifndef __KERNEL__ */

#include <asm/types.h>
#include <linux/major.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <asm/page.h>

#include <sys/types.h>
#include <linux/version.h>

typedef unsigned long blkoff_t;

#ifndef uint8_t
#  define uint8_t       unsigned char
#endif
#ifndef uint16_t
#  define uint16_t      unsigned short int
#endif
#ifndef uint32_t
#  define uint32_t      unsigned int
#endif
#ifndef uint64_t
#  define uint64_t      unsigned long long int
#endif

/*
 * This is basically the on disk metadata layout version.
 */
#define LVM_STRUCT_VERSION 2

/*
 * Limits for VGs, PVs per VG and LVs per VG.  The ABS_* limit's are
 * used in defining the data structures, MAX_* are the real limits
 * used.
 */
#define MAX_VG  99
#define MAX_PV  256
#define MAX_LV  256     /* caused by 8 bit minor */

#define NAME_LEN 128            /* the maximum length of various names */
#define UUID_LEN 32             /* some components have unique identifiers */


/*
 * A little struct to hold a sector range on a block device.  Used to
 * hold the location of on disk metadata
 */
typedef struct {
        uint32_t base;
        uint32_t size;
} lvm_disk_data_t;


/***********************************************************
 * On disk representation:
 *
 * The beginning of each PV contains metadata, known int the Volume
 * Group Data Area (VGDA).  This metadata has the following structure:
 *
 *  offset              description                         size
 *   ---------------   ----------------------------------  ------------
 *   0                 physical volume structure           ~500 byte
 *   1K                volume group structure              ~200 byte
 *   6K                namelist of physical volumes        128 byte each
 *  + n * ~300byte      n logical volume structures         ~300 byte each
 *   + m * 4byte       m physical extent alloc. structs    4 byte each
 *  + ~ 1 PE size       physical extents                    total * size
 *
 * Spaces are left between the structures for later extensions.
 ***********************************************************/

/*
 * physical volume - disk
 */
typedef struct {
	uint8_t id[2];		             /* identifier */
	uint16_t version;	             /* lvm struct version */

	/* these define the locations of various bits of on disk metadata */
	lvm_disk_data_t pv_on_disk;          /* pv_disk_t location */
	lvm_disk_data_t vg_on_disk;          /* vg_disk_t location */
	lvm_disk_data_t pv_uuidlist_on_disk; /* uuid list location */
	lvm_disk_data_t lv_on_disk;          /* lv_disk_t locations */
	lvm_disk_data_t pe_on_disk;          /* pe mapping table location */

	uint8_t pv_uuid[NAME_LEN];           /* uuid for this PV */
	uint8_t vg_name[NAME_LEN];           /* which vg it belongs to */
	uint8_t system_id[NAME_LEN];         /* for vgexport/vgimport */
	uint32_t pv_major;
	uint32_t pv_number;
	uint32_t pv_status;
	uint32_t pv_allocatable;
	uint32_t pv_size;
	uint32_t lv_cur;
	uint32_t pe_size;
	uint32_t pe_total;
	uint32_t pe_allocated;

	/* data_location.base holds the start of the pe's in sectors */
	uint32_t pe_start;
} pv_disk_t;

/*
 * logical volume - disk
 */
typedef struct {
	uint8_t lv_name[NAME_LEN];
	uint8_t vg_name[NAME_LEN];
	uint32_t lv_access;
	uint32_t lv_status;
	uint32_t lv_open;
	uint32_t lv_dev;
	uint32_t lv_number;
	uint32_t lv_mirror_copies;	/* for future use */
	uint32_t lv_recovery;	        /*       "        */
	uint32_t lv_schedule;	        /*       "        */
	uint32_t lv_size;
	uint32_t lv_snapshot_minor;     /* minor number of origin */
	uint16_t lv_chunk_size;	        /* chunk size of snapshot */
	uint16_t dummy;
	uint32_t lv_allocated_le;
	uint32_t lv_stripes;
	uint32_t lv_stripesize;
	uint32_t lv_badblock;	        /* for future use */
	uint32_t lv_allocation;
	uint32_t lv_io_timeout;	        /* for future use */
	uint32_t lv_read_ahead;
} lv_disk_t;

/*
 * volume group - disk
 */
typedef struct {
	uint8_t vg_uuid[UUID_LEN];	/* volume group UUID */

        /* rest of v1 VG name */
	uint8_t vg_name_dummy[NAME_LEN-UUID_LEN];
	uint32_t vg_number;	/* volume group number */
	uint32_t vg_access;	/* read/write */
	uint32_t vg_status;	/* active or not */
	uint32_t lv_max;	/* maximum logical volumes */
	uint32_t lv_cur;	/* current logical volumes */
	uint32_t lv_open;	/* open logical volumes */
	uint32_t pv_max;	/* maximum physical volumes */
	uint32_t pv_cur;	/* current physical volumes FU */
	uint32_t pv_act;	/* active physical volumes */
	uint32_t dummy;
	uint32_t vgda;		/* volume group descriptor arrays FU */
	uint32_t pe_size;	/* physical extent size in sectors */
	uint32_t pe_total;	/* total of physical extents */
	uint32_t pe_allocated;	/* allocated physical extents */
	uint32_t pvg_total;	/* physical volume groups FU */
} vg_disk_t;

/*
 * pe mapping - disk
 */
typedef struct {
	uint16_t lv_num;
	uint16_t le_num;
} pe_disk_t;

/*
 * copy on write tables - disk
 */
typedef struct lv_COW_table_disk_v1 {
	uint64_t pv_org_number;
	uint64_t pv_org_rsector;
	uint64_t pv_snap_number;
	uint64_t pv_snap_rsector;
} lv_COW_table_disk_t;



#endif

