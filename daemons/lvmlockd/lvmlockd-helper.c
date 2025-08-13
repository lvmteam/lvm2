/*
 * Copyright 2025 Red Hat, Inc.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v2 or (at your option) any later version.
 */

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <poll.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <grp.h>
#include <syslog.h>

#include "lvmlockd-internal.h"

struct list_head commands; /* helper_msg_list entries */

static int _log_stderr;

#define log_helper(fmt, args...) \
do { \
	if (_log_stderr) \
		fprintf(stderr, fmt "\n", ##args); \
} while (0)

static void _save_command(struct helper_msg *msg)
{
	struct helper_msg_list *ml;

	ml = malloc(sizeof(struct helper_msg_list));
	if (!ml)
		return;

	memcpy(&ml->msg, msg, sizeof(struct helper_msg));
	list_add_tail(&ml->list, &commands);
}

static struct helper_msg_list *_get_command(int pid)
{
	struct helper_msg_list *ml;

	list_for_each_entry(ml, &commands, list) {
		if (ml->msg.pid == pid)
			return ml;
	}
	return NULL;
}

static int read_msg(int fd, struct helper_msg *msg)
{
	int rv;
 retry:
	rv = read(fd, msg, sizeof(struct helper_msg));
	if (rv == -1 && errno == EINTR)
		goto retry;

	if (rv != sizeof(struct helper_msg))
		return -1;
	return 0;
}

static void exec_command(char *cmd_str)
{
	char arg[ONE_ARG_LEN];
	char *av[MAX_AV_COUNT + 1]; /* +1 for NULL */
	int av_count = 0;
	int i, arg_len, cmd_len;

	for (i = 0; i < MAX_AV_COUNT + 1; i++)
		av[i] = NULL;

	if (!cmd_str[0])
		return;

	/* this should already be done, but make sure */
	cmd_str[RUN_COMMAND_LEN - 1] = '\0';

	memset(&arg, 0, sizeof(arg));
	arg_len = 0;
	cmd_len = strlen(cmd_str);

	for (i = 0; i < cmd_len; i++) {
		if (!cmd_str[i])
			break;

		if (av_count == MAX_AV_COUNT)
			break;

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
			if (arg_len)
				av[av_count++] = strdup(arg);

			memset(arg, 0, sizeof(arg));
			arg_len = 0;
		} else {
			break;
		}
	}

	if ((av_count < MAX_AV_COUNT) && arg_len) {
		av[av_count++] = strdup(arg);
	}

	execvp(av[0], av);
}

static int send_result(struct helper_msg *msg, int fd)
{
	int rv;

	rv = write(fd, msg, sizeof(struct helper_msg));

	if (rv == sizeof(struct helper_msg))
		return 0;
	return -1;
}

#define IDLE_TIMEOUT_MS (30 * 1000)
#define ACTIVE_TIMEOUT_MS 500

__attribute__((noreturn)) void helper_main(int in_fd, int out_fd, int log_stderr)
{
	struct pollfd pollfd;
	struct helper_msg msg;
	struct helper_msg_list *ml;
	siginfo_t info;
	unsigned int fork_count = 0;
	unsigned int done_count = 0;
	int timeout = IDLE_TIMEOUT_MS;
	int rv, pid;

	INIT_LIST_HEAD(&commands);

	_log_stderr = log_stderr;

	rv = setgroups(0, NULL);
	if (rv < 0)
		log_helper("error clearing helper groups errno %i", errno);

	memset(&pollfd, 0, sizeof(pollfd));
	pollfd.fd = in_fd;
	pollfd.events = POLLIN;

	openlog("lvmlockd-helper", LOG_CONS | LOG_PID, LOG_LOCAL4);

	while (1) {
		rv = poll(&pollfd, 1, timeout);
		if (rv == -1 && errno == EINTR)
			continue;

		if (rv < 0)
			exit(0);

		if (pollfd.revents & POLLIN) {
			memset(&msg, 0, sizeof(msg));

			rv = read_msg(in_fd, &msg);
			if (rv)
				continue;

			if (msg.type == HELPER_COMMAND) {
				pid = fork();
				if (!pid) {
					exec_command(msg.command);
					exit(1);
				}

				msg.pid = pid;

				_save_command(&msg);

				fork_count++;
			}
		}

		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			exit(0);

		/* collect child exits until no more children exist (ECHILD)
		   or none are ready (WNOHANG) */

		while (1) {
			memset(&info, 0, sizeof(info));

			rv = waitid(P_ALL, 0, &info, WEXITED | WNOHANG);

			if ((rv < 0) && (errno == ECHILD)) {
				/*
				log_helper("helper no children exist fork_count %d done_count %d", fork_count, done_count);
				*/
				timeout = IDLE_TIMEOUT_MS;
			}

			else if (!rv && !info.si_pid) {
				log_helper("helper no children ready fork_count %d done_count %d", fork_count, done_count);
				timeout = ACTIVE_TIMEOUT_MS;
			}

			else if (!rv && info.si_pid) {
				done_count++;

				if (!(ml = _get_command(info.si_pid))) {
					log_helper("command for pid %d result %d not found",
						  info.si_pid, info.si_status);
					continue;
				}

				log_helper("command for pid %d result %d done", info.si_pid, info.si_status);

				ml->msg.type = HELPER_COMMAND_RESULT;
				ml->msg.result = info.si_status;

				send_result(&ml->msg, out_fd);

				list_del(&ml->list);
				free(ml);
				continue;
			}

			else {
				log_helper("helper waitid rv %d errno %d fork_count %d done_count %d",
					  rv, errno, fork_count, done_count);
			}

			break;
		}
	}
}
