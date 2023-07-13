/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _LVM_DEVICE_H
#define _LVM_DEVICE_H

#include "lib/uuid/uuid.h"

#include <fcntl.h>

#define DEV_REGULAR		0x00000002	/* Regular file? */
#define DEV_ALLOCED		0x00000004	/* malloc used */
#define DEV_OPENED_RW		0x00000008	/* Opened RW */
#define DEV_OPENED_EXCL		0x00000010	/* Opened EXCL */
#define DEV_O_DIRECT		0x00000020	/* Use O_DIRECT */
#define DEV_O_DIRECT_TESTED	0x00000040	/* DEV_O_DIRECT is reliable */
#define DEV_OPEN_FAILURE	0x00000080	/* Has last open failed? */
#define DEV_USED_FOR_LV		0x00000100	/* Is device used for an LV */
#define DEV_ASSUMED_FOR_LV	0x00000200	/* Is device assumed for an LV */
#define DEV_NOT_O_NOATIME	0x00000400	/* Don't use O_NOATIME */
#define DEV_IN_BCACHE		0x00000800      /* dev fd is open and used in bcache */
#define DEV_BCACHE_EXCL		0x00001000      /* bcache_fd should be open EXCL */
#define DEV_ADDED_SYS_WWID	0x00002000      /* wwid has been added from sysfs wwid file */
#define DEV_ADDED_VPD_WWIDS	0x00004000	/* wwids have been added from vpd_pg83 */
#define DEV_BCACHE_WRITE	0x00008000      /* bcache_fd is open with RDWR */
#define DEV_SCAN_FOUND_LABEL	0x00010000      /* label scan read dev and found label */
#define DEV_IS_MD_COMPONENT	0x00020000	/* device is an md component */
#define DEV_IS_NVME		0x00040000	/* set if dev is nvme */
#define DEV_MATCHED_USE_ID	0x00080000	/* matched an entry from cmd->use_devices */
#define DEV_SCAN_FOUND_NOLABEL	0x00100000	/* label_scan read, passed filters, but no lvm label */
#define DEV_SCAN_NOT_READ	0x00200000	/* label_scan not able to read dev */

/*
 * Support for external device info.
 * Any new external device info source needs to be
 * registered using EXT_REGISTER macro in dev-ext.c.
 */
typedef enum dev_ext_e {
	DEV_EXT_NONE,
	DEV_EXT_UDEV,
	DEV_EXT_NUM
} dev_ext_t;

struct dev_ext {
	int enabled;
	dev_ext_t src;
	void *handle;
};

#define DEV_ID_TYPE_SYS_WWID   1
#define DEV_ID_TYPE_SYS_SERIAL 2
#define DEV_ID_TYPE_MPATH_UUID 3
#define DEV_ID_TYPE_MD_UUID    4
#define DEV_ID_TYPE_LOOP_FILE  5
#define DEV_ID_TYPE_CRYPT_UUID 6
#define DEV_ID_TYPE_LVMLV_UUID 7
#define DEV_ID_TYPE_DEVNAME    8
#define DEV_ID_TYPE_WWID_NAA   9
#define DEV_ID_TYPE_WWID_EUI  10
#define DEV_ID_TYPE_WWID_T10  11

/* Max length of WWID_NAA, WWID_EUI, WWID_T10 */
#define DEV_WWID_SIZE 128

/*
 * A wwid read from:
 * /sys/dev/block/%d:%d/device/wwid
 * /sys/dev/block/%d:%d/wwid
 * /sys/dev/block/%d:%d/device/vpd_pg83
 */

struct dev_wwid {
	struct dm_list list;     /* dev->wwids */
	int type;                /* 1,2,3 for NAA,EUI,T10 */
	char id[DEV_WWID_SIZE];  /* includes prefix naa.,eui.,t10. */
};

/*
 * A device ID of a certain type for a device.
 * A struct device may have multiple dev_id structs on dev->ids.
 * One of them will be the one that's used, pointed to by dev->id.
 */

struct dev_id {
	struct dm_list list;    /* dev->ids */
	struct device *dev;
	uint16_t idtype;	/* DEV_ID_TYPE_ */
	char *idname;		/* id string determined by idtype */
};

/*
 * A device listed in devices file that lvm should use.
 * Each entry in the devices file is represented by a struct dev_use.
 * The structs are kept on cmd->use_devices.
 * idtype/idname/pvid/part are set when reading the devices file.
 * du->dev is set when a struct dev_use is matched to a struct device.
 */

struct dev_use {
	struct dm_list list;
	struct device *dev;
	int part;
	uint16_t idtype;
	char *idname;
	char *devname;
	char *pvid;
};

struct dev_use_list {
	struct dm_list list;
	struct dev_use *du;
};

