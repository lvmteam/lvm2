/*
 * Copyright (C) 2011-2012 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "daemon-io.h"
#include "config-util.h"
#include "daemon-server.h"
#include "daemon-log.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

#include <syslog.h> /* FIXME. For the global closelog(). */

#if 0
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
#endif

static volatile sig_atomic_t _shutdown_requested = 0;
static int _systemd_activation = 0;

static void _exit_handler(int sig __attribute__((unused)))
{
	_shutdown_requested = 1;
}

#define EXIT_ALREADYRUNNING 13

#ifdef linux

#include <stddef.h>

/*
 * Kernel version 2.6.36 and higher has
 * new OOM killer adjustment interface.
 */
#  define OOM_ADJ_FILE_OLD "/proc/self/oom_adj"
#  define OOM_ADJ_FILE "/proc/self/oom_score_adj"

/* From linux/oom.h */
/* Old interface */
#  define OOM_DISABLE (-17)
#  define OOM_ADJUST_MIN (-16)
/* New interface */
#  define OOM_SCORE_ADJ_MIN (-1000)

/* Systemd on-demand activation support */
#  define SD_ACTIVATION_ENV_VAR_NAME "SD_ACTIVATION"
#  define SD_LISTEN_PID_ENV_VAR_NAME "LISTEN_PID"
#  define SD_LISTEN_FDS_ENV_VAR_NAME "LISTEN_FDS"
#  define SD_LISTEN_FDS_START 3
#  define SD_FD_SOCKET_SERVER SD_LISTEN_FDS_START

#  include <stdio.h>

static int _set_oom_adj(const char *oom_adj_path, int val)
{
	FILE *fp;

	if (!(fp = fopen(oom_adj_path, "w"))) {
		perror("oom_adj: fopen failed");
		return 0;
	}

	fprintf(fp, "%i", val);

	if (dm_fclose(fp))
		perror("oom_adj: fclose failed");

	return 1;
}

/*
 * Protection against OOM killer if kernel supports it
 */
static int _protect_against_oom_killer(void)
{
	struct stat st;

	if (stat(OOM_ADJ_FILE, &st) == -1) {
		if (errno != ENOENT)
			perror(OOM_ADJ_FILE ": stat failed");

		/* Try old oom_adj interface as a fallback */
		if (stat(OOM_ADJ_FILE_OLD, &st) == -1) {
			if (errno == ENOENT)
				perror(OOM_ADJ_FILE_OLD " not found");
			else
				perror(OOM_ADJ_FILE_OLD ": stat failed");
			return 1;
		}

		return _set_oom_adj(OOM_ADJ_FILE_OLD, OOM_DISABLE) ||
		       _set_oom_adj(OOM_ADJ_FILE_OLD, OOM_ADJUST_MIN);
	}

	return _set_oom_adj(OOM_ADJ_FILE, OOM_SCORE_ADJ_MIN);
}

union sockaddr_union {
	struct sockaddr sa;
	struct sockaddr_un un;
};

static int _handle_preloaded_socket(int fd, const char *path)
{
	struct stat st_fd;
	union sockaddr_union sockaddr = { .sa.sa_family = 0 };
	int type = 0;
	socklen_t len = sizeof(type);
	size_t path_len = strlen(path);

	if (fd < 0)
		return 0;

	if (fstat(fd, &st_fd) < 0 || !S_ISSOCK(st_fd.st_mode))
		return 0;

	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) < 0 ||
	    len != sizeof(type) || type != SOCK_STREAM)
		return 0;

	len = sizeof(sockaddr);
	if (getsockname(fd, &sockaddr.sa, &len) < 0 ||
	    len < sizeof(sa_family_t) ||
	    sockaddr.sa.sa_family != PF_UNIX)
		return 0;

	if (!(len >= offsetof(struct sockaddr_un, sun_path) + path_len + 1 &&
	      memcmp(path, sockaddr.un.sun_path, path_len) == 0))
		return 0;

	return 1;
}

static int _systemd_handover(struct daemon_state *ds)
{
	const char *e;
	char *p;
	unsigned long env_pid, env_listen_fds;
	int r = 0;

	/* SD_ACTIVATION must be set! */
	if (!(e = getenv(SD_ACTIVATION_ENV_VAR_NAME)) || strcmp(e, "1"))
		goto out;

	/* LISTEN_PID must be equal to our PID! */
	if (!(e = getenv(SD_LISTEN_PID_ENV_VAR_NAME)))
		goto out;

	errno = 0;
	env_pid = strtoul(e, &p, 10);
	if (errno || !p || *p || env_pid <= 0 ||
	    getpid() != (pid_t) env_pid)
		goto out;

	/* LISTEN_FDS must be 1 and the fd must be a socket! */
	if (!(e = getenv(SD_LISTEN_FDS_ENV_VAR_NAME)))
		goto out;

	errno = 0;
	env_listen_fds = strtoul(e, &p, 10);
	if (errno || !p || *p || env_listen_fds != 1)
		goto out;

	/* Check and handle the socket passed in */
	if ((r = _handle_preloaded_socket(SD_FD_SOCKET_SERVER, ds->socket_path)))
		ds->socket_fd = SD_FD_SOCKET_SERVER;

out:
	unsetenv(SD_ACTIVATION_ENV_VAR_NAME);
	unsetenv(SD_LISTEN_PID_ENV_VAR_NAME);
	unsetenv(SD_LISTEN_FDS_ENV_VAR_NAME);
	return r;
}

