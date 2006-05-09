/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvm-file.h"
#include "lvm-string.h"

#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <dirent.h>

/*
 * Creates a temporary filename, and opens a descriptor to the
 * file.  Both the filename and descriptor are needed so we can
 * rename the file after successfully writing it.  Grab
 * NFS-supported exclusive fcntl discretionary lock.
 */
int create_temp_name(const char *dir, char *buffer, size_t len, int *fd)
{
	int i, num;
	pid_t pid;
	char hostname[255];
	struct flock lock = {
		.l_type = F_WRLCK,
		.l_whence = 0,
		.l_start = 0,
		.l_len = 0
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

	if (link(old, new)) {
		log_error("%s: rename to %s failed: %s", old, new,
			  strerror(errno));
		return 0;
	}

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

int path_exists(const char *path)
{
	struct stat info;

	if (!*path)
		return 0;

	if (stat(path, &info) < 0)
		return 0;

	return 1;
}

int dir_exists(const char *path)
{
	struct stat info;

	if (!*path)
		return 0;

	if (stat(path, &info) < 0)
		return 0;

	if (!S_ISDIR(info.st_mode))
		return 0;

	return 1;
}

static int _create_dir_recursive(const char *dir)
{
	char *orig, *s;
	int rc;

	log_verbose("Creating directory \"%s\"", dir);
	/* Create parent directories */
	orig = s = dm_strdup(dir);
	while ((s = strchr(s, '/')) != NULL) {
		*s = '\0';
		if (*orig) {
			rc = mkdir(orig, 0777);
			if (rc < 0 && errno != EEXIST) {
				if (errno != EROFS)
					log_sys_error("mkdir", orig);
				dm_free(orig);
				return 0;
			}
		}
		*s++ = '/';
	}
	dm_free(orig);

	/* Create final directory */
	rc = mkdir(dir, 0777);
	if (rc < 0 && errno != EEXIST) {
		if (errno != EROFS)
			log_sys_error("mkdir", dir);
		return 0;
	}
	return 1;
}

int create_dir(const char *dir)
{
	struct stat info;

	if (!*dir)
		return 1;

	if (stat(dir, &info) < 0)
		return _create_dir_recursive(dir);

	if (S_ISDIR(info.st_mode))
		return 1;

	log_error("Directory \"%s\" not found", dir);
	return 0;
}

int is_empty_dir(const char *dir)
{
	struct dirent *dirent;
	DIR *d;

	if (!(d = opendir(dir))) {
		log_sys_error("opendir", dir);
		return 0;
	}

	while ((dirent = readdir(d)))
		if (strcmp(dirent->d_name, ".") && strcmp(dirent->d_name, ".."))
			break;

	if (closedir(d)) {
		log_sys_error("closedir", dir);
	}

	return dirent ? 0 : 1;
}

void sync_dir(const char *file)
{
	int fd;
	char *dir, *c;

	if (!(dir = dm_strdup(file))) {
		log_error("sync_dir failed in strdup");
		return;
	}

	if (!dir_exists(dir)) {
		c = dir + strlen(dir);
		while (*c != '/' && c > dir)
			c--;

		if (c == dir)
			*c++ = '.';

		*c = '\0';
	}

	if ((fd = open(dir, O_RDONLY)) == -1) {
		log_sys_error("open", dir);
		goto out;
	}

	if (fsync(fd) && (errno != EROFS) && (errno != EINVAL))
		log_sys_error("fsync", dir);

	close(fd);

      out:
	dm_free(dir);
}
