/*
 * Copyright (C) 2014-2015 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#include "tools/tool.h"

#include "daemons/lvmlockd/lvmlockd-client.h"

#include <stddef.h>
#include <getopt.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <syslog.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <sys/wait.h>

static int quit = 0;
static int info = 0;
static int dump = 0;
static int wait_opt = 1;
static int force_opt = 0;
static int kill_vg = 0;
static int drop_vg = 0;
static int gl_enable = 0;
static int gl_disable = 0;
static int use_stderr = 0;
static int stop_lockspaces = 0;
static char *arg_vg_name = NULL;

#define DUMP_SOCKET_NAME "lvmlockd-dump.sock"
#define DUMP_BUF_SIZE (4 * 1024 * 1024)
static char dump_buf[DUMP_BUF_SIZE+1];
static int dump_len;
static struct sockaddr_un dump_addr;
static socklen_t dump_addrlen;

daemon_handle _lvmlockd;

#define log_error(fmt, args...) \
do { \
	printf(fmt "\n", ##args); \
} while (0)

#define log_sys_emerg(fmt, args...) \
do { \
	if (use_stderr) \
		fprintf(stderr, fmt "\n", ##args); \
	else \
		syslog(LOG_EMERG, fmt, ##args); \
} while (0)

#define log_sys_warn(fmt, args...) \
do { \
	if (use_stderr) \
		fprintf(stderr, fmt "\n", ##args); \
	else \
		syslog(LOG_WARNING, fmt, ##args); \
} while (0)

#define MAX_LINE 512

/* copied from lvmlockd-internal.h */
#define MAX_NAME 64
#define MAX_ARGS 64

/*
 * lvmlockd dumps the client info before the lockspaces,
 * so we can look up client info when printing lockspace info.
 */

#define MAX_CLIENTS 100

struct client_info {
	uint32_t client_id;
	int pid;
	char name[MAX_NAME+1];
};

static struct client_info clients[MAX_CLIENTS];
static int num_clients;

static void save_client_info(char *line)
{
	uint32_t pid = 0;
	int fd = 0;
	int pi = 0;
	uint32_t client_id = 0;
	char name[MAX_NAME+1] = { 0 };

	(void) sscanf(line, "info=client pid=%u fd=%d pi=%d id=%u name=%s",
	       &pid, &fd, &pi, &client_id, name);

	clients[num_clients].client_id = client_id;
	clients[num_clients].pid = pid;
	strcpy(clients[num_clients].name, name);
	num_clients++;
}

static void find_client_info(uint32_t client_id, uint32_t *pid, char *cl_name)
{
	int i;

	for (i = 0; i < num_clients; i++) {
		if (clients[i].client_id == client_id) {
			*pid = clients[i].pid;
			strcpy(cl_name, clients[i].name);
			return;
		}
	}
}

static int first_ls = 1;

static void format_info_ls(char *line)
{
	char ls_name[MAX_NAME+1] = { 0 };
	char vg_name[MAX_NAME+1] = { 0 };
	char vg_uuid[MAX_NAME+1] = { 0 };
	char vg_sysid[MAX_NAME+1] = { 0 };
	char lock_args[MAX_ARGS+1] = { 0 };
	char lock_type[MAX_NAME+1] = { 0 };

	(void) sscanf(line, "info=ls ls_name=%s vg_name=%s vg_uuid=%s vg_sysid=%s vg_args=%s lm_type=%s",
	       ls_name, vg_name, vg_uuid, vg_sysid, lock_args, lock_type);

	if (!first_ls)
		printf("\n");
	first_ls = 0;

	printf("VG %s lock_type=%s %s\n", vg_name, lock_type, vg_uuid);

	printf("LS %s %s\n", lock_type, ls_name);
}

static void format_info_ls_action(char *line)
{
	uint32_t client_id = 0;
	char flags[MAX_NAME+1] = { 0 };
	char version[MAX_NAME+1] = { 0 };
	char op[MAX_NAME+1] = { 0 };
	uint32_t pid = 0;
	char cl_name[MAX_NAME+1] = { 0 };

	(void) sscanf(line, "info=ls_action client_id=%u %s %s op=%s",
	       &client_id, flags, version, op);

	find_client_info(client_id, &pid, cl_name);

	printf("OP %s pid %u (%s)\n", op, pid, cl_name);
}

