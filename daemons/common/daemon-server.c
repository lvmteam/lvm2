/*
 * Copyright (C) 2011 Red Hat, Inc. All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>

#include <syslog.h>

/* Create a device monitoring thread. */
static int _pthread_create(pthread_t *t, void *(*fun)(void *), void *arg, int stacksize)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	/*
	 * We use a smaller stack since it gets preallocated in its entirety
	 */
	pthread_attr_setstacksize(&attr, stacksize);
	return pthread_create(t, &attr, fun, arg);
}

static volatile sig_atomic_t _shutdown_requested = 0;

static void _exit_handler(int sig __attribute__((unused)))
{
	_shutdown_requested = 1;
}

#ifdef linux
#  define OOM_ADJ_FILE "/proc/self/oom_adj"

/* From linux/oom.h */
#  define OOM_DISABLE (-17)
#  define OOM_ADJUST_MIN (-16)

/*
 * Protection against OOM killer if kernel supports it
 */
static int _set_oom_adj(int val)
{
	FILE *fp;

	struct stat st;

	if (stat(OOM_ADJ_FILE, &st) == -1) {
		if (errno == ENOENT)
			perror(OOM_ADJ_FILE " not found");
		else
			perror(OOM_ADJ_FILE ": stat failed");
		return 1;
	}

	if (!(fp = fopen(OOM_ADJ_FILE, "w"))) {
		perror(OOM_ADJ_FILE ": fopen failed");
		return 0;
	}

	fprintf(fp, "%i", val);
	if (dm_fclose(fp))
		perror(OOM_ADJ_FILE ": fclose failed");

	return 1;
}
#endif

static void remove_lockfile(const char *file)
{
	if (unlink(file))
		perror(file ": unlink failed");
}

static void _daemonise(void)
{
	int child_status;
	int fd;
	pid_t pid;
	struct rlimit rlim;
	struct timeval tval;
	sigset_t my_sigset;

	sigemptyset(&my_sigset);
	if (sigprocmask(SIG_SETMASK, &my_sigset, NULL) < 0) {
		fprintf(stderr, "Unable to restore signals.\n");
		exit(EXIT_FAILURE);
	}
	signal(SIGTERM, &_exit_handler);

	switch (pid = fork()) {
	case -1:
		perror("fork failed:");
		exit(EXIT_FAILURE);

	case 0:		/* Child */
		break;

	default:
		/* Wait for response from child */
		while (!waitpid(pid, &child_status, WNOHANG) && !_exit_now) {
			tval.tv_sec = 0;
			tval.tv_usec = 250000;	/* .25 sec */
			select(0, NULL, NULL, NULL, &tval);
		}

		if (_shutdown_requested) /* Child has signaled it is ok - we can exit now */
			exit(0);

		/* Problem with child.  Determine what it is by exit code */
		switch (WEXITSTATUS(child_status)) {
		case EXIT_DESC_CLOSE_FAILURE:
		case EXIT_DESC_OPEN_FAILURE:
		case EXIT_FIFO_FAILURE:
		case EXIT_CHDIR_FAILURE:
		default:
			fprintf(stderr, "Child exited with code %d\n", WEXITSTATUS(child_status));
			break;
		}

		exit(WEXITSTATUS(child_status));
	}

	if (chdir("/"))
		exit(1);

	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
		fd = 256; /* just have to guess */
	else
		fd = rlim.rlim_cur;

	for (--fd; fd >= 0; fd--)
		close(fd);

	if ((open("/dev/null", O_RDONLY) < 0) ||
	    (open("/dev/null", O_WRONLY) < 0) ||
	    (open("/dev/null", O_WRONLY) < 0))
		exit(1);

	setsid();
}

void daemon_start(daemon_state s, handle_request r)
{
	/*
	 * Switch to C locale to avoid reading large locale-archive file used by
	 * some glibc (on some distributions it takes over 100MB). Some daemons
	 * need to use mlockall().
	 */
	if (setenv("LANG", "C", 1))
		perror("Cannot set LANG to C");

	if (!s.foreground)
		_daemonise();

	/* TODO logging interface should be somewhat more elaborate */
	openlog(s.name, LOG_PID, LOG_DAEMON);

	(void) dm_prepare_selinux_context(s.pidfile, S_IFREG);

	/*
	 * NB. Past this point, exit is not allowed. You have to return to this
	 * function at all costs. More or less.
	 */
	if (dm_create_lockfile(s.pidfile) == 0)
		exit(1);

	(void) dm_prepare_selinux_context(NULL, 0);

	/* Set normal exit signals to request shutdown instead of dying. */
	signal(SIGINT, &_exit_handler);
	signal(SIGHUP, &_exit_handler);
	signal(SIGQUIT, &_exit_handler);

#ifdef linux
	if (s.avoid_oom && !_set_oom_adj(OOM_DISABLE) && !_set_oom_adj(OOM_ADJUST_MIN))
		syslog(LOG_ERR, "Failed to set oom_adj to protect against OOM killer");
#endif

	/* Signal parent, letting them know we are ready to go. */
	if (!s.foreground)
		kill(getppid(), SIGTERM);

	while (!_shutdown_requested) {
		/* TODO: do work */
	}

	syslog(LOG_NOTICE, "%s shutting down", s.name);
	closelog();
	remove_lockfile(s.pidfile);
}
