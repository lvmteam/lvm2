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

#include "lib/misc/lib.h"
#include "lib/misc/lvm-file.h"

#include <unistd.h>
#include <sys/file.h>
#include <fcntl.h>
#include <dirent.h>

/*
 * Creates a temporary filename, and opens a descriptor to the
 * file.  Both the filename and descriptor are needed so we can
 * rename the file after successfully writing it.  Grab
 * NFS-supported exclusive fcntl discretionary lock.
 */
int create_temp_name(const char *dir, char *buffer, size_t len, int *fd,
		     unsigned *seed)
{
	const struct flock lock = { .l_type = F_WRLCK };
	int i, num;
	pid_t pid;
	char hostname[255];
	char *p;

	num = rand_r(seed);
	pid = getpid();
	if (gethostname(hostname, sizeof(hostname)) < 0) {
		log_sys_error("gethostname", "");
		dm_strncpy(hostname, "nohostname", sizeof(hostname));
	} else {
		/* Replace any '/' with '?' found in the hostname. */
		p = hostname;
		while ((p = strchr(p, '/')))
			*p = '?';
	}

	for (i = 0; i < 20; i++, num++) {

		if (dm_snprintf(buffer, len, "%s/.lvm_%s_%d_%d",
				 dir, hostname, pid, num) == -1) {
			log_error("Not enough space to build temporary file "
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

		if (close(*fd))
			log_sys_error("close", buffer);
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

	if (unlink(old) && (errno != ENOENT)) {
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

int dir_create(const char *path, int mode)
{
	int r;

	log_debug("Creating directory %s.", path);

	dm_prepare_selinux_context(path, S_IFDIR);
	r = mkdir(path, mode);
	dm_prepare_selinux_context(NULL, 0);

	if (r == 0)
		return 1;

	if (errno == EEXIST) {
		if (dir_exists(path))
			return 1;

		log_error("Path %s is not a directory.", path);
	} else
		log_sys_error("mkdir", path);

	return 0;
}

int dir_create_recursive(const char *path, int mode)
{
	int r = 0;
	char *orig, *s;

	orig = s = strdup(path);
	if (!s) {
		log_error("Failed to duplicate directory path %s.", path);
		return 0;
	}

	while ((s = strchr(s, '/')) != NULL) {
		*s = '\0';
		if (*orig && !dir_exists(orig) && !dir_create(orig, mode))
			goto_out;
		*s++ = '/';
	}

	if (!dir_exists(path) && !dir_create(path, mode))
		goto_out;
	r = 1;
out:
	free(orig);

	return r;
}

void sync_dir(const char *file)
{
	int fd;
	char *dir, *c;

	if (!(dir = strdup(file))) {
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

	if (close(fd))
		log_sys_error("close", dir);

      out:
	free(dir);
}

/*
 * Attempt to obtain fcntl lock on a file, if necessary creating file first
 * or waiting.
 * Returns file descriptor on success, else -1.
 * mode is F_WRLCK or F_RDLCK
 */
int fcntl_lock_file(const char *file, short lock_type, int warn_if_read_only)
{
	const struct flock lock = { .l_type = lock_type };
	int lockfd;
	char *dir;
	char *c;

	if (!(dir = strdup(file))) {
		log_error("fcntl_lock_file failed in strdup.");
		return -1;
	}

	if ((c = strrchr(dir, '/')))
		*c = '\0';

	if (!dm_create_dir(dir)) {
		free(dir);
		return -1;
	}

	free(dir);

	log_very_verbose("Locking %s (%s, %hd)", file,
			 (lock_type == F_WRLCK) ? "F_WRLCK" : "F_RDLCK",
			 lock_type);
	if ((lockfd = open(file, O_RDWR | O_CREAT, 0777)) < 0) {
		/* EACCES has been reported on NFS */
		if (warn_if_read_only || (errno != EROFS && errno != EACCES))
			log_sys_error("open", file);
		else
			stack;

		return -1;
	}

	if (fcntl(lockfd, F_SETLKW, &lock)) {
		log_sys_error("fcntl", file);
		if (close(lockfd))
			log_sys_error("close", file);
		return -1;
	}

	return lockfd;
}

void fcntl_unlock_file(int lockfd)
{
	const struct flock lock = { .l_type = F_UNLCK };

	log_very_verbose("Unlocking fd %d", lockfd);

	if (fcntl(lockfd, F_SETLK, &lock) == -1)
		log_sys_error("fcntl", "");

	if (close(lockfd))
		log_sys_error("close","");
}

int lvm_fclose(FILE *fp, const char *filename)
{
	if (!dm_fclose(fp))
		return 0;

	if (errno == 0)
		log_error("%s: write error", filename);
	else
		log_sys_error("write error", filename);

	return EOF;
}

void lvm_stat_ctim(struct timespec *ctim, const struct stat *buf)
{
#ifdef HAVE_STAT_ST_CTIM
	*ctim = buf->st_ctim;
#else
	ctim->tv_sec = buf->st_ctime;
	ctim->tv_nsec = 0;
#endif
}
