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

/* FIXME: LVM1-format-specific stuff should be in lvm_v1.h instead */

#ifndef _LVM_METADATA_H
#define _LVM_METADATA_H

#include "metadata/lvm_v1.h"

/***********************************************************
 * In core data representation.
 ***********************************************************/

/*
 * physical volume - core
 */
typedef struct pv {
	char id[2];		/* Identifier */
	unsigned short version;	/* HM lvm version */
	lvm_disk_data_t pv_on_disk;
	lvm_disk_data_t vg_on_disk;
	lvm_disk_data_t pv_uuidlist_on_disk;
	lvm_disk_data_t lv_on_disk;
	lvm_disk_data_t pe_on_disk;
	char pv_name[NAME_LEN];
	char vg_name[NAME_LEN];
	char system_id[NAME_LEN];	/* for vgexport/vgimport */
	kdev_t pv_dev;
	uint pv_number;
	uint pv_status;
	uint pv_allocatable;
	uint pv_size;		/* HM */
	uint lv_cur;
	uint pe_size;
	uint pe_total;
	uint pe_allocated;
	uint pe_stale;		/* for future use */
	pe_disk_t *pe;		/* HM */
	struct block_device *bd;
	char pv_uuid[UUID_LEN+1];
#ifdef __KERNEL__
#else
	uint32_t pe_start;
	char dummy[39];
#endif
} pv_t;


/*
 * extent descriptor - core
 */
typedef struct {
	kdev_t dev;
	uint32_t pe;		/* to be changed if > 2TB */
	uint32_t reads;
	uint32_t writes;
} pe_t;

/*
 * block exception descriptor for snapshots - core
 */
typedef struct lv_block_exception_v1 {
	struct list_head hash;
	uint32_t rsector_org;
	kdev_t   rdev_org;
	uint32_t rsector_new;
	kdev_t   rdev_new;
} lv_block_exception_t;


/*
 * logical volume - core
 */
typedef struct lv {
	char lv_name[NAME_LEN];
	char vg_name[NAME_LEN];
	uint lv_access;
	uint lv_status;
	uint lv_open;
	kdev_t lv_dev;
	uint lv_number;
	uint lv_mirror_copies;	/* for future use */
	uint lv_recovery;	/*       "        */
	uint lv_schedule;	/*       "        */
	uint lv_size;
	pe_t *lv_current_pe;
	uint lv_current_le;	/* for future use */
	uint lv_allocated_le;
	uint lv_stripes;
	uint lv_stripesize;
	uint lv_badblock;	/* for future use */
	uint lv_allocation;
	uint lv_io_timeout;	/* for future use */
	uint lv_read_ahead;

	/* delta to version 1 starts here */
	struct lv *lv_snapshot_org;
	struct lv *lv_snapshot_prev;
	struct lv *lv_snapshot_next;
	lv_block_exception_t *lv_block_exception;
	uint lv_remap_ptr;
	uint lv_remap_end;
	uint lv_chunk_size;
	uint lv_snapshot_minor;
	uint32_t chunk_shift;
	uint32_t chunk_mask;

} lv_t;

/*
 * volume group - core
 */
typedef struct {
	char vg_name[NAME_LEN];	/* volume group name */
	uint vg_number;		/* volume group number */
	uint vg_access;		/* read/write */
	uint vg_status;		/* active or not */
	uint lv_max;		/* maximum logical volumes */
	uint lv_cur;		/* current logical volumes */
	uint lv_open;		/* open    logical volumes */
	uint pv_max;		/* maximum physical volumes */
	uint pv_cur;		/* current physical volumes FU */
	uint pv_act;		/* active physical volumes */
	uint dummy;		/* was obsolete max_pe_per_pv */
	uint vgda;		/* volume group descriptor arrays FU */
	uint pe_size;		/* physical extent size in sectors */
	uint pe_total;		/* total of physical extents */
	uint pe_allocated;	/* allocated physical extents */
	uint pvg_total;		/* physical volume groups FU */
	struct proc_dir_entry *proc;
	pv_t *pv[MAX_PV];	/* physical volume struct pointers */
	lv_t *lv[MAX_LV];	/* logical  volume struct pointers */
	char vg_uuid[UUID_LEN+1];	/* volume group UUID */
	uint32_t pe_shift;
	uint32_t pe_mask;

	struct proc_dir_entry *vg_dir_pde;
	struct proc_dir_entry *lv_subdir_pde;
	struct proc_dir_entry *pv_subdir_pde;
} vg_t;


