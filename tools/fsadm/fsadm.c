/*
 * Copyright (C) 2004  Red Hat GmbH. All rights reserved.
 *
 * LVM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 */

/*
 * FIXME: pass smart resizer arguments through from lvresize
 *        (eg, for xfs_growfs)
 */

/* FIXME All funcs to return 0 on success or an error from errno.h on failure */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#define MAX_ARGS 8

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <fstab.h>
#include <sys/mount.h>
#include <sys/vfs.h>

#define log_error(str, x...) fprintf(stderr, "%s(%u):  " str "\n", __FILE__, __LINE__, x)

/* Filesystem related information */
struct fsinfo {
	struct fstab *fsent;
	struct statfs statfs;
	uint64_t new_size;
	const char *cmd;
};

static void _usage(const char *cmd)
{
	log_error("Usage: %s [check <filesystem> | resize <filesystem> <size>]",
		  basename(cmd));
}

/* FIXME Make this more robust - /proc, multiple mounts, TMPDIR + security etc. */
/* FIXME Ensure filesystem is not mounted anywhere before running fsck/resize */
/* Gather filesystem information (VFS type, special file and block size) */
static int _get_fsinfo(const char *file, struct fsinfo *fsinfo)
{
	char *dir, template[] = "/tmp/fscmd_XXXXXX";
	struct stat info;
	int ret = 0;

	if (stat(file, &info)) {
		log_error("%s: stat failed: %s", file, strerror(errno));
		return errno;
	}

	/* FIXME: are we limited to /etc/fstab entries ? */
	if (!(fsinfo->fsent = info.st_rdev ? getfsspec(file) : getfsfile(file))) {
		log_error("%s: getfsspec/getfsfile failed: "
			  "Missing from /etc/fstab?", file);
		return EINVAL;
	}

	/* FIXME: any other way to retrieve fs blocksize avoiding mounting ? */
	if (!(dir = (mkdtemp(template)))) {
		log_error("%s: mkdtemp failed: %s", template, strerror(errno));
		return errno;
	}

	if (mount(fsinfo->fsent->fs_spec, dir, fsinfo->fsent->fs_vfstype,
		  MS_RDONLY, NULL)) {
		log_error("%s: mount %s failed: %s", fsinfo->fsent->fs_spec,
			  dir, strerror(errno));
		ret = errno;
		goto out;
	}

	if (statfs(dir, &fsinfo->statfs)) {
		log_error("%s: statfs failed: %s", dir, strerror(errno));
		ret = errno;
		goto out1;
	}

      out1:
	if (umount(dir))
		log_error("%s: umount failed: %s", dir, strerror(errno));

      out:
	if (rmdir(dir))
		log_error("%s: rmdir failed: %s", dir, strerror(errno));

	return ret;
}

enum {
	BLOCKS,
	KILOBYTES
};

#define	LEN	32		/* Length of temporary string */

/* Create size string in units of blocks or kilobytes
 * (size expected in kilobytes)
 */
static char *_size_str(struct fsinfo *fsinfo, int unit)
{
	uint64_t new_size = fsinfo->new_size;
	static char s[LEN];

	/* Avoid switch() as long as there's only 2 cases */
	snprintf(s, LEN, "%" PRIu64,
		 unit == BLOCKS ? new_size / (fsinfo->statfs.f_bsize >> 10) :
		 new_size);

	return s;
}

/* Allocate memory and store string updating string pointer */
static int _store(char **arg, char **str)
{
	size_t len = 0;
	char *s = *str;

	while (*s && *s != '%' && *(s++) != ' ')
		len++;

	if ((*arg = (char *) malloc(len + 1))) {
		strncpy(*arg, *str, len);
		(*arg)[len] = '\0';
		*str = s - 1;
		return 0;
	}

	return 1;
}