static void format_info_r(char *line, char *r_name_out, char *r_type_out)
{
	char r_name[MAX_NAME+1] = { 0 };
	char r_type[4] = { 0 };
	char mode[4] = { 0 };
	char sh_count[MAX_NAME+1] = { 0 };
	uint32_t ver = 0;

	(void) sscanf(line, "info=r name=%s type=%s mode=%s %s version=%u",
	       r_name, r_type, mode, sh_count, &ver);

	strcpy(r_name_out, r_name);
	strcpy(r_type_out, r_type);

	/* when mode is not un, wait and print each lk line */
	if (strcmp(mode, "un"))
		return;

	/* when mode is un, there will be no lk lines, so print now */

	if (!strcmp(r_type, "gl")) {
		printf("LK GL un ver %u\n", ver);

	} else if (!strcmp(r_type, "vg")) {
		printf("LK VG un ver %u\n", ver);

	} else if (!strcmp(r_type, "lv")) {
		printf("LK LV un %s\n", r_name);
	}
}

static void format_info_lk(char *line, char *r_name, char *r_type)
{
	char mode[4] = { 0 };
	uint32_t ver = 0;
	char flags[MAX_NAME+1] = { 0 };
	uint32_t client_id = 0;
	uint32_t pid = 0;
	char cl_name[MAX_NAME+1] = { 0 };

	if (!r_name[0] || !r_type[0]) {
		printf("format_info_lk error r_name %s r_type %s\n", r_name, r_type);
		printf("%s\n", line);
		return;
	}

	(void) sscanf(line, "info=lk mode=%s version=%u %s client_id=%u",
	       mode, &ver, flags, &client_id);

	find_client_info(client_id, &pid, cl_name);

	if (!strcmp(r_type, "gl")) {
		printf("LK GL %s ver %u pid %u (%s)\n", mode, ver, pid, cl_name);

	} else if (!strcmp(r_type, "vg")) {
		printf("LK VG %s ver %u pid %u (%s)\n", mode, ver, pid, cl_name);

	} else if (!strcmp(r_type, "lv")) {
		printf("LK LV %s %s\n", mode, r_name);
	}
}

static void format_info_r_action(char *line, char *r_name, char *r_type)
{
	uint32_t client_id = 0;
	char flags[MAX_NAME+1] = { 0 };
	char version[MAX_NAME+1] = { 0 };
	char op[MAX_NAME+1] = { 0 };
	char rt[4] = { 0 };
	char mode[4] = { 0 };
	char lm[MAX_NAME+1] = { 0 };
	char result[MAX_NAME+1] = { 0 };
	char lm_rv[MAX_NAME+1] = { 0 };
	uint32_t pid = 0;
	char cl_name[MAX_NAME+1] = { 0 };

	if (!r_name[0] || !r_type[0]) {
		printf("format_info_r_action error r_name %s r_type %s\n", r_name, r_type);
		printf("%s\n", line);
		return;
	}

	(void) sscanf(line, "info=r_action client_id=%u %s %s op=%s rt=%s mode=%s %s %s %s",
	       &client_id, flags, version, op, rt, mode, lm, result, lm_rv);

	find_client_info(client_id, &pid, cl_name);

	if (strcmp(op, "lock")) {
		printf("OP %s pid %u (%s)\n", op, pid, cl_name);
		return;
	}

	if (!strcmp(r_type, "gl")) {
		printf("LW GL %s ver %u pid %u (%s)\n", mode, 0, pid, cl_name);

	} else if (!strcmp(r_type, "vg")) {
		printf("LW VG %s ver %u pid %u (%s)\n", mode, 0, pid, cl_name);

	} else if (!strcmp(r_type, "lv")) {
		printf("LW LV %s %s\n", mode, r_name);
	}
}

