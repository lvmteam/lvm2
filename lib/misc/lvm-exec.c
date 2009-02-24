/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvm-exec.h"

#include <unistd.h>
#include <sys/wait.h>


/*
 * Create verbose string with list of parameters
 */
static char *verbose_args(const char *const argv[])
{
	char *buf = 0;
	int pos = 0;
	size_t sz = 0;
	size_t len;
	int i;

	for (i = 0; argv[i] != NULL; i++) {
		len = strlen(argv[i]);
		if (pos + len >= sz) {
			sz = 64 + (sz + len) * 2;
			if (!(buf = realloc(buf, sz)))
				break;
		}
		if (pos)
			buf[pos++] = ' ';
		memcpy(buf + pos, argv[i], len + 1); /* copy with '\0' */
		pos += len;
	}

	return buf;
}

/*
 * Execute and wait for external command
 */
int exec_cmd(const char *const argv[])
{
	pid_t pid;
	int status;
	char *buf = 0;

	log_verbose("Executing: %s", buf = verbose_args(argv));
	free(buf);

	if ((pid = fork()) == -1) {
		log_error("fork failed: %s", strerror(errno));
		return 0;
	}

	if (!pid) {
		/* Child */
		/* FIXME Use execve directly */
		execvp(argv[0], (char **const) argv); /* cast to match execvp prototype */
		log_sys_error("execvp", argv[0]);
		exit(errno);
	}

	/* Parent */
	if (wait4(pid, &status, 0, NULL) != pid) {
		log_error("wait4 child process %u failed: %s", pid,
			  strerror(errno));
		return 0;
	}

	if (!WIFEXITED(status)) {
		log_error("Child %u exited abnormally", pid);
		return 0;
	}

	if (WEXITSTATUS(status)) {
		log_error("%s failed: %u", argv[0], WEXITSTATUS(status));
		return 0;
	}

	return 1;
}