#endif

static int _open_socket(daemon_state s)
{
	int fd = -1;
	struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };
	mode_t old_mask;

	(void) dm_prepare_selinux_context(s.socket_path, S_IFSOCK);
	old_mask = umask(0077);

	/* Open local socket */
	fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("Can't create local socket.");
		goto error;
	}

	/* Set Close-on-exec & non-blocking */
	if (fcntl(fd, F_SETFD, 1))
		fprintf(stderr, "setting CLOEXEC on socket fd %d failed: %s\n", fd, strerror(errno));
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK))
		fprintf(stderr, "setting O_NONBLOCK on socket fd %d failed: %s\n", fd, strerror(errno));

	fprintf(stderr, "[D] creating %s\n", s.socket_path);
	if (!dm_strncpy(sockaddr.sun_path, s.socket_path, sizeof(sockaddr.sun_path))) {
		fprintf(stderr, "%s: daemon socket path too long.\n", s.socket_path);
		goto error;
	}

	if (bind(fd, (struct sockaddr *) &sockaddr, sizeof(sockaddr))) {
		perror("can't bind local socket.");
		goto error;
	}
	if (listen(fd, 1) != 0) {
		perror("listen local");
		goto error;
	}

out:
	umask(old_mask);
	(void) dm_prepare_selinux_context(NULL, 0);
	return fd;

error:
	if (fd >= 0) {
		if (close(fd))
			perror("close failed");
		if (unlink(s.socket_path))
			perror("unlink failed");
		fd = -1;
	}
	goto out;
}

static void remove_lockfile(const char *file)
{
	if (unlink(file))
		perror("unlink failed");
}

static void _daemonise(daemon_state s)
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
		while (!waitpid(pid, &child_status, WNOHANG) && !_shutdown_requested) {
			tval.tv_sec = 0;
			tval.tv_usec = 250000;	/* .25 sec */
			select(0, NULL, NULL, NULL, &tval);
		}

		if (_shutdown_requested) /* Child has signaled it is ok - we can exit now */
			exit(0);

		switch (WEXITSTATUS(child_status)) {
		case EXIT_ALREADYRUNNING:
			fprintf(stderr, "Failed to acquire lock on %s. Already running?\n", s.pidfile);
			break;
		default:
			/* Problem with child.  Determine what it is by exit code */
			fprintf(stderr, "Child exited with code %d\n", WEXITSTATUS(child_status));
		}
		exit(WEXITSTATUS(child_status));
	}

	if (chdir("/"))
		exit(1);

	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
		fd = 256; /* just have to guess */
	else
		fd = rlim.rlim_cur;

	for (--fd; fd >= 0; fd--) {
#ifdef linux
		/* Do not close fds preloaded by systemd! */
		if (_systemd_activation && fd == SD_FD_SOCKET_SERVER)
			continue;
#endif
		(void) close(fd);
	}

	if ((open("/dev/null", O_RDONLY) < 0) ||
	    (open("/dev/null", O_WRONLY) < 0) ||
	    (open("/dev/null", O_WRONLY) < 0))
		exit(1);

	setsid();
}

response daemon_reply_simple(const char *id, ...)
{
	va_list ap;
	response res = { .cft = NULL };

	va_start(ap, id);

	buffer_init(&res.buffer);
	if (!buffer_append_f(&res.buffer, "response = %s", id, NULL)) {
		res.error = ENOMEM;
		goto end;
	}
	if (!buffer_append_vf(&res.buffer, ap)) {
		res.error = ENOMEM;
		goto end;
	}

end:
	va_end(ap);
	return res;
}

struct thread_baton {
	daemon_state s;
	client_handle client;
};

static response builtin_handler(daemon_state s, client_handle h, request r)
{
	const char *rq = daemon_request_str(r, "request", "NONE");
	response res = { .error = EPROTO };

	if (!strcmp(rq, "hello")) {
		return daemon_reply_simple("OK", "protocol = %s", s.protocol ?: "default",
					   "version = %" PRId64, (int64_t) s.protocol_version, NULL);
	}

	buffer_init(&res.buffer);
	return res;
}

