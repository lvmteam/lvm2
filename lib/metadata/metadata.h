/*
 * Copyright (C) 2001 Sistina Software
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

#define ID_LEN 32
#define NAME_LEN 128

/* Various flags */
/* Note that the bits no longer necessarily correspond to LVM1 disk format */

#define STATUS_ACTIVE            0x01  /* PV VG LV */

#define STATUS_EXPORTED          0x02  /* VG */
#define STATUS_EXTENDABLE        0x04  /* VG */

#define STATUS_ALLOCATED         0x02  /* PV */

#define STATUS_SPINDOWN          0x02  /* LV */
#define STATUS_BADBLOCK_ON       0x04  /* LV */
#define STATUS_ALLOC_STRICT      0x08  /* LV */
#define STATUS_ALLOC_CONTIGUOUS  0x10  /* LV */

#define ACCESS_READ              0x01  /* LV VG */
#define ACCESS_WRITE             0x02  /* LV VG */

#define ACCESS_SNAPSHOT          0x04  /* LV */
#define ACCESS_SNAPSHOT_ORG      0x08  /* LV */

#define ACCESS_CLUSTERED         0x04  /* VG */
#define ACCESS_SHARED            0x08  /* VG */



#define EXPORTED_TAG "PV_EXP"  /* Identifier of exported PV */
#define IMPORTED_TAG "PV_IMP"  /* Identifier of imported PV */



struct id {
	__uint8_t uuid[ID_LEN];
};

struct physical_volume {
        struct id *id;
	struct device *dev;
	char *vg_name;
	char *exported;

        __uint32_t status;
        __uint64_t size;

        /* physical extents */
        __uint64_t pe_size;
        __uint64_t pe_start;
        __uint32_t pe_count;
        __uint32_t pe_allocated;
};

struct pe_specifier {
        struct physical_volume *pv;
        __uint32_t pe;
};

struct logical_volume {
        /* disk */
	struct id *id;
        char *name;

        __uint32_t access;
        __uint32_t status;
        __uint32_t open;

        __uint64_t size;
        __uint32_t le_count;

        /* le -> pe mapping array */
        struct pe_specifier *map;
};

struct volume_group {
	struct id *id;
	char *name;

        __uint32_t status;
        __uint32_t access;

        __uint64_t extent_size;
        __uint32_t extent_count;
        __uint32_t free_count;

        __uint32_t max_lv;
        __uint32_t max_pv;

        /* physical volumes */
        __uint32_t pv_count;
        struct physical_volume **pv;

        /* logical volumes */
        __uint32_t lv_count;
        struct logical_volume **lv;
};

struct string_list {
	struct list_head list;
	char * string;
};

struct pv_list {
	struct list_head list;
	struct physical_volume pv;
};

/* ownership of returned objects passes */
struct io_space {
	struct string_list *(*get_vgs)(struct io_space *is);
	struct pv_list *(*get_pvs)(struct io_space *is);

	struct physical_volume *(*pv_read)(struct io_space *is,
					struct device *dev);
	int (*pv_write)(struct io_space *is, struct physical_volume *pv);

	struct volume_group *(*vg_read)(struct io_space *is,
					const char *vg_name);
	int (*vg_write)(struct io_space *is, struct volume_group *vg);
	void (*destroy)(struct io_space *is);

	struct dev_filter *filter;
	void *private;
};

/* FIXME: Move to other files */
struct io_space *create_text_format(struct dev_filter *filter,
				    const char *text_file);
struct io_space *create_lvm_v1_format(struct dev_filter *filter);

inline int write_backup(struct io_space *orig, struct io_space *text)
{

}


int id_eq(struct id *op1, struct id *op2);

struct volume_group *vg_create();
struct physical_volume *pv_create();

int vg_destroy(struct volume_group *vg);

int pv_add(struct volume_group *vg, struct physical_volume *pv);
int pv_remove(struct volume_group *vg, struct physical_volume *pv);
struct physical_volume *pv_find(struct volume_group *vg,
				const char *pv_name);

int lv_add(struct volume_group *vg, struct logical_volume *lv);
int lv_remove(struct volume_group *vg, struct logical_volume *lv);
struct logical_volume *lv_find(struct volume_group *vg,
			       const char *lv_name);

#endif
