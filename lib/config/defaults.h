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

#define DEFAULT_LOCK_DIR "/var/lock/lvm"
#define DEFAULT_LOCKING_LIB "lvm2_locking.so"

#define DEFAULT_UMASK 0077

#ifdef LVM1_SUPPORT
  #define DEFAULT_FORMAT "lvm1"
#else
  #define DEFAULT_FORMAT "lvm2"
#endif

#define DEFAULT_PVMETADATASIZE 255
#define DEFAULT_PVMETADATACOPIES 1
#define DEFAULT_LABELSECTOR 1

#define DEFAULT_MSG_PREFIX "  "
#define DEFAULT_CMD_NAME 0
#define DEFAULT_OVERWRITE 0

#ifndef DEFAULT_LOG_FACILITY
  #define DEFAULT_LOG_FACILITY LOG_USER
#endif

#define DEFAULT_SYSLOG 1
#define DEFAULT_VERBOSE 0
#define DEFAULT_LOGLEVEL 0
#define DEFAULT_INDENT 1

#define DEFAULT_ACTIVATION 1

#ifdef READLINE_SUPPORT
  #define DEFAULT_MAX_HISTORY 100
#endif

#endif				/* _LVM_DEFAULTS_H */
