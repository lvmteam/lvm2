/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEFAULTS_H
#define _LVM_DEFAULTS_H

#define DEFAULT_ARCHIVE_ENABLED 1
#define DEFAULT_BACKUP_ENABLED 1

#define DEFAULT_ARCHIVE_SUBDIR "archive"
#define DEFAULT_BACKUP_SUBDIR "backup"

#define DEFAULT_ARCHIVE_DAYS 30
#define DEFAULT_ARCHIVE_NUMBER 10

#define DEFAULT_SYS_DIR "/etc/lvm"
#define DEFAULT_DEV_DIR "/dev"
#define DEFAULT_PROC_DIR "/proc"
#define DEFAULT_SYSFS_SCAN 1

#define DEFAULT_LOCK_DIR "/var/lock/lvm"
#define DEFAULT_LOCKING_LIB "lvm2_locking.so"

#define DEFAULT_UMASK 0077

#ifdef LVM1_SUPPORT
#  define DEFAULT_FORMAT "lvm1"
#else
#  define DEFAULT_FORMAT "lvm2"
#endif

#define DEFAULT_STRIPESIZE 64	/* KB */
#define DEFAULT_PVMETADATASIZE 255
#define DEFAULT_PVMETADATACOPIES 1
#define DEFAULT_LABELSECTOR UINT64_C(1)

#define DEFAULT_MSG_PREFIX "  "
#define DEFAULT_CMD_NAME 0
#define DEFAULT_OVERWRITE 0

#ifndef DEFAULT_LOG_FACILITY
#  define DEFAULT_LOG_FACILITY LOG_USER
#endif

#define DEFAULT_SYSLOG 1
#define DEFAULT_VERBOSE 0
#define DEFAULT_LOGLEVEL 0
#define DEFAULT_INDENT 1
#define DEFAULT_UNITS "h"
#define DEFAULT_SUFFIX 1
#define DEFAULT_HOSTTAGS 0

#ifdef DEVMAPPER_SUPPORT
#  define DEFAULT_ACTIVATION 1
#  define DEFAULT_RESERVED_MEMORY 8192
#  define DEFAULT_RESERVED_STACK 256
#  define DEFAULT_PROCESS_PRIORITY -18
#else
#  define DEFAULT_ACTIVATION 0
#endif

#define DEFAULT_STRIPE_FILLER "/dev/ioerror"
#define DEFAULT_MIRROR_REGION_SIZE 512	/* KB */
#define DEFAULT_INTERVAL 15

#ifdef READLINE_SUPPORT
#  define DEFAULT_MAX_HISTORY 100
#endif

#define DEFAULT_REP_ALIGNED 1
#define DEFAULT_REP_BUFFERED 1
#define DEFAULT_REP_HEADINGS 1
#define DEFAULT_REP_SEPARATOR " "

#define DEFAULT_LVS_COLS "lv_name,vg_name,lv_attr,lv_size,origin,snap_percent,move_pv,move_percent"
#define DEFAULT_VGS_COLS "vg_name,pv_count,lv_count,snap_count,vg_attr,vg_size,vg_free"
#define DEFAULT_PVS_COLS "pv_name,vg_name,pv_fmt,pv_attr,pv_size,pv_free"
#define DEFAULT_SEGS_COLS "lv_name,vg_name,lv_attr,stripes,segtype,seg_size"

#define DEFAULT_LVS_COLS_VERB "lv_name,vg_name,seg_count,lv_attr,lv_size,lv_major,lv_minor,origin,snap_percent,move_pv,move_percent,lv_uuid"
#define DEFAULT_VGS_COLS_VERB "vg_name,vg_attr,vg_extent_size,pv_count,lv_count,snap_count,vg_size,vg_free,vg_uuid"
#define DEFAULT_PVS_COLS_VERB "pv_name,vg_name,pv_fmt,pv_attr,pv_size,pv_free,pv_uuid"
#define DEFAULT_SEGS_COLS_VERB "lv_name,vg_name,lv_attr,seg_start,seg_size,stripes,segtype,stripesize,chunksize"

#define DEFAULT_LVS_SORT "vg_name,lv_name"
#define DEFAULT_VGS_SORT "vg_name"
#define DEFAULT_PVS_SORT "pv_name"
#define DEFAULT_SEGS_SORT "vg_name,lv_name,seg_start"

#endif				/* _LVM_DEFAULTS_H */
