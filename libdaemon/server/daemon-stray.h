/*
 * Copyright (C) 2024 Red Hat, Inc.
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

#ifndef _LVM_DAEMON_STRAY_H
#define _LVM_DAEMON_STRAY_H

/*
 * needs dm -> #include "device_mapper/all.h"
 * needs logging -> #include "libdm/misc/dmlib.h"
 */
#include "lib/misc/lvm-file.h"

/*
 * When compiling with valgrind pool support, skip closing descriptors
 * as there is couple more of them being held by valgrind itself.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <limits.h>

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#endif

static void _daemon_get_cmdline(pid_t pid, char *cmdline, size_t size)
{
	char buf[sizeof(DEFAULT_PROC_DIR) + 32];
	int fd, n = 0;

	snprintf(buf, sizeof(buf), DEFAULT_PROC_DIR "/%u/cmdline", pid);
	/* FIXME Use generic read code. */
	if ((fd = open(buf, O_RDONLY)) >= 0) {
		if ((n = read(fd, cmdline, size - 1)) < 0) {
			//perror("read", buf);
			n = 0;
		}
		(void) close(fd);
	}
	cmdline[n] = '\0';
}

static void _daemon_get_filename(int fd, char *filename, size_t size)
{
	char buf[sizeof(DEFAULT_PROC_DIR) + 32];
	ssize_t lsize;

	snprintf(buf, sizeof(buf), DEFAULT_PROC_DIR "/self/fd/%u", fd);

	if ((lsize = readlink(buf, filename, size - 1)) == -1)
		filename[0] = '\0';
	else
		filename[lsize] = '\0';
}

static void _daemon_close_descriptor(int fd, unsigned suppress_warnings,
				     const char *command, pid_t ppid,
				     const char *parent_cmdline)
{
	char filename[PATH_MAX];
	int r;

	/* Ignore bad file descriptors */
	if (!is_valid_fd(fd))
		return;

	if (!suppress_warnings)
		_daemon_get_filename(fd, filename, sizeof(filename));

	r = close(fd);
	if ((fd <= STDERR_FILENO) || suppress_warnings)
		return;

	if (!r)
		fprintf(stderr, "File descriptor %d (%s) leaked on "
			"%s invocation.", fd, filename, command);
	else if (errno == EBADF)
		return;
	else
		fprintf(stderr, "Close failed on stray file descriptor "
			"%d (%s): %s", fd, filename, strerror(errno));

	fprintf(stderr, " Parent PID %d: %s\n", (int)ppid, parent_cmdline);
}

/* Close all stray descriptor except custom fds.
 * Note: when 'from_fd' is set to -1,  unused 'custom_fds' must use same value!
 *
 * command:		print command name with warning message
 * suppress_warning:	whether to print warning messages
 * above_fd:		close all descriptors above this descriptor
 * custom_fds:		preserve descriptors from this set of descriptors
 */
static int daemon_close_stray_fds(const char *command, int suppress_warning,
				  int from_fd, const struct custom_fds *custom_fds)
{
	struct rlimit rlim;
	int fd;
	unsigned suppress_warnings = 0;
	pid_t ppid = getppid();
	char parent_cmdline[64];
	static const char _fd_dir[] = DEFAULT_PROC_DIR "/self/fd";
	struct dirent *dirent;
	DIR *d;

#ifdef HAVE_VALGRIND
	if (RUNNING_ON_VALGRIND)
		/* Skipping close of descriptors within valgrind execution. */
		return 1;
#endif /* HAVE_VALGRIND */
	_daemon_get_cmdline(ppid, parent_cmdline, sizeof(parent_cmdline));

	if ((d = opendir(_fd_dir))) {
		/* Discover opened descriptors from /proc/self/fd listing */
		while ((dirent = readdir(d))) {
			fd = atoi(dirent->d_name);
			if ((fd > from_fd) &&
			    (fd != dirfd(d)) &&
			    (fd != custom_fds->out) &&
			    (fd != custom_fds->err) &&
			    (fd != custom_fds->report))
				_daemon_close_descriptor(fd, suppress_warnings,
							 command, ppid, parent_cmdline);
		}

		(void) closedir(d);
	} else if (errno == ENOENT) {
		/* Path does not exist, use the old way */
		if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
			fd = 256; /* just have to guess */
		else if ((fd = (int)rlim.rlim_cur) > 65536)
			fd = 65536; /* do not bother with more then 64K fds */

		while (--fd > from_fd) {
			if ((fd != custom_fds->out) &&
			    (fd != custom_fds->err) &&
			    (fd != custom_fds->report))
				_daemon_close_descriptor(fd, suppress_warnings, command,
							 ppid, parent_cmdline);
		}
	} else
		return 0; /* broken system */

	return 1;
}

#endif
