/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
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

/* Routines dealing with the System LV */

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <mntent.h>

#include "libdlm.h"
#include "log.h"
#include "list.h"
#include "locking.h"
#include "system-lv.h"
#include "clvm.h"
#include "clvmd-comms.h"
#include "clvmd.h"
#ifdef HAVE_CCS
#include "ccs.h"
#endif

#define SYSTEM_LV_FILESYSTEM "ext2"
#define SYSTEM_LV_MOUNTPOINT "/tmp/.clvmd-XXXXXX"

extern char *config_filename(void);

static char system_lv_name[PATH_MAX] = { '\0' };
static char mount_point[PATH_MAX] = { '\0' };
static int mounted = 0;
static int mounted_rw = 0;
static int lockid;
static const char *lock_name = "CLVM_SYSTEM_LV";

/* Look in /proc/mounts or (as a last resort) /etc/mtab to
   see if the system-lv is mounted. If it is mounted and we
   think it's not then abort because we don't have the right
   lock status and we don't know what other processes are doing with it.

   Returns 1 for mounted, 0 for not mounted so it matches the condition
   of the "mounted" static variable above.
*/
static int is_really_mounted(void)
{
	FILE *mountfile;
	struct mntent *ment;

	mountfile = setmntent("/proc/mounts", "r");
	if (!mountfile) {
		mountfile = setmntent("/etc/mtab", "r");
		if (!mountfile) {
			log_error("Unable to open /proc/mounts or /etc/mtab");
			return -1;
		}
	}

	/* Look for system LV name in the file */
	do {
		ment = getmntent(mountfile);
		if (ment) {
			if (strcmp(ment->mnt_fsname, system_lv_name) == 0) {
				endmntent(mountfile);
				return 1;
			}
		}
	}
	while (ment);

	endmntent(mountfile);
	return 0;
}

/* Get the system LV name from the config file */
static int find_system_lv(void)
{
	if (system_lv_name[0] == '\0') {
#ifdef HAVE_CCS
		int error;
		ccs_node_t *ctree;

		/* Read the cluster config file */
		/* Open the config file */
		error = open_ccs_file(&ctree, "clvm.ccs");
		if (error) {
			perror("reading config file");
			return -1;
		}

		strcpy(system_lv_name, find_ccs_str(ctree,
						    "cluster/systemlv", '/',
						    "/dev/vg/system_lv"));

		/* Finished with config file */
		close_ccs_file(ctree);
#else
		if (getenv("CLVMD_SYSTEM_LV"))
			strcpy(system_lv_name, getenv("CLVMD_SYSTEM_LV"));
		else
			return -1;
#endif
	}

	/* See if it has been mounted outside our control */
	if (is_really_mounted() != mounted) {
		log_error
		    ("The system LV state has been mounted/umounted outside the control of clvmd\n"
		     "it cannot not be used for cluster communications until this is fixed.\n");
		return -1;
	}
	return 0;
}

/* No prizes */
int system_lv_umount(void)
{
	if (!mounted)
		return 0;

	if (umount(mount_point) < 0) {
		log_error("umount of system LV (%s) failed: %m\n",
			  system_lv_name);
		return -1;
	}

	sync_unlock(lock_name, lockid);
	mounted = 0;

	/* Remove the mount point */
	rmdir(mount_point);

	return 0;
}

