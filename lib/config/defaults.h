/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEFAULTS_H
#define _LVM_DEFAULTS_H


#define DEFAULT_SYS_DIR "/etc/lvm"

#define DEFAULT_ARCHIVE_ENABLED 1
#define DEFAULT_BACKUP_ENABLED 1

#define DEFAULT_ARCHIVE_SUBDIR "archive"
#define DEFAULT_BACKUP_SUBDIR "backup"

#define DEFAULT_ARCHIVE_DAYS 30
#define DEFAULT_ARCHIVE_NUMBER 10

#define DEFAULT_DEV_DIR "/dev"
#define DEFAULT_PROC_DIR "/proc"

#define DEFAULT_LOCK_DIR "/var/lock/lvm"

#define DEFAULT_UMASK 0077

#ifdef READLINE_SUPPORT
  #define DEFAULT_MAX_HISTORY 100
#endif


#endif /* _LVM_DEFAULTS_H */
