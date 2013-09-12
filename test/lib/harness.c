/*
 * Copyright (C) 2010-2013 Red Hat, Inc. All rights reserved.
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

#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h> /* rusage */
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pid_t pid;
static int fds[2];

#define MAX 1024
#define MAX_LOG_SIZE (32*1024*1024) /* Default max size of test log */

struct stats {
	int nfailed;
	int nskipped;
	int npassed;
	int nknownfail;
	int nwarned;
	int ninterrupted;
	int status[MAX];
};

static struct stats s;

static char *readbuf = NULL;
static size_t readbuf_sz = 0, readbuf_used = 0;

static int die = 0;
static int verbose = 0; /* >1 with timestamps */
static int interactive = 0; /* disable all redirections */
static const char *results;
static unsigned fullbuffer = 0;

static FILE *outfile = NULL;

struct subst {
	const char *key;
	char *value;
};

static struct subst subst[2];

enum {
	UNKNOWN,
	PASSED,
	SKIPPED,
	FAILED,
	WARNED,
	KNOWNFAIL,
	INTERRUPTED,
	TIMEOUT,
};

static void handler( int sig ) {
	signal( sig, SIG_DFL );
	kill( -pid, sig );
	die = sig;
}

static int outline(FILE *out, char *buf, int start, int force) {
	char *from = buf + start;
	char *next = strchr(buf + start, '\n');

	if (!next && !force) /* not a complete line yet... */
		return start;

	if (!next)
		next = from + strlen(from);
	else
		++next;

	if (!strncmp(from, "@TESTDIR=", 9)) {
		subst[0].key = "@TESTDIR@";
		free(subst[0].value);
		subst[0].value = strndup(from + 9, next - from - 9 - 1);
	} else if (!strncmp(from, "@PREFIX=", 8)) {
		subst[1].key = "@PREFIX@";
		free(subst[1].value);
		subst[1].value = strndup(from + 8, next - from - 8 - 1);
	} else {
		char *line = strndup(from, next - from);
		char *a = line, *b;
		do {
			int idx = -1;
			int i;
			b = line + strlen(line);
			for ( i = 0; i < 2; ++i ) {
				if (subst[i].key) {
					// printf("trying: %s -> %s\n", subst[i].value, subst[i].key);
					char *stop = strstr(a, subst[i].value);
					if (stop && stop < b) {
						idx = i;
						b = stop;
					}
				}
			}
			fwrite(a, 1, b - a, out);
			a = b;

			if ( idx >= 0 ) {
				fprintf(out, "%s", subst[idx].key);
				a += strlen(subst[idx].value);
			}
		} while (b < line + strlen(line));
		free(line);
	}

	return next - buf + (force ? 0 : 1);
}

static void dump(void) {
	int counter_last = -1, counter = 0;

	while ((counter < (int) readbuf_used) && (counter != counter_last)) {
		counter_last = counter;
		counter = outline( stdout, readbuf, counter, 1 );
	}
}

static void trickle(FILE *out, int *last, int *counter) {
	if (*last > (int) readbuf_used) {
		*last = -1;
		*counter = 0;
	}
	while ((*counter < (int) readbuf_used) && (*counter != *last)) {
		*last = *counter;
		*counter = outline( out, readbuf, *counter, 1 );
	}
}

static void clear(void) {
	readbuf_used = 0;
	fullbuffer = 0;
}

static int64_t _get_time_us(void)
{
       struct timeval tv;

       (void) gettimeofday(&tv, 0);
       return (int64_t) tv.tv_sec * 1000000 + (int64_t) tv.tv_usec;
}

static void _append_buf(const char *buf, size_t len)
{
	if ((readbuf_used + len) >= readbuf_sz) {
		if ((readbuf_sz >= MAX_LOG_SIZE) &&
		    !getenv("LVM_TEST_UNLIMITED")) {
			if (fullbuffer++ ==  0)
				kill(-pid, SIGINT);
			return;
		}
		readbuf_sz = readbuf_sz ? 2 * readbuf_sz : 4096;
		readbuf = realloc(readbuf, readbuf_sz);
	}

	if (!readbuf)
		exit(205);

	memcpy(readbuf + readbuf_used, buf, len);
	readbuf_used += len;
}