/*
 * All devices in LVM will be represented by one of these.
 * pointer comparisons are valid.
 */
struct device {
	struct dm_list aliases;	/* struct dm_str_list */
	struct dm_list wwids; /* struct dev_wwid, used for multipath component detection */
	struct dm_list ids; /* struct dev_id, different entries for different idtypes */
	struct dev_id *id; /* points to the the ids entry being used for this dev */
	dev_t dev;

	/* private */
	int fd;
	int open_count;
	int physical_block_size; /* From BLKPBSZGET: lowest possible sector size that the hardware can operate on without reverting to read-modify-write operations */
	int logical_block_size;  /* From BLKSSZGET: lowest possible block size that the storage device can address */
	int read_ahead;
	int bcache_fd;
	int bcache_di;
	int part;		/* partition number */
	uint32_t flags;
	uint32_t filtered_flags;
	unsigned size_seqno;
	uint64_t size;
	uint64_t end;
	struct dev_ext ext;
	const char *duplicate_prefer_reason;

	const char *vgid; /* if device is an LV */
	const char *lvid; /* if device is an LV */

	char pvid[ID_LEN + 1]; /* if device is a PV */
	char _padding[7];
};

/*
 * All I/O is annotated with the reason it is performed.
 */
typedef enum dev_io_reason {
	DEV_IO_SIGNATURES = 0,	/* Scanning device signatures */
	DEV_IO_LABEL,		/* LVM PV disk label */
	DEV_IO_MDA_HEADER,	/* Text format metadata area header */
	DEV_IO_MDA_CONTENT,	/* Text format metadata area content */
	DEV_IO_MDA_EXTRA_HEADER,	/* Header of any extra metadata areas on device */
	DEV_IO_MDA_EXTRA_CONTENT,	/* Content of any extra metadata areas on device */
	DEV_IO_FMT1,		/* Original LVM1 metadata format */
	DEV_IO_POOL,		/* Pool metadata format */
	DEV_IO_LV,		/* Content written to an LV */
	DEV_IO_LOG		/* Logging messages */
} dev_io_reason_t;

struct device_list {
	struct dm_list list;
	struct device *dev;
};

struct device_id_list {
	struct dm_list list;
	struct device *dev;
	char pvid[ID_LEN + 1];
};

struct device_area {
	struct device *dev;
	uint64_t start;		/* Bytes */
	uint64_t size;		/* Bytes */
};

/*
 * Support for external device info.
 */
const char *dev_ext_name(struct device *dev);
int dev_ext_enable(struct device *dev, dev_ext_t src);
int dev_ext_disable(struct device *dev);
struct dev_ext *dev_ext_get(struct device *dev);
int dev_ext_release(struct device *dev);

/*
 * Increment current dev_size_seqno.
 * This is used to control lifetime
 * of cached device size.
 */
void dev_size_seqno_inc(void);

/*
 * All io should use these routines.
 */
int dev_get_direct_block_sizes(struct device *dev, unsigned int *physical_block_size,
                               unsigned int *logical_block_size);
int dev_get_size(struct device *dev, uint64_t *size);
int dev_get_read_ahead(struct device *dev, uint32_t *read_ahead);
int dev_discard_blocks(struct device *dev, uint64_t offset_bytes, uint64_t size_bytes);

/* Use quiet version if device number could change e.g. when opening LV */
int dev_open(struct device *dev);
int dev_open_quiet(struct device *dev);
int dev_open_flags(struct device *dev, int flags, int direct, int quiet);
int dev_open_readonly(struct device *dev);
int dev_open_readonly_buffered(struct device *dev);
int dev_open_readonly_quiet(struct device *dev);
int dev_close(struct device *dev);
int dev_close_immediate(struct device *dev);

int dev_fd(struct device *dev);
const char *dev_name(const struct device *dev);

void dev_flush(struct device *dev);

struct device *dev_create_file(const char *filename, struct device *dev,
			       struct dm_str_list *alias, int use_malloc);
void dev_destroy_file(struct device *dev);

int dev_mpath_init(const char *config_wwids_file);
void dev_mpath_exit(void);
int parse_vpd_ids(const unsigned char *vpd_data, int vpd_datalen, struct dm_list *ids);
int format_t10_id(const unsigned char *in, size_t in_bytes, unsigned char *out, size_t out_bytes);
int format_general_id(const char *in, size_t in_bytes, unsigned char *out, size_t out_bytes);
int parse_vpd_serial(const unsigned char *in, char *out, size_t outsize);

/* dev_util */
int device_id_list_remove(struct dm_list *devices, struct device *dev);
struct device_id_list *device_id_list_find_dev(struct dm_list *devices, struct device *dev);
int device_list_remove(struct dm_list *devices, struct device *dev);
struct device_list *device_list_find_dev(struct dm_list *devices, struct device *dev);

#endif
