/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#ifndef _LVM_DEFAULTS_H
#define _LVM_DEFAULTS_H


#define DEFAULT_SYS_DIR "/etc/lvm"

#define DEFAULT_ARCHIVE_FLAG 1
#define DEFAULT_BACKUP_FLAG 1

#define DEFAULT_ARCHIVE_SUBDIR "archive"
#define DEFAULT_BACKUP_SUBDIR "backup"

/*
 * FIXME: these are deliberately low for the beta
 * series to encourage testing.
 */
#define DEFAULT_ARCHIVE_DAYS 7
#define DEFAULT_ARCHIVE_NUMBER 5

#define DEFAULT_DEV_DIR "/dev"


#endif /* _LVM_DEFAULTS_H */