static const char *_append_with_stamp(const char *buf, int stamp)
{
	static const char spaces[] = "                 ";
	static int64_t t_last;
	static int64_t t_start = 0;
	int64_t t_now;
	char stamp_buf[32]; /* Bigger to always fit both numbers */
	const char *be;
	const char *bb = buf;
	size_t len;

	while ((be = strchr(bb, '\n'))) {
		if (stamp++ == 0) {
			t_now = _get_time_us();
			if (!t_start)
				t_start = t_last = t_now;
			len = snprintf(stamp_buf, sizeof(stamp_buf),
				       "%8.3f%8.4f ",
				       (t_now - t_start) / 1000000.f,
				       (t_now - t_last) / 1000000.f);
			_append_buf(stamp_buf, (len < (sizeof(spaces) - 1)) ?
				    len : (sizeof(spaces) - 1));
			t_last = t_now;
		}

		_append_buf(bb, be + 1 - bb);
		bb = be + 1;

		if (stamp > 0 && bb[0])
			_append_buf(spaces, sizeof(spaces) - 1);
	}

	return bb;
}

static void drain(int fd)
{
	char buf[4096];
	const char *bp;
	int stamp = 0;
	int sz;

	static int stdout_last = -1, stdout_counter = 0;
	static int outfile_last = -1, outfile_counter = 0;

	while ((sz = read(fd, buf, sizeof(buf) - 1)) > 0) {
		if (fullbuffer)
			continue;
		buf[sz] = '\0';
		bp = (verbose < 2) ? buf : _append_with_stamp(buf, stamp);
		if (sz > (bp - buf)) {
			_append_buf(bp, sz - (bp - buf));
			stamp = -1; /* unfinished line */
		} else
			stamp = 0;

		readbuf[readbuf_used] = 0;

		if (verbose)
			trickle(stdout, &stdout_last, &stdout_counter);
		if (outfile)
			trickle(outfile, &outfile_last, &outfile_counter);
	}
}

static const char *duration(time_t start, const struct rusage *usage)
{
	static char buf[100];
	int t = (int)(time(NULL) - start);

	int p = sprintf(buf, "%2d:%02d", t / 60, t % 60);

	if (usage)
		sprintf(buf + p, "   %2ld:%02ld.%03ld/%ld:%02ld.%03ld%5ld%8ld/%ld",
			usage->ru_utime.tv_sec / 60, usage->ru_utime.tv_sec % 60,
			usage->ru_utime.tv_usec / 1000,
			usage->ru_stime.tv_sec / 60, usage->ru_stime.tv_sec % 60,
			usage->ru_stime.tv_usec / 1000,
			usage->ru_maxrss / 1024,
			usage->ru_inblock, usage->ru_oublock);

	return buf;
}

static void passed(int i, char *f, time_t t, const struct rusage *usage) {
	if (readbuf && strstr(readbuf, "TEST EXPECT FAIL")) {
		++ s.npassed;
		s.status[i] = PASSED;
		printf("passed (UNEXPECTED). %s\n", duration(t, usage));
	} else if (readbuf && strstr(readbuf, "TEST WARNING")) {
		++s.nwarned;
		s.status[i] = WARNED;
		printf("warnings  %s\n", duration(t, usage));
	} else {
		++ s.npassed;
		s.status[i] = PASSED;
		printf("passed.   %s\n", duration(t, usage));
	}
}

static void interrupted(int i, char *f) {
	++ s.ninterrupted;
	s.status[i] = INTERRUPTED;
	printf("\ninterrupted.\n");
	if (!verbose && fullbuffer) {
		printf("-- Interrupted %s ------------------------------------\n", f);
		dump();
		printf("\n-- Interrupted %s (end) ------------------------------\n", f);
	}
}

