/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "log.h"
#include "lvm-file.h"
#include "lvm-string.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>

/*
 * Creates a temporary filename, and opens a descriptor to the
 * file.  Both the filename and descriptor are needed so we can
 * rename the file after successfully writing it.  Grab
 * NFS-supported exclusive fcntl discretionary lock.
 */
int create_temp_name(const char *dir, char *buffer, size_t len,
			     int *fd)
{
	int i, num;
	pid_t pid;
	char hostname[255];
	struct flock lock = {
		l_type: F_WRLCK,
		l_whence: 0,
		l_start: 0,
		l_len: 0
	};

	num = rand();
	pid = getpid();
	if (gethostname(hostname, sizeof(hostname)) < 0) {
		log_sys_error("gethostname", "");
		strcpy(hostname, "nohostname");
	}

	for (i = 0; i < 20; i++, num++) {

		if (lvm_snprintf(buffer, len, "%s/.lvm_%s_%d_%d",
				 dir, hostname, pid, num) == -1) {
			log_err("Not enough space to build temporary file "
				"string.");
			return 0;
		}

		*fd = open(buffer, O_CREAT | O_EXCL | O_WRONLY | O_APPEND,
			   S_IRUSR | S_IRGRP | S_IROTH |
			   S_IWUSR | S_IWGRP | S_IWOTH);
		if (*fd < 0)
			continue;

		if (!fcntl(*fd, F_SETLK, &lock))
			return 1;

		close(*fd);
	}

	return 0;
}

/*
 * NFS-safe rename of a temporary file to a common name, designed
 * to avoid race conditions and not overwrite the destination if
 * it exists.
 *
 * Try to create the new filename as a hard link to the original.
 * Check the link count of the original file to see if it worked.
 * (Assumes nothing else touches our temporary file!)  If it
 * worked, unlink the old filename.
 */
int lvm_rename(const char *old, const char *new)
{
	struct stat buf;

	link(old, new);

	if (stat(old, &buf)) {
		log_sys_error("stat", old);
		return 0;
	}

	if (buf.st_nlink != 2) {
		log_error("%s: rename to %s failed", old, new);
		return 0;
	}

	if (unlink(old)) {
		log_sys_error("unlink", old);
		return 0;
	}

	return 1;
}