static void *client_thread(void *baton)
{
	struct thread_baton *b = baton;
	request req;
	response res;

	buffer_init(&req.buffer);

	while (1) {
		if (!buffer_read(b->client.socket_fd, &req.buffer))
			goto fail;

		req.cft = dm_config_from_string(req.buffer.mem);

		if (!req.cft)
			fprintf(stderr, "error parsing request:\n %s\n", req.buffer.mem);
		else
			daemon_log_cft(b->s.log, DAEMON_LOG_WIRE, "<- ", req.cft->root);

		res = builtin_handler(b->s, b->client, req);

		if (res.error == EPROTO) /* Not a builtin, delegate to the custom handler. */
			res = b->s.handler(b->s, b->client, req);

		if (!res.buffer.mem) {
			if (!dm_config_write_node(res.cft->root, buffer_line, &res.buffer))
				goto fail;
			if (!buffer_append(&res.buffer, "\n\n"))
				goto fail;
			dm_config_destroy(res.cft);
		}

		if (req.cft)
			dm_config_destroy(req.cft);
		buffer_destroy(&req.buffer);

		daemon_log_multi(b->s.log, DAEMON_LOG_WIRE, "-> ", res.buffer.mem);
		buffer_write(b->client.socket_fd, &res.buffer);

		buffer_destroy(&res.buffer);
	}
fail:
	/* TODO what should we really do here? */
	if (close(b->client.socket_fd))
		perror("close");
	buffer_destroy(&req.buffer);
	dm_free(baton);
	return NULL;
}

static int handle_connect(daemon_state s)
{
	struct thread_baton *baton;
	struct sockaddr_un sockaddr;
	client_handle client = { .thread_id = 0 };
	socklen_t sl = sizeof(sockaddr);

	client.socket_fd = accept(s.socket_fd, (struct sockaddr *) &sockaddr, &sl);
	if (client.socket_fd < 0)
		return 0;

	if (!(baton = dm_malloc(sizeof(struct thread_baton)))) {
		if (close(client.socket_fd))
			perror("close");
		ERROR(&s, "Failed to allocate thread baton");
		return 0;
	}

	baton->s = s;
	baton->client = client;

	if (pthread_create(&baton->client.thread_id, NULL, client_thread, baton))
		return 0;

	pthread_detach(baton->client.thread_id);

	return 1;
}

void daemon_start(daemon_state s)
{
	int failed = 0;
	log_state _log = { { 0 } };

	/*
	 * Switch to C locale to avoid reading large locale-archive file used by
	 * some glibc (on some distributions it takes over 100MB). Some daemons
	 * need to use mlockall().
	 */
	if (setenv("LC_ALL", "C", 1))
		perror("Cannot set LC_ALL to C");

#ifdef linux
	_systemd_activation = _systemd_handover(&s);
#endif

	if (!s.foreground)
		_daemonise(s);

	s.log = &_log;
	s.log->name = s.name;

	/* Log important things to syslog by default. */
	daemon_log_enable(s.log, DAEMON_LOG_OUTLET_SYSLOG, DAEMON_LOG_FATAL, 1);
	daemon_log_enable(s.log, DAEMON_LOG_OUTLET_SYSLOG, DAEMON_LOG_ERROR, 1);

	if (s.pidfile) {
		(void) dm_prepare_selinux_context(s.pidfile, S_IFREG);

		/*
		 * NB. Take care to not keep stale locks around. Best not exit(...)
		 * after this point.
		 */
		if (dm_create_lockfile(s.pidfile) == 0)
			exit(EXIT_ALREADYRUNNING);

		(void) dm_prepare_selinux_context(NULL, 0);
	}

	/* Set normal exit signals to request shutdown instead of dying. */
	signal(SIGINT, &_exit_handler);
	signal(SIGHUP, &_exit_handler);
	signal(SIGQUIT, &_exit_handler);
	signal(SIGTERM, &_exit_handler);
	signal(SIGALRM, &_exit_handler);
	signal(SIGPIPE, SIG_IGN);

#ifdef linux
	/* Systemd has adjusted oom killer for us already */
	if (s.avoid_oom && !_systemd_activation && !_protect_against_oom_killer())
		ERROR(&s, "Failed to protect against OOM killer");
#endif

	if (!_systemd_activation && s.socket_path) {
		s.socket_fd = _open_socket(s);
		if (s.socket_fd < 0)
			failed = 1;
	}

	/* Signal parent, letting them know we are ready to go. */
	if (!s.foreground)
		kill(getppid(), SIGTERM);

	if (s.daemon_init)
		if (!s.daemon_init(&s))
			failed = 1;

	while (!_shutdown_requested && !failed) {
		fd_set in;
		FD_ZERO(&in);
		FD_SET(s.socket_fd, &in);
		if (select(FD_SETSIZE, &in, NULL, NULL, NULL) < 0 && errno != EINTR)
			perror("select error");
		if (FD_ISSET(s.socket_fd, &in))
			if (!_shutdown_requested && !handle_connect(s))
				ERROR(&s, "Failed to handle a client connection.");
	}

	/* If activated by systemd, do not unlink the socket - systemd takes care of that! */
	if (!_systemd_activation && s.socket_fd >= 0)
		if (unlink(s.socket_path))
			perror("unlink error");

	if (s.daemon_fini)
		if (!s.daemon_fini(&s))
			failed = 1;

	INFO(&s, "%s shutting down", s.name);

	closelog(); /* FIXME */
	if (s.pidfile)
		remove_lockfile(s.pidfile);
	if (failed)
		exit(1);
}