/***********************************************************
 * Status flags
 **********************************************************/

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


/* misc */
#define LVM_SNAPSHOT_DROPPED_SECTOR 1


/***********************************************************
 * Constants and limits
 **********************************************************/

/*
 * LVM_PE_T_MAX corresponds to:
 * 8KB PE size can map a ~512 MB logical volume at the cost of 1MB memory,
 * 128MB PE size can map a 8TB logical volume at the same cost of memory.
 *
 * Default PE size of 4 MB gives a maximum logical volume size of 256 GB.
 * Maximum PE size of 16GB gives a maximum logical volume size of 1024 TB.
 *
 * AFAIK, the actual kernels limit this to 1 TB.
 *
 * Should be a sufficient spectrum
 */

/* This is the usable size of pe_disk_t.le_num */
#define	LVM_PE_T_MAX ((1 << (sizeof(uint16_t) * 8)) - 2)

/* FIXME: these numbers look like they could get too big */
#define	LVM_LV_SIZE_MAX(a) \
((long long) LVM_PE_T_MAX * (a)->pe_size > \
 (long long) 1024 * 1024 / SECTOR_SIZE * 1024 * 1024 ? \
 (long long) 1024 * 1024 / SECTOR_SIZE * 1024 * 1024 : \
 (long long) LVM_PE_T_MAX * (a)->pe_size)

#define	LVM_MIN_PE_SIZE	(8192L / SECTOR_SIZE) /* 8 KB in sectors */

/* 16GB in sectors */
#define	LVM_MAX_PE_SIZE	((16L * 1024L * 1024L / SECTOR_SIZE) * 1024)

/* 4 MB in sectors */
#define	LVM_DEFAULT_PE_SIZE (4096L * 1024 / SECTOR_SIZE)

#define	LVM_DEFAULT_STRIPE_SIZE	16L /* 16 KB  */

/* PAGESIZE in sectors */
#define	LVM_MIN_STRIPE_SIZE (PAGE_SIZE / SECTOR_SIZE)

/* 512 KB in sectors */
#define	LVM_MAX_STRIPE_SIZE (512L * 1024 / SECTOR_SIZE)

#define	LVM_MAX_STRIPES	128	/* max # of stripes */

/* 1TB[sectors] */
#define	LVM_MAX_SIZE (1024LU * 1024 / SECTOR_SIZE * 1024 * 1024)

#define	LVM_MAX_MIRRORS 2	     /* future use */
#define	LVM_MIN_READ_AHEAD 2	     /* minimum read ahead sectors */
#define	LVM_MAX_READ_AHEAD 120	     /* maximum read ahead sectors */
#define	LVM_MAX_LV_IO_TIMEOUT 60     /* seconds I/O timeout (future use) */
#define	LVM_PARTITION 0xfe	     /* LVM partition id */
#define	LVM_NEW_PARTITION 0x8e	     /* new LVM partition id (10/09/1999) */
#define	LVM_PE_SIZE_PV_SIZE_REL	5    /* max relation PV size and PE size */

#define	LVM_SNAPSHOT_MAX_CHUNK	1024 /* 1024 KB */
#define	LVM_SNAPSHOT_DEF_CHUNK	64   /* 64  KB */
#define	LVM_SNAPSHOT_MIN_CHUNK	(PAGE_SIZE / 1024) /* 4 or 8 KB */

#define lvm_version "device-mapper-1"

#ifdef _G_LSEEK64
int lseek64 ( unsigned int, unsigned long long, unsigned int);
#define llseek lseek64
#else
int llseek ( unsigned int, unsigned long long, unsigned int);
#endif

#define	LVM_ID		"HM"      /* Identifier PV (id in pv_t) */
#define	EXPORTED	"PV_EXP"  /* Identifier exported PV (system_id in pv_t) */
#define	IMPORTED	"PV_IMP"  /* Identifier imported PV (        "        ) */
#define	DISK_NAME_LEN		8
#define	LV_MIN_NAME_LEN		5
#define	LV_MAX_NAME_LEN		7
#define	MIN_PART		1
#define	MAX_PART	 	15

/* some metadata on the disk need to be aligned */
#define LVM_VGDA_ALIGN 4096UL