static void timeout(int i, char *f) {
	++ s.ninterrupted;
	s.status[i] = TIMEOUT;
	printf("timeout.\n");
	if (!verbose && readbuf) {
		printf("-- Timed out %s ------------------------------------\n", f);
		dump();
		printf("\n-- Timed out %s (end) ------------------------------\n", f);
	}
}

static void skipped(int i, char *f) {
	++ s.nskipped;
	s.status[i] = SKIPPED;
	printf("skipped.\n");
}

static void failed(int i, char *f, int st) {
	if (readbuf && strstr(readbuf, "TEST EXPECT FAIL")) {
		printf("FAILED (expected).\n");
		s.status[i] = KNOWNFAIL;
		++ s.nknownfail;
		return;
	}

	++ s.nfailed;
	s.status[i] = FAILED;
	printf("FAILED.\n");
	if (!verbose && readbuf) {
		printf("-- FAILED %s ------------------------------------\n", f);
		dump();
		printf("-- FAILED %s (end) ------------------------------\n", f);
	}
}

static void run(int i, char *f) {
	struct rusage usage;
	char flavour[512], script[512];

	pid = fork();
	if (pid < 0) {
		perror("Fork failed.");
		exit(201);
	} else if (pid == 0) {
		if (!interactive) {
			close(0);
			dup2(fds[0], 1);
			dup2(fds[0], 2);
			close(fds[0]);
			close(fds[1]);
		}
		if (strchr(f, ':')) {
			strcpy(flavour, f);
			*strchr(flavour, ':') = 0;
			setenv("LVM_TEST_FLAVOUR", flavour, 1);
			strcpy(script, strchr(f, ':') + 1);
		} else {
			strcpy(script, f);
		}
		setpgid(0, 0);
		execlp("bash", "bash", "-noprofile", "-norc", script, NULL);
		perror("execlp");
		fflush(stderr);
		_exit(202);
	} else {
		int st = -1, w;
		time_t start = time(NULL);
		char buf[128];
		char outpath[PATH_MAX];
		char *c = outpath + strlen(results) + 1;
		struct timeval selectwait;
		fd_set set;
		int runaway = 0;
		int no_write = 0;
		FILE *varlogmsg;
		int fd_vlm = -1;

		snprintf(buf, sizeof(buf), "%s ...", f);
		printf("Running %-60s ", buf);
		fflush(stdout);
		snprintf(outpath, sizeof(outpath), "%s/%s.txt", results, f);
		while ((c = strchr(c, '/')))
			*c = '_';
		if (!(outfile = fopen(outpath, "w")))
			perror("fopen");

		/* Mix in kernel log message */
		if (!(varlogmsg = fopen("/var/log/messages", "r")))
			perror("fopen");
		else if (((fd_vlm = fileno(varlogmsg)) >= 0) &&
			 fseek(varlogmsg, 0L, SEEK_END))
			perror("fseek");

		while ((w = wait4(pid, &st, WNOHANG, &usage)) == 0) {
			if ((fullbuffer && fullbuffer++ == 8000) ||
			    (no_write > 180 * 2)) /* a 3 minute timeout */
			{
				system("echo t > /proc/sysrq-trigger");
				kill(pid, SIGINT);
				sleep(5); /* wait a bit for a reaction */
				if ((w = waitpid(pid, &st, WNOHANG)) == 0) {
					kill(-pid, SIGKILL);
					w = pid; // waitpid(pid, &st, NULL);
				}
				runaway = 1;
				break;
			}

			FD_ZERO(&set);
			FD_SET(fds[1], &set);
			selectwait.tv_sec = 0;
			selectwait.tv_usec = 500000; /* timeout 0.5s */
			if (select(fds[1] + 1, &set, NULL, NULL, &selectwait) <= 0) {
				no_write++;
				continue;
			}
			drain(fds[1]);
			no_write = 0;
			if (fd_vlm >= 0)
				drain(fd_vlm);
		}
		if (w != pid) {
			perror("waitpid");
			exit(206);
		}
		drain(fds[1]);
		if (fd_vlm >= 0)
			drain(fd_vlm);
		if (die == 2)
			interrupted(i, f);
		else if (runaway) {
			timeout(i, f);
		} else if (WIFEXITED(st)) {
			if (WEXITSTATUS(st) == 0)
				passed(i, f, start, &usage);
			else if (WEXITSTATUS(st) == 200)
				skipped(i, f);
			else
				failed(i, f, st);
		} else
			failed(i, f, st);

		if (varlogmsg)
			fclose(varlogmsg);
		if (outfile)
			fclose(outfile);
		if (fullbuffer)
			printf("\nTest was interrupted, output has got too large (>%u) (loop:%u)\n"
			       "Set LVM_TEST_UNLIMITED=1 for unlimited log.\n",
			       (unsigned) readbuf_sz, fullbuffer);
		clear();
	}
}