static void format_info_line(char *line, char *r_name, char *r_type)
{
	if (!strncmp(line, "info=structs ", sizeof("info=structs ") - 1)) {
		/* only print this in the raw info dump */

	} else if (!strncmp(line, "info=client ", sizeof("info=client ") - 1)) {
		save_client_info(line);

	} else if (!strncmp(line, "info=ls ", sizeof("info=ls ") - 1)) {
		format_info_ls(line);

	} else if (!strncmp(line, "info=ls_action ", sizeof("info=ls_action ") - 1)) {
		format_info_ls_action(line);

	} else if (!strncmp(line, "info=r ", sizeof("info=r ") - 1)) {
		/*
		 * r_name/r_type are reset when a new resource is found.
		 * They are reused for the lock and action lines that
		 * follow a resource line.
		 */
		memset(r_name, 0, MAX_NAME+1);
		memset(r_type, 0, MAX_NAME+1);
		format_info_r(line, r_name, r_type);

	} else if (!strncmp(line, "info=lk ", sizeof("info=lk ") - 1)) {
		/* will use info from previous r */
		format_info_lk(line, r_name, r_type);

	} else if (!strncmp(line, "info=r_action ", sizeof("info=r_action ") - 1)) {
		/* will use info from previous r */
		format_info_r_action(line, r_name, r_type);
	} else {
		printf("UN %s\n", line);
	}
}

static void format_info(void)
{
	char line[MAX_LINE] = { 0 };
	char r_name[MAX_NAME+1] = { 0 };
	char r_type[MAX_NAME+1] = { 0 };
	int i, j;

	j = 0;

	for (i = 0; i < dump_len; i++) {
		line[j++] = dump_buf[i];

		if ((line[j-1] == '\n') || (line[j-1] == '\0')) {
			format_info_line(line, r_name, r_type);
			j = 0;
			memset(line, 0, sizeof(line));
		}
	}
}


static daemon_reply _lvmlockd_send(const char *req_name, ...)
{
	va_list ap;
	daemon_reply repl;
	daemon_request req;

	req = daemon_request_make(req_name);

	va_start(ap, req_name);
	daemon_request_extend_v(req, ap);
	va_end(ap);

	repl = daemon_send(_lvmlockd, req);

	daemon_request_destroy(req);

	return repl;
}

/* See the same in lib/locking/lvmlockd.c */
#define NO_LOCKD_RESULT -1000

static int _lvmlockd_result(daemon_reply reply, int *result)
{
	int reply_result;

	*result = NO_LOCKD_RESULT;

	if (reply.error) {
		log_error("lvmlockd_result reply error %d", reply.error);
		return 0;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		log_error("lvmlockd_result bad response");
		return 0;
	}

	reply_result = daemon_reply_int(reply, "op_result", NO_LOCKD_RESULT);
	if (reply_result == NO_LOCKD_RESULT) {
		log_error("lvmlockd_result no op_result");
		return 0;
	}

	*result = reply_result;

	return 1;
}

static int do_quit(void)
{
	daemon_reply reply;
	int rv = 0;

	reply = daemon_send_simple(_lvmlockd, "quit", NULL);

	if (reply.error) {
		log_error("reply error %d", reply.error);
		rv = reply.error;
	}

	daemon_reply_destroy(reply);
	return rv;
}

static int setup_dump_socket(void)
{
	int s, rv;

	s = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (s < 0)
		return s;

	memset(&dump_addr, 0, sizeof(dump_addr));
	dump_addr.sun_family = AF_LOCAL;
	strcpy(&dump_addr.sun_path[1], DUMP_SOCKET_NAME);
	dump_addrlen = sizeof(sa_family_t) + strlen(dump_addr.sun_path+1) + 1;

	rv = bind(s, (struct sockaddr *) &dump_addr, dump_addrlen);
	if (rv < 0) {
		rv = -errno;
		if (close(s))
			log_error("failed to close dump socket");
		return rv;
	}

	return s;
}

static int do_dump(const char *req_name)
{
	daemon_reply reply;
	int result;
	int fd, rv = 0;
	int count = 0;

	fd = setup_dump_socket();
	if (fd < 0) {
		log_error("socket error %d", fd);
		return fd;
	}

	reply = daemon_send_simple(_lvmlockd, req_name, NULL);

	if (reply.error) {
		log_error("reply error %d", reply.error);
		rv = reply.error;
		goto out;
	}

	result = daemon_reply_int(reply, "result", 0);
	dump_len = daemon_reply_int(reply, "dump_len", 0);

	daemon_reply_destroy(reply);

	if (result < 0) {
		rv = result;
		log_error("result %d", result);
	}

	if (!dump_len)
		goto out;

	memset(dump_buf, 0, sizeof(dump_buf));

retry:
	rv = recvfrom(fd, dump_buf + count, dump_len - count, MSG_WAITALL,
		      (struct sockaddr *)&dump_addr, &dump_addrlen);
	if (rv < 0) {
		log_error("recvfrom error %d %d", rv, errno);
		rv = -errno;
		goto out;
	}
	count += rv;

	if (count < dump_len)
		goto retry;

	dump_buf[count] = 0;
	rv = 0;
	if ((info && dump) || !strcmp(req_name, "dump"))
		printf("%s\n", dump_buf);
	else
		format_info();
out:
	if (close(fd))
		log_error("failed to close dump socket %d", fd);
	return rv;
}