int system_lv_mount(int readwrite)
{
	int status;
	int saved_errno;
	int fd;

	if (find_system_lv()) {
		errno = EBUSY;
		return -1;
	}

	/* Is it already mounted suitably? */
	if (mounted) {
		if (!readwrite || (readwrite && mounted_rw)) {
			return 0;
		} else {
			/* Mounted RO and we need RW */
			if (system_lv_umount() < 0)
				return -1;
		}
	}

	/* Randomize the mount point */
	strcpy(mount_point, SYSTEM_LV_MOUNTPOINT);
	fd = mkstemp(mount_point);
	if (fd < 0) {
		log_error("mkstemp for system LV mount point failed: %m\n");
		return -1;
	}

	/* Race condition here but there's no mkstemp for directories */
	close(fd);
	unlink(mount_point);
	mkdir(mount_point, 0600);

	/* Make sure we have a system-lv lock */
	status =
	    sync_lock(lock_name, (readwrite) ? LKM_EXMODE : LKM_CRMODE, 0,
		      &lockid);
	if (status < 0)
		return -1;

	/* Mount it */
	if (mount(system_lv_name, mount_point, SYSTEM_LV_FILESYSTEM,
		  MS_MGC_VAL | MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_SYNCHRONOUS
		  | (readwrite ? 0 : MS_RDONLY), NULL) < 0) {
		/* mount(2) returns EINVAL if the volume has no FS on it. So, if we want to
		   write to it we try to make a filesystem in it and retry the mount */
		if (errno == EINVAL && readwrite) {
			char cmd[256];

			log_error("Attempting mkfs on system LV device %s\n",
				  system_lv_name);
			snprintf(cmd, sizeof(cmd), "/sbin/mkfs -t %s %s",
				 SYSTEM_LV_FILESYSTEM, system_lv_name);
			system(cmd);

			if (mount
			    (system_lv_name, mount_point, SYSTEM_LV_FILESYSTEM,
			     MS_MGC_VAL | MS_NOSUID | MS_NODEV | MS_NOEXEC |
			     MS_SYNCHRONOUS | (readwrite ? 0 : MS_RDONLY),
			     NULL) == 0)
				goto mounted;
		}

		saved_errno = errno;
		log_error("mount of system LV (%s, %s, %s) failed: %m\n",
			  system_lv_name, mount_point, SYSTEM_LV_FILESYSTEM);
		sync_unlock(lock_name, lockid);
		errno = saved_errno;
		return -1;
	}

      mounted:
/* Set the internal flags */
	mounted = 1;
	mounted_rw = readwrite;

	return 0;
}

/* Erase *all* files in the root directory of the system LV.
   This *MUST* be called with an appropriate lock held!
   The LV is left mounted RW because it is assumed that the
   caller wants to write something here after clearing some space */
int system_lv_eraseall(void)
{
	DIR *dir;
	struct dirent *ent;
	char fname[PATH_MAX];

	/* Must be mounted R/W */
	system_lv_mount(1);

	dir = opendir(mount_point);
	if (!dir)
		return -1;

	while ((ent = readdir(dir))) {
		struct stat st;
		snprintf(fname, sizeof(fname), "%s/%s", mount_point,
			 ent->d_name);

		if (stat(fname, &st)) {
			if (S_ISREG(st.st_mode))
				unlink(fname);
		}
	}
	closedir(dir);
	return 0;
}

/* This is a "high-level" routine - it mounts the system LV, writes
   the data into a file named after this node and then umounts the LV
   again */
int system_lv_write_data(char *data, ssize_t len)
{
	struct utsname nodeinfo;
	char fname[PATH_MAX];
	int outfile;
	ssize_t thiswrite;
	ssize_t written;

	if (system_lv_mount(1))
		return -1;

	/* Build the file name we are goingto use. */
	uname(&nodeinfo);
	snprintf(fname, sizeof(fname), "%s/%s", mount_point, nodeinfo.nodename);

	/* Open the file for output */
	outfile = open(fname, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (outfile < 0) {
		int saved_errno = errno;
		system_lv_umount();
		errno = saved_errno;
		return -1;
	}

	written = 0;
	do {
		thiswrite = write(outfile, data + written, len - written);
		if (thiswrite > 0)
			written += thiswrite;

	} while (written < len && thiswrite > 0);

	close(outfile);

	system_lv_umount();
	return (thiswrite < 0) ? -1 : 0;
}

/* This is a "high-level" routine - it mounts the system LV, reads
   the data from a named file and then umounts the LV
   again */
int system_lv_read_data(char *fname_base, char *data, ssize_t *len)
{
	char fname[PATH_MAX];
	int outfile;
	struct stat st;
	ssize_t filesize;
	ssize_t thisread;
	ssize_t readbytes;

	if (system_lv_mount(0))
		return -1;

	/* Build the file name we are going to use. */
	snprintf(fname, sizeof(fname), "%s/%s", mount_point, fname_base);

	/* Get the file size and stuff. Actually we only need the file size but
	   this will also check that the file exists */
	if (stat(fname, &st) < 0) {
		int saved_errno = errno;

		log_error("stat of file %s on system LV failed: %m\n", fname);
		system_lv_umount();
		errno = saved_errno;
		return -1;
	}
	filesize = st.st_size;

	outfile = open(fname, O_RDONLY);
	if (outfile < 0) {
		int saved_errno = errno;

		log_error("open of file %s on system LV failed: %m\n", fname);
		system_lv_umount();
		errno = saved_errno;
		return -1;
	}

	readbytes = 0;
	do {
		thisread =
		    read(outfile, data + readbytes, filesize - readbytes);
		if (thisread > 0)
			readbytes += thisread;

	} while (readbytes < filesize && thisread > 0);

	close(outfile);

	system_lv_umount();

	*len = readbytes;
	return (thisread < 0) ? -1 : 0;
}