/* base of PV structure in disk partition */
#define	LVM_PV_DISK_BASE	0L

/* size reserved for PV structure on disk */
#define	LVM_PV_DISK_SIZE	1024L

/* base of VG structure in disk partition */
#define	LVM_VG_DISK_BASE round_up(LVM_PV_DISK_BASE + LVM_PV_DISK_SIZE, \
                                  LVM_VGDA_ALIGN)

/* size reserved for VG structure */
#define	LVM_VG_DISK_SIZE  	(8 * 512L)

/* name list of physical volumes on disk */
#define	LVM_PV_UUIDLIST_DISK_BASE round_up(LVM_VG_DISK_BASE + \
                                           LVM_VG_DISK_SIZE, LVM_VGDA_ALIGN)

/* now for the dynamically calculated parts of the VGDA */
#define	LVM_LV_DISK_OFFSET(a, b) ((a)->lv_on_disk.base + sizeof(lv_disk_t) * b)

#define	LVM_VGDA_SIZE(pv) ((pv)->pe_on_disk.base + (pv)->pe_on_disk.size)

#define	LVM_PE_ALIGN		 (65536UL / SECTOR_SIZE)

/* core <-> disk conversion macros */
#if __BYTE_ORDER == __BIG_ENDIAN
#define LVM_TO_CORE16(x) ( \
        ((uint16_t)((((uint16_t)(x) & 0x00FFU) << 8) | \
                    (((uint16_t)(x) & 0xFF00U) >> 8))))

#define LVM_TO_DISK16(x) LVM_TO_CORE16(x)

#define LVM_TO_CORE32(x) ( \
        ((uint32_t)((((uint32_t)(x) & 0x000000FFU) << 24) | \
                    (((uint32_t)(x) & 0x0000FF00U) << 8)  | \
                    (((uint32_t)(x) & 0x00FF0000U) >> 8)  | \
                    (((uint32_t)(x) & 0xFF000000U) >> 24))))

#define LVM_TO_DISK32(x) LVM_TO_CORE32(x)

#define LVM_TO_CORE64(x) \
        ((uint64_t)((((uint64_t)(x) & 0x00000000000000FFULL) << 56) | \
                    (((uint64_t)(x) & 0x000000000000FF00ULL) << 40) | \
                    (((uint64_t)(x) & 0x0000000000FF0000ULL) << 24) | \
                    (((uint64_t)(x) & 0x00000000FF000000ULL) <<  8) | \
                    (((uint64_t)(x) & 0x000000FF00000000ULL) >>  8) | \
                    (((uint64_t)(x) & 0x0000FF0000000000ULL) >> 24) | \
                    (((uint64_t)(x) & 0x00FF000000000000ULL) >> 40) | \
                    (((uint64_t)(x) & 0xFF00000000000000ULL) >> 56)))

#define LVM_TO_DISK64(x) LVM_TO_CORE64(x)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define LVM_TO_CORE16(x) x
#define LVM_TO_DISK16(x) x
#define LVM_TO_CORE32(x) x
#define LVM_TO_DISK32(x) x
#define LVM_TO_CORE64(x) x
#define LVM_TO_DISK64(x) x
#else
#error "__BYTE_ORDER must be defined as __LITTLE_ENDIAN or __BIG_ENDIAN"
#endif /* #if __BYTE_ORDER == __BIG_ENDIAN */

/* return codes */
#define LVM_VG_CFGBACKUP_NO_DIFF                                          100

#define BLOCK_SIZE 1024
#define SECTOR_SIZE 512

#define UNDEF -1

#ifndef min
#define min(a,b) (((a)<(b))?(a):(b))
#endif
#ifndef max
#define max(a,b) (((a)>(b))?(a):(b))
#endif


/* FIXME */
#include "dev-mgr/dev-manager.h"
pv_t *pv_read_lvm_v1(struct dev_mgr *dm, const char *pv_name);
pv_t *pv_read(struct dev_mgr *dm, const char *pv_name);
pe_disk_t *pv_read_pe(const char *pv_name, const pv_t *pv);
	
pe_disk_t *pv_read_pe_lvm_v1(const char *pv_name, const pv_t * pv);
lv_disk_t *pv_read_lvs(const pv_t *pv);
lv_disk_t *pv_read_lvs_lvm_v1(const pv_t *pv);

#endif