static int do_able(const char *req_name)
{
	daemon_reply reply;
	int result;
	int rv;

	reply = _lvmlockd_send(req_name,
				"cmd = %s", "lvmlockctl",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", arg_vg_name,
				NULL);

	if (!_lvmlockd_result(reply, &result)) {
		log_error("lvmlockd result %d", result);
		rv = result;
	} else {
		rv = 0;
	}

	daemon_reply_destroy(reply);
	return rv;
}

static int do_stop_lockspaces(void)
{
	daemon_reply reply;
	char opts[32];
	int result;
	int rv;

	memset(opts, 0, sizeof(opts));

	if (wait_opt)
		strcat(opts, "wait ");
	if (force_opt)
		strcat(opts, "force ");

	reply = _lvmlockd_send("stop_all",
				"cmd = %s", "lvmlockctl",
				"pid = " FMTd64, (int64_t) getpid(),
				"opts = %s", opts[0] ? opts : "none",
				NULL);

	if (!_lvmlockd_result(reply, &result)) {
		log_error("lvmlockd result %d", result);
		rv = result;
	} else {
		rv = 0;
	}

	daemon_reply_destroy(reply);
	return rv;
}

static int _reopen_fd_to_null(int fd)
{
	int null_fd;
	int r = 0;

	if ((null_fd = open("/dev/null", O_RDWR)) == -1) {
		log_error("open error /dev/null %d", errno);
		return 0;
	}

	if (close(fd)) {
		log_error("close error fd %d %d", fd, errno);
		goto out;
	}

	if (dup2(null_fd, fd) == -1) {
		log_error("dup2 error %d", errno);
		goto out;
	}

	r = 1;
out:
	if (close(null_fd)) {
		log_error("close error fd %d %d", null_fd, errno);
		return 0;
	}

	return r;
}

#define MAX_AV_COUNT 32
#define ONE_ARG_LEN 1024

static void _run_command_pipe(const char *cmd_str, pid_t *pid_out, FILE **fp_out)
{
	char arg[ONE_ARG_LEN];
	char *av[MAX_AV_COUNT + 1]; /* +1 for NULL */
	char *arg_dup;
	int av_count = 0;
	int cmd_len;
	int arg_len;
	pid_t pid = 0;
	FILE *fp = NULL;
	int pipefd[2];
	int i;

	for (i = 0; i < MAX_AV_COUNT + 1; i++)
		av[i] = NULL;

	cmd_len = strlen(cmd_str);

	memset(&arg, 0, sizeof(arg));
	arg_len = 0;

	for (i = 0; i < cmd_len; i++) {
		if (!cmd_str[i])
			break;

		if (av_count == MAX_AV_COUNT)
			goto out;

		if (cmd_str[i] == '\\') {
			if (i == (cmd_len - 1))
				break;
			i++;

			if (cmd_str[i] == '\\') {
				arg[arg_len++] = cmd_str[i];
				continue;
			}
			if (isspace(cmd_str[i])) {
				arg[arg_len++] = cmd_str[i];
				continue;
			} else {
				break;
			}
		}

		if (isalnum(cmd_str[i]) || ispunct(cmd_str[i])) {
			arg[arg_len++] = cmd_str[i];
		} else if (isspace(cmd_str[i])) {
			if (arg_len) {
				if (!(arg_dup = strdup(arg)))
					goto out;
				av[av_count++] = arg_dup;
			}

			memset(arg, 0, sizeof(arg));
			arg_len = 0;
		} else {
			break;
		}
	}

	if (arg_len) {
		if (av_count >= MAX_AV_COUNT)
			goto out;
		if (!(arg_dup = strdup(arg)))
			goto out;
		av[av_count++] = arg_dup;
	}

	if (pipe(pipefd)) {
		log_error("pipe error %d", errno);
		goto out;
	}

	pid = fork();

	if (pid < 0) {
		log_error("fork error %d", errno);
		pid = 0;
		goto out;
	}

	if (pid == 0) {
		/* Child -> writer, convert pipe[0] to STDOUT */
		if (!_reopen_fd_to_null(STDIN_FILENO))
			log_error("reopen STDIN error");
		else if (close(pipefd[0 /*read*/]))
			log_error("close error pipe[0] %d", errno);
		else if (close(STDOUT_FILENO))
			log_error("close error STDOUT %d", errno);
		else if (dup2(pipefd[1 /*write*/], STDOUT_FILENO) == -1)
			log_error("dup2 error STDOUT %d", errno);
		else if (close(pipefd[1]))
			log_error("close error pipe[1] %d", errno);
		else {
			execvp(av[0], av);
			log_error("execvp error %d", errno);
		}
		_exit(errno);
	}

	/* Parent -> reader */
	if (close(pipefd[1 /*write*/]))
		log_error("close error STDOUT %d", errno);

	if (!(fp = fdopen(pipefd[0 /*read*/],  "r"))) {
		log_error("fdopen STDIN error %d", errno);
		if (close(pipefd[0]))
			log_error("close error STDIN %d", errno);
	}

 out:
	for (i = 0; i < MAX_AV_COUNT + 1; i++)
		free(av[i]);

	*pid_out = pid;
	*fp_out = fp;
}