/* Construct a new argument vector for the real external command */
static int _new_argv(char **new_argv, struct fsinfo *fsinfo)
{
	int i = 0, b = 0, d = 0, k = 0, m = 0;
	char *s1;
	char *s = (char *) fsinfo->cmd;

	if (!s || !strlen(s))
		return 1;

	do {
		switch (*s) {
		case ' ':
			break;

		case '%':
			s++;

			switch (tolower(*s)) {
			case 'b':
				s1 = _size_str(fsinfo, BLOCKS);
				if (b++ + k || _store(&new_argv[i++], &s1))
					goto error;
				break;

			case 'k':
				s1 = _size_str(fsinfo, KILOBYTES);
				if (b + k++ || _store(&new_argv[i++], &s1))
					goto error;
				break;

			case 'd':
				s1 = fsinfo->fsent->fs_spec;
				if (d++ + m || _store(&new_argv[i++], &s1))
					goto error;
				break;

			case 'm':
				s1 = fsinfo->fsent->fs_file;
				if (d + m++ || _store(&new_argv[i++], &s1))
					goto error;
				break;

			default:
				goto error;
			}

			break;

		default:
			if (_store(&new_argv[i++], &s))
				goto error;
		}
	} while (*++s);

	new_argv[i] = NULL;

	return 0;

      error:
	new_argv[i] = NULL;
	log_error("Failed constructing arguments for %s", s);

	return EINVAL;
}

/*
 * Get filesystem command arguments derived from a command definition string
 *
 * Command definition syntax: 'cmd [-option]{0,} [(option)argument]{0,}'
 *
 * (option)argument can be: '%{bdkm}'
 *
 * Command definition is parsed into argument strings of
 * an argument vector with:
 *
 *	%b replaced by the size in filesystem blocks
 *	%k replaced by the size in kilobytes
 *	%d replaced by the name of the device special of the LV
 *	%m replaced by the mountpoint of the filesystem
 *
 */
static int _get_cmd(char *command, struct fsinfo *fsinfo)
{
	const char *vfstype = fsinfo->fsent->fs_vfstype;
	struct fscmd {
		const char *vfstype;
		const char *fsck;
		const char *fsresize;
	} fscmds[] = {
		{ "ext2", "fsck -fy %d", "ext2resize %d %b"},
		{"ext3", "fsck -fy %d", "ext2resize %d %b"},
		{"reiserfs", "", "resize_reiserfs -s%k %d"},
		{"xfs", "", "xfs_growfs -D %b %m"},	/* simple xfs grow */
		{NULL, NULL, NULL},
	}, *p = &fscmds[0];

	for (; p->vfstype; p++) {
		if (!strcmp(vfstype, p->vfstype)) {
			if (!strcmp(command, "resize"))
				fsinfo->cmd = p->fsresize;
			else if (!strcmp(command, "check"))
				fsinfo->cmd = p->fsck;
			else {
				log_error("Unrecognised command: %s", command);
				return EINVAL;
			}

			break;
		}
	}

	if (!fsinfo->cmd) {
		log_error("%s: Unrecognised filesystem type", vfstype);
		return EINVAL;
	}

	return 0;
}

/* Collapse multiple slashes */
static char *_collapse_slashes(char *path)
{
	char *s = path;

	/* Slight overhead but short ;) */
	while ((s = strchr(s, '/')) && *(s + 1))
		*(s + 1) == '/' ? memmove(s, s + 1, strlen(s)) : s++;

	return path;
}

/* Free the argument array */
static void _free_argv(char **new_argv)
{
	int i;

	for (i = 0; new_argv[i]; i++)
		free(new_argv[i]);
}

/*
 * check/resize a filesystem
 */
int main(int argc, char **argv)
{
	int ret = 0;
	struct fsinfo fsinfo;
	char *new_argv[MAX_ARGS];
	char *command, *path;

	if (argc < 3)
		goto error;

	command = argv[1];
	path = _collapse_slashes(argv[2]);

	if (!strcmp(command, "resize")) {
		if (argc != 4)
			goto error;
		/* FIXME sanity checks */
		fsinfo.new_size = strtoul(argv[3], NULL, 10);
	} else if (argc != 3)
		goto error;

	/* Retrieve filesystem information (block size...) */
	if ((ret = _get_fsinfo(path, &fsinfo))) {
		log_error("Can't get filesystem information from %s", path);
		return ret;
	}

	/* Get filesystem command info */
	if ((ret = _get_cmd(command, &fsinfo))) {
		log_error("Can't get filesystem command for %s", command);
		return ret;
	}

	if (_new_argv(new_argv, &fsinfo))
		return EINVAL;

	if (!new_argv[0])
		return 0;

	execvp(new_argv[0], new_argv);
	ret = errno;
	log_error("%s: execvp %s failed", new_argv[0], strerror(errno));
	_free_argv(new_argv);

	return ret;

      error:
	_usage(argv[0]);
	return EINVAL;
}