int main(int argc, char **argv) {
	char results_list[PATH_MAX];
	const char *result;
	const char *be_verbose = getenv("VERBOSE"),
		   *be_interactive = getenv("INTERACTIVE");
	time_t start = time(NULL);
	int i;
	FILE *list;

	if (argc >= MAX) {
		fprintf(stderr, "Sorry, my head exploded. Please increase MAX.\n");
		exit(1);
	}

	if (be_verbose)
		verbose = atoi(be_verbose);

	if (be_interactive)
		interactive = atoi(be_interactive);

	results = getenv("LVM_TEST_RESULTS") ? : "results";
	(void) snprintf(results_list, sizeof(results_list), "%s/list", results);

	if (socketpair(PF_UNIX, SOCK_STREAM, 0, fds)) {
		perror("socketpair");
		return 201;
	}

	if (fcntl(fds[1], F_SETFL, O_NONBLOCK ) == -1) {
		perror("fcntl on socket");
		return 202;
	}

	/* set up signal handlers */
	for (i = 0; i <= 32; ++i)
		switch (i) {
		case SIGCHLD: case SIGWINCH: case SIGURG:
		case SIGKILL: case SIGSTOP: break;
		default: signal(i, handler);
		}

	/* run the tests */
	for (i = 1; !die && i < argc; ++i)
		run(i, argv[i]);

	free(subst[0].value);
	free(subst[1].value);
	free(readbuf);

	printf("\n## %d tests %s : %d OK, %d warnings, %d failures, %d known failures; "
	       "%d skipped, %d interrupted\n",
	       s.nwarned + s.npassed + s.nfailed + s.nskipped + s.ninterrupted,
	       duration(start, NULL),
	       s.npassed, s.nwarned, s.nfailed, s.nknownfail,
	       s.nskipped, s.ninterrupted);

	/* dump a list to results */
	if ((list = fopen(results_list, "w"))) {
		for (i = 1; i < argc; ++ i) {
			switch (s.status[i]) {
			case PASSED: result = "passed"; break;
			case FAILED: result = "failed"; break;
			case SKIPPED: result = "skipped"; break;
			case WARNED: result = "warnings"; break;
			case TIMEOUT: result = "timeout"; break;
			case INTERRUPTED: result = "interrupted"; break;
			default: result = "unknown"; break;
			}
			fprintf(list, "%s %s\n", argv[i], result);
		}
		fclose(list);
	} else
		perror("fopen result");

	/* print out a summary */
	if (s.nfailed || s.nskipped || s.nknownfail || s.ninterrupted) {
		for (i = 1; i < argc; ++ i) {
			switch (s.status[i]) {
			case FAILED:
				printf("FAILED: %s\n", argv[i]);
				break;
			case KNOWNFAIL:
				printf("FAILED (expected): %s\n", argv[i]);
				break;
			case SKIPPED:
				printf("skipped: %s\n", argv[i]);
				break;
			case INTERRUPTED:
				printf("interrupted: %s\n", argv[i]);
				break;
			case TIMEOUT:
				printf("timeout: %s\n", argv[i]);
				break;
			default: /* do nothing */ ;
			}
		}
		printf("\n");
		return (s.nfailed > 0) || (s.ninterrupted > 0) || die;
	}

	return die;
}