/* Returns -1 on error, 0 on success. */

static int _close_command_pipe(pid_t pid, FILE *fp)
{
	int status, estatus;
	int ret = -1;

	if (waitpid(pid, &status, 0) != pid) {
		log_error("waitpid error pid %d %d", pid, errno);
		goto out;
	}

	if (WIFEXITED(status)) {
		/* pid exited with an exit code */
		estatus = WEXITSTATUS(status);

		/* exit status 0: child success */
		if (!estatus) {
			ret = 0;
			goto out;
		}

		/* exit status not zero: child error */
		log_error("child exit error %d", estatus);
		goto out;
	}

	if (WIFSIGNALED(status)) {
		/* pid terminated due to a signal */
		log_error("child exit from signal");
		goto out;
	}

	log_error("child exit problem");

out:
	if (fp && fclose(fp))
		log_error("fclose error STDIN %d", errno);
	return ret;
}

/* Returns -1 on error, 0 on success. */

static int _get_kill_command(char *kill_cmd)
{
	char config_cmd[PATH_MAX + 128] = { 0 };
	char config_val[1024] = { 0 };
	char line[PATH_MAX] = { 0 };
	pid_t pid = 0;
	FILE *fp = NULL;

	snprintf(config_cmd, PATH_MAX, "%s config --typeconfig full global/lvmlockctl_kill_command", LVM_PATH);

	_run_command_pipe(config_cmd, &pid, &fp);

	if (!pid) {
		log_error("failed to run %s", config_cmd);
		return -1;
	}

	if (!fp) {
		log_error("failed to get output %s", config_cmd);
		_close_command_pipe(pid, fp);
		return -1;
	}

	if (!fgets(line, sizeof(line), fp)) {
		log_error("no output from %s", config_cmd);
		goto bad;
	}

	if (sscanf(line, "lvmlockctl_kill_command=\"%256[^\n\"]\"", config_val) != 1) {
		log_error("unrecognized config value from %s", config_cmd);
		goto bad;
	}

	if (!config_val[0] || (config_val[0] == ' ')) {
		log_error("invalid config value from %s", config_cmd);
		goto bad;
	}

	if (config_val[0] != '/') {
		log_error("lvmlockctl_kill_command must be full path");
		goto bad;
	}

	printf("Found lvmlockctl_kill_command: %s\n", config_val);

	snprintf(kill_cmd, PATH_MAX, "%s %s", config_val, arg_vg_name);
	kill_cmd[PATH_MAX-1] = '\0';

	_close_command_pipe(pid, fp);
	return 0;
bad:
	_close_command_pipe(pid, fp);
	return -1;
}

/* Returns -1 on error, 0 on success. */

static int _run_kill_command(char *kill_cmd)
{
	pid_t pid = 0;
	FILE *fp = NULL;
	int rv;

	_run_command_pipe(kill_cmd, &pid, &fp);
	rv = _close_command_pipe(pid, fp);

	if (!pid)
		return -1;

	if (rv < 0)
		return -1;

	return 0;
}

static int do_drop(void)
{
	daemon_reply reply;
	int result;
	int rv;

	log_sys_warn("Dropping locks for VG %s.", arg_vg_name);

	/*
	 * Check for misuse by looking for any active LVs in the VG
	 * and refusing this operation if found?  One possible way
	 * to kill LVs (e.g. if fs cannot be unmounted) is to suspend
	 * them, or replace them with the error target.  In that
	 * case the LV will still appear to be active, but it is
	 * safe to release the lock.
	 */

	reply = _lvmlockd_send("drop_vg",
				"cmd = %s", "lvmlockctl",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", arg_vg_name,
				NULL);

	if (!_lvmlockd_result(reply, &result)) {
		log_error("lvmlockd result %d", result);
		rv = result;
	} else {
		rv = 0;
	}

	daemon_reply_destroy(reply);
	return rv;
}

static int do_kill(void)
{
	char kill_cmd[PATH_MAX] = { 0 };
	daemon_reply reply;
	int no_kill_command = 0;
	int result;
	int rv;

	log_sys_emerg("lvmlockd lost access to locks in VG %s.", arg_vg_name);

	rv = _get_kill_command(kill_cmd);
	if (rv < 0) {
		log_sys_emerg("Immediately deactivate LVs in VG %s.", arg_vg_name);
		log_sys_emerg("Once VG is unused, run lvmlockctl --drop %s.", arg_vg_name);
		no_kill_command = 1;
	}

	/*
	 * It may not be strictly necessary to notify lvmlockd of the kill, but
	 * lvmlockd can use this information to avoid attempting any new lock
	 * requests in the VG (which would fail anyway), and can return an
	 * error indicating that the VG has been killed.
	 */
	_lvmlockd = lvmlockd_open(NULL);
	if (_lvmlockd.socket_fd < 0 || _lvmlockd.error) {
		log_error("Cannot connect to lvmlockd for kill_vg.");
		goto run;
	}
	reply = _lvmlockd_send("kill_vg",
				"cmd = %s", "lvmlockctl",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", arg_vg_name,
				NULL);
	if (!_lvmlockd_result(reply, &result))
		log_error("lvmlockd result %d kill_vg", result);
	daemon_reply_destroy(reply);
	lvmlockd_close(_lvmlockd);

 run:
	if (no_kill_command)
		return 0;

	rv = _run_kill_command(kill_cmd);
	if (rv < 0) {
		log_sys_emerg("Failed to run VG %s kill command %s", arg_vg_name, kill_cmd);
		log_sys_emerg("Immediately deactivate LVs in VG %s.", arg_vg_name);
		log_sys_emerg("Once VG is unused, run lvmlockctl --drop %s.", arg_vg_name);
		return -1;
	}

	log_sys_warn("Successful VG %s kill command %s", arg_vg_name, kill_cmd);

	/*
	 * If kill command was successfully, call do_drop().  (The drop step
	 * may not always be necessary if the lvm commands run while shutting
	 * things down release all the leases.)
	 */
	rv = 0;
	_lvmlockd = lvmlockd_open(NULL);
	if (_lvmlockd.socket_fd < 0 || _lvmlockd.error) {
		log_sys_emerg("Failed to connect to lvmlockd to drop locks in VG %s.", arg_vg_name);
		return -1;
	}
	reply = _lvmlockd_send("drop_vg",
				"cmd = %s", "lvmlockctl",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", arg_vg_name,
				NULL);
	if (!_lvmlockd_result(reply, &result)) {
		log_sys_emerg("Failed to drop locks in VG %s", arg_vg_name);
		rv = result;
	}
	daemon_reply_destroy(reply);
	lvmlockd_close(_lvmlockd);

	return rv;
}

static void print_usage(void)
{
	printf("lvmlockctl options\n");
	printf("Options:\n");
	printf("--help | -h\n");
	printf("      Show this help information.\n");
	printf("--quit | -q\n");
	printf("      Tell lvmlockd to quit.\n");
	printf("--info | -i\n");
	printf("      Print lock state information from lvmlockd.\n");
	printf("--dump | -d\n");
	printf("      Print log buffer from lvmlockd.\n");
	printf("--wait | -w 0|1\n");
	printf("      Wait option for other commands.\n");
	printf("--force | -f 0|1>\n");
	printf("      Force option for other commands.\n");
	printf("--kill | -k <vgname>\n");
	printf("      Kill access to the VG locks are lost (see lvmlockctl_kill_command).\n");
	printf("--drop | -r <vgname>\n");
	printf("      Clear locks for the VG when it is unused after kill (-k).\n");
	printf("--gl-enable | -E <vgname>\n");
	printf("      Tell lvmlockd to enable the global lock in a sanlock VG.\n");
	printf("--gl-disable | -D <vgname>\n");
	printf("      Tell lvmlockd to disable the global lock in a sanlock VG.\n");
	printf("--stop-lockspaces | -S\n");
	printf("      Stop all lockspaces.\n");
	printf("--stderr | -e\n");
	printf("      Send kill and drop messages to stderr instead of syslog\n");
}

static int read_options(int argc, char *argv[])
{
	int option_index = 0;
	int c;

	static const struct option _long_options[] = {
		{"help",            no_argument,       0,  'h' },
		{"quit",            no_argument,       0,  'q' },
		{"info",            no_argument,       0,  'i' },
		{"dump",            no_argument,       0,  'd' },
		{"wait",            required_argument, 0,  'w' },
		{"force",           required_argument, 0,  'f' },
		{"kill",            required_argument, 0,  'k' },
		{"drop",            required_argument, 0,  'r' },
		{"gl-enable",       required_argument, 0,  'E' },
		{"gl-disable",      required_argument, 0,  'D' },
		{"stop-lockspaces", no_argument,       0,  'S' },
		{"stderr",          no_argument,       0,  'e' },
		{0, 0, 0, 0 }
	};

	if (argc == 1) {
		print_usage();
		exit(0);
	}

	while (1) {
		c = getopt_long(argc, argv, "hqidE:D:w:k:r:Se", _long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			/* --help */
			print_usage();
			exit(0);
		case 'q':
			/* --quit */
			quit = 1;
			break;
		case 'i':
			/* --info */
			info = 1;
			break;
		case 'd':
			/* --dump */
			dump = 1;
			break;
		case 'w':
			wait_opt = atoi(optarg);
			break;
		case 'k':
			kill_vg = 1;
			free(arg_vg_name);
			arg_vg_name = strdup(optarg);
			break;
		case 'r':
			drop_vg = 1;
			free(arg_vg_name);
			arg_vg_name = strdup(optarg);
			break;
		case 'E':
			gl_enable = 1;
			free(arg_vg_name);
			arg_vg_name = strdup(optarg);
			break;
		case 'D':
			gl_disable = 1;
			free(arg_vg_name);
			arg_vg_name = strdup(optarg);
			break;
		case 'S':
			stop_lockspaces = 1;
			break;
		case 'e':
			use_stderr = 1;
			break;
		default:
			print_usage();
			exit(1);
		}
	}


	return 0;
}

int main(int argc, char **argv)
{
	int rv = 0;

	rv = read_options(argc, argv);
	if (rv < 0)
		return rv;

	/* do_kill handles lvmlockd connections itself */
	if (kill_vg)
		return do_kill();


	_lvmlockd = lvmlockd_open(NULL);
	if (_lvmlockd.socket_fd < 0 || _lvmlockd.error) {
		log_error("Cannot connect to lvmlockd.");
		return -1;
	}

	if (quit) {
		rv = do_quit();
		goto out;
	}

	if (info) {
		rv = do_dump("info");
		goto out;
	}

	if (dump) {
		rv = do_dump("dump");
		goto out;
	}

	if (drop_vg) {
		rv = do_drop();
		goto out;
	}

	if (gl_enable) {
		syslog(LOG_INFO, "Enabling global lock in VG %s.", arg_vg_name);
		rv = do_able("enable_gl");
		goto out;
	}

	if (gl_disable) {
		syslog(LOG_INFO, "Disabling global lock in VG %s.", arg_vg_name);
		rv = do_able("disable_gl");
		goto out;
	}

	if (stop_lockspaces) {
		rv = do_stop_lockspaces();
		goto out;
	}

out:
	lvmlockd_close(_lvmlockd);
	return rv;
}
