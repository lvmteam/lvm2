/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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

/*
 * CLVMD: Cluster LVM daemon
 */

#include "clvmd-common.h"

#include <pthread.h>
#include <getopt.h>
#include <ctype.h>

#include "clvmd-comms.h"
#include "clvm.h"
#include "clvmd.h"
#include "lvm-functions.h"
#include "lvm-version.h"
#include "refresh_clvmd.h"

#ifdef HAVE_COROSYNC_CONFDB_H
#include <corosync/confdb.h>
#endif

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <syslog.h>
#include <sys/un.h>
#include <sys/utsname.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define MAX_RETRIES 4
#define MAX_MISSING_LEN 8000 /* Max supported clvmd message size ? */

#define ISLOCAL_CSID(c) (memcmp(c, our_csid, max_csid_len) == 0)

/* Head of the fd list. Also contains
   the cluster_socket details */
static struct local_client local_client_head;

static unsigned short global_xid = 0;	/* Last transaction ID issued */

struct cluster_ops *clops = NULL;

static char our_csid[MAX_CSID_LEN];
static unsigned max_csid_len;
static unsigned max_cluster_message;
static unsigned max_cluster_member_name_len;

/* Structure of items on the LVM thread list */
struct lvm_thread_cmd {
	struct dm_list list;

	struct local_client *client;
	struct clvm_header *msg;
	char csid[MAX_CSID_LEN];
	int remote;		/* Flag */
	int msglen;
	unsigned short xid;
};

struct lvm_startup_params {
	struct dm_hash_table *excl_uuid;
};

static debug_t debug = DEBUG_OFF;
static int foreground_mode = 0;
static pthread_t lvm_thread;
/* Stack size 128KiB for thread, must be bigger then DEFAULT_RESERVED_STACK */
static const size_t STACK_SIZE = 128 * 1024;
static pthread_attr_t stack_attr;
static pthread_mutex_t lvm_thread_mutex;
static pthread_cond_t lvm_thread_cond;
static pthread_barrier_t lvm_start_barrier;
static struct dm_list lvm_cmd_head;
static volatile sig_atomic_t quit = 0;
static volatile sig_atomic_t reread_config = 0;
static int child_pipe[2];

/* Reasons the daemon failed initialisation */
#define DFAIL_INIT       1
#define DFAIL_LOCAL_SOCK 2
#define DFAIL_CLUSTER_IF 3
#define DFAIL_MALLOC     4
#define DFAIL_TIMEOUT    5
#define SUCCESS          0

typedef enum {IF_AUTO, IF_CMAN, IF_OPENAIS, IF_COROSYNC, IF_SINGLENODE} if_type_t;

/* Prototypes for code further down */
static void sigusr2_handler(int sig);
static void sighup_handler(int sig);
static void sigterm_handler(int sig);
static void send_local_reply(struct local_client *client, int status,
			     int clientid);
static void free_reply(struct local_client *client);
static void send_version_message(void);
static void *pre_and_post_thread(void *arg);
static int send_message(void *buf, int msglen, const char *csid, int fd,
			const char *errtext);
static int read_from_local_sock(struct local_client *thisfd);
static int cleanup_zombie(struct local_client *thisfd);
static int process_local_command(struct clvm_header *msg, int msglen,
				 struct local_client *client,
				 unsigned short xid);
static void process_remote_command(struct clvm_header *msg, int msglen, int fd,
				   const char *csid);
static int process_reply(const struct clvm_header *msg, int msglen,
			 const char *csid);
static int open_local_sock(void);
static void close_local_sock(int local_socket);
static int check_local_clvmd(void);
static struct local_client *find_client(int clientid);
static void main_loop(int local_sock, int cmd_timeout);
static void be_daemon(int start_timeout);
static int check_all_clvmds_running(struct local_client *client);
static int local_rendezvous_callback(struct local_client *thisfd, char *buf,
				     int len, const char *csid,
				     struct local_client **new_client);
static void *lvm_thread_fn(void *) __attribute__((noreturn));
static int add_to_lvmqueue(struct local_client *client, struct clvm_header *msg,
			   int msglen, const char *csid);
static int distribute_command(struct local_client *thisfd);
static void hton_clvm(struct clvm_header *hdr);
static void ntoh_clvm(struct clvm_header *hdr);
static void add_reply_to_list(struct local_client *client, int status,
			      const char *csid, const char *buf, int len);
static if_type_t parse_cluster_interface(char *ifname);
static if_type_t get_cluster_type(void);

static void usage(const char *prog, FILE *file)
{
	fprintf(file, "Usage: %s [options]\n"
		"   -V       Show version of clvmd\n"
		"   -h       Show this help information\n"
		"   -d[n]    Set debug logging (0:none, 1:stderr (implies -f option), 2:syslog)\n"
		"   -f       Don't fork, run in the foreground\n"
		"   -E<lockuuid> Take this lock uuid as exclusively locked resource (for restart)\n"
		"   -R       Tell all running clvmds in the cluster to reload their device cache\n"
		"   -S       Restart clvmd, preserving exclusive locks\n"
		"   -C       Sets debug level (from -d) on all clvmd instances clusterwide\n"
		"   -t<secs> Command timeout (default 60 seconds)\n"
		"   -T<secs> Startup timeout (default none)\n"
		"   -I<cmgr> Cluster manager (default: auto)\n"
		"            Available cluster managers: "
#ifdef USE_COROSYNC
		"corosync "
#endif
#ifdef USE_CMAN
		"cman "
#endif
#ifdef USE_OPENAIS
		"openais "
#endif
#ifdef USE_SINGLENODE
		"singlenode "
#endif
		"\n", prog);
}

/* Called to signal the parent how well we got on during initialisation */
static void child_init_signal(int status)
{
	if (child_pipe[1]) {
		/* FIXME Use a proper wrapper around write */
		if (write(child_pipe[1], &status, sizeof(status)) < 0)
			log_sys_error("write", "child_pipe");
		if (close(child_pipe[1]))
			log_sys_error("close", "child_pipe");
	}
}

static __attribute__((noreturn)) void child_init_signal_and_exit(int status) 
{
	child_init_signal(status);
	exit(status);
}

static void safe_close(int *fd)
{
	if (*fd >= 0) {
		int to_close = *fd;
		*fd = -1;
		if (close(to_close))
			log_sys_error("close", ""); /* path */
	}
}

void debuglog(const char *fmt, ...)
{
	time_t P;
	va_list ap;
	static int syslog_init = 0;

	switch (clvmd_get_debug()) {
	case DEBUG_STDERR:
		va_start(ap,fmt);
		time(&P);
		fprintf(stderr, "CLVMD[%x]: %.15s ", (int)pthread_self(), ctime(&P)+4 );
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		break;
	case DEBUG_SYSLOG:
		if (!syslog_init) {
			openlog("clvmd", LOG_PID, LOG_DAEMON);
			syslog_init = 1;
		}

		va_start(ap,fmt);
		vsyslog(LOG_DEBUG, fmt, ap);
		va_end(ap);
		break;
	case DEBUG_OFF:
		break;
	}
}

void clvmd_set_debug(debug_t new_debug)
{
	if (!foreground_mode && new_debug == DEBUG_STDERR)
		new_debug = DEBUG_SYSLOG;

	if (new_debug > DEBUG_SYSLOG)
		new_debug = DEBUG_SYSLOG;

	debug = new_debug;
}

debug_t clvmd_get_debug(void)
{
	return debug;
}

int clvmd_get_foreground(void)
{
	return foreground_mode;
}

static const char *decode_cmd(unsigned char cmdl)
{
	static char buf[128];
	const char *command;

	switch (cmdl) {
	case CLVMD_CMD_TEST:
		command = "TEST";
		break;
	case CLVMD_CMD_LOCK_VG:
		command = "LOCK_VG";
		break;
	case CLVMD_CMD_LOCK_LV:
		command = "LOCK_LV";
		break;
	case CLVMD_CMD_REFRESH:
		command = "REFRESH";
		break;
	case CLVMD_CMD_SET_DEBUG:
		command = "SET_DEBUG";
		break;
	case CLVMD_CMD_GET_CLUSTERNAME:
		command = "GET_CLUSTERNAME";
		break;
	case CLVMD_CMD_VG_BACKUP:
		command = "VG_BACKUP";
		break;
	case CLVMD_CMD_REPLY:
		command = "REPLY";
		break;
	case CLVMD_CMD_VERSION:
		command = "VERSION";
		break;
	case CLVMD_CMD_GOAWAY:
		command = "GOAWAY";
		break;
	case CLVMD_CMD_LOCK:
		command = "LOCK";
		break;
	case CLVMD_CMD_UNLOCK:
		command = "UNLOCK";
		break;
	case CLVMD_CMD_LOCK_QUERY:
		command = "LOCK_QUERY";
		break;
	case CLVMD_CMD_RESTART:
		command = "RESTART";
		break;
	case CLVMD_CMD_SYNC_NAMES:
		command = "SYNC_NAMES";
		break;
	default:
		command = "unknown";
		break;
	}

	snprintf(buf, sizeof(buf), "%s (0x%x)", command, cmdl);

	return buf;
}

static void remove_lockfile(void)
{
	if (unlink(CLVMD_PIDFILE))
		log_sys_error("unlink", CLVMD_PIDFILE);
}

/*
 * clvmd require dm-ioctl capability for operation
 */
static void check_permissions(void)
{
	if (getuid() || geteuid()) {
		log_error("Cannot run as a non-root user.");

		 /*
		  * Fail cleanly here if not run as root, instead of failing
		  * later when attempting a root-only operation 
		  * Preferred exit code from an initscript for this.
		  */
		exit(4);
	}
}

int main(int argc, char *argv[])
{
	int local_sock;
	struct local_client *newfd, *delfd;
	struct lvm_startup_params lvm_params;
	int opt;
	int cmd_timeout = DEFAULT_CMD_TIMEOUT;
	int start_timeout = 0;
	if_type_t cluster_iface = IF_AUTO;
	sigset_t ss;
	debug_t debug_opt = DEBUG_OFF;
	debug_t debug_arg = DEBUG_OFF;
	int clusterwide_opt = 0;
	mode_t old_mask;
	int ret = 1;

	struct option longopts[] = {
		{ "help", 0, 0, 'h' },
		{ NULL, 0, 0, 0 }
	};

	if (!(lvm_params.excl_uuid = dm_hash_create(128))) {
		fprintf(stderr, "Failed to allocate hash table\n");
		return 1;
	}

	/* Deal with command-line arguments */
	opterr = 0;
	optind = 0;
	while ((opt = getopt_long(argc, argv, "vVhfd:t:RST:CI:E:",
				  longopts, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(argv[0], stdout);
			exit(0);

		case 'R':
			check_permissions();
			ret = (refresh_clvmd(1) == 1) ? 0 : 1;
			goto out;

		case 'S':
			check_permissions();
			ret = (restart_clvmd(clusterwide_opt) == 1) ? 0 : 1;
			goto out;

		case 'C':
			clusterwide_opt = 1;
			break;

		case 'd':
			debug_opt = DEBUG_STDERR;
			debug_arg = (debug_t) atoi(optarg);
			if (debug_arg == DEBUG_STDERR)
				foreground_mode = 1;
			break;

		case 'f':
			foreground_mode = 1;
			break;
		case 't':
			cmd_timeout = atoi(optarg);
			if (!cmd_timeout) {
				fprintf(stderr, "command timeout is invalid\n");
				usage(argv[0], stderr);
				exit(1);
			}
			break;
		case 'I':
			cluster_iface = parse_cluster_interface(optarg);
			break;
		case 'E':
			if (!dm_hash_insert(lvm_params.excl_uuid, optarg, optarg)) {
				fprintf(stderr, "Failed to allocate hash entry\n");
				goto out;
			}
			break;
		case 'T':
			start_timeout = atoi(optarg);
			if (start_timeout <= 0) {
				fprintf(stderr, "startup timeout is invalid\n");
				usage(argv[0], stderr);
				exit(1);
			}
			break;

		case 'V':
		        printf("Cluster LVM daemon version: %s\n", LVM_VERSION);
			printf("Protocol version:           %d.%d.%d\n",
			       CLVMD_MAJOR_VERSION, CLVMD_MINOR_VERSION,
			       CLVMD_PATCH_VERSION);
			exit(0);
			break;

		default:
			usage(argv[0], stderr);
			exit(2);
		}
	}

	check_permissions();

	/*
	 * Switch to C locale to avoid reading large locale-archive file
	 * used by some glibc (on some distributions it takes over 100MB).
	 * Daemon currently needs to use mlockall().
	 */
	if (setenv("LC_ALL", "C", 1))
		perror("Cannot set LC_ALL to C");

	/* Setting debug options on an existing clvmd */
	if (debug_opt && !check_local_clvmd()) {
		dm_hash_destroy(lvm_params.excl_uuid);
		return debug_clvmd(debug_arg, clusterwide_opt)==1?0:1;
	}

	clvmd_set_debug(debug_arg);

	/* Fork into the background (unless requested not to) */
	if (!foreground_mode)
		be_daemon(start_timeout);

        (void) dm_prepare_selinux_context(DEFAULT_RUN_DIR, S_IFDIR);
        old_mask = umask(0077);
        if (dm_create_dir(DEFAULT_RUN_DIR) == 0) {
                DEBUGLOG("clvmd: unable to create %s directory\n",
                          DEFAULT_RUN_DIR);
                umask(old_mask);
                exit(1);
        }
        umask(old_mask);

	/* Create pidfile */
	(void) dm_prepare_selinux_context(CLVMD_PIDFILE, S_IFREG);
	if (dm_create_lockfile(CLVMD_PIDFILE) == 0) {
		DEBUGLOG("clvmd: unable to create lockfile\n");
		exit(1);
	}
	(void) dm_prepare_selinux_context(NULL, 0);

	atexit(remove_lockfile);

	DEBUGLOG("CLVMD started\n");

	/* Open the Unix socket we listen for commands on.
	   We do this before opening the cluster socket so that
	   potential clients will block rather than error if we are running
	   but the cluster is not ready yet */
	local_sock = open_local_sock();
	if (local_sock < 0) {
		child_init_signal_and_exit(DFAIL_LOCAL_SOCK);
		/* NOTREACHED */
	}

	/* Set up signal handlers, USR1 is for cluster change notifications (in cman)
	   USR2 causes child threads to exit.
	   (HUP used to cause gulm to re-read the nodes list from CCS.)
	   PIPE should be ignored */
	signal(SIGUSR2, sigusr2_handler);
	signal(SIGHUP,  sighup_handler);
	signal(SIGPIPE, SIG_IGN);

	/* Block SIGUSR2/SIGINT/SIGTERM in process */
	sigemptyset(&ss);
	sigaddset(&ss, SIGUSR2);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGTERM);
	sigprocmask(SIG_BLOCK, &ss, NULL);

	/* Initialise the LVM thread variables */
	dm_list_init(&lvm_cmd_head);
	if (pthread_attr_init(&stack_attr) ||
	    pthread_attr_setstacksize(&stack_attr, STACK_SIZE)) {
		log_sys_error("pthread_attr_init", "");
		exit(1);
	}
	pthread_mutex_init(&lvm_thread_mutex, NULL);
	pthread_cond_init(&lvm_thread_cond, NULL);
	pthread_barrier_init(&lvm_start_barrier, NULL, 2);
	init_lvhash();

	/* Start the cluster interface */
	if (cluster_iface == IF_AUTO)
		cluster_iface = get_cluster_type();

#ifdef USE_CMAN
	if ((cluster_iface == IF_AUTO || cluster_iface == IF_CMAN) && (clops = init_cman_cluster())) {
		max_csid_len = CMAN_MAX_CSID_LEN;
		max_cluster_message = CMAN_MAX_CLUSTER_MESSAGE;
		max_cluster_member_name_len = CMAN_MAX_NODENAME_LEN;
		syslog(LOG_NOTICE, "Cluster LVM daemon started - connected to CMAN");
	}
#endif
#ifdef USE_COROSYNC
	if (!clops)
		if (((cluster_iface == IF_AUTO || cluster_iface == IF_COROSYNC) && (clops = init_corosync_cluster()))) {
			max_csid_len = COROSYNC_CSID_LEN;
			max_cluster_message = COROSYNC_MAX_CLUSTER_MESSAGE;
			max_cluster_member_name_len = COROSYNC_MAX_CLUSTER_MEMBER_NAME_LEN;
			syslog(LOG_NOTICE, "Cluster LVM daemon started - connected to Corosync");
		}
#endif
#ifdef USE_OPENAIS
	if (!clops)
		if ((cluster_iface == IF_AUTO || cluster_iface == IF_OPENAIS) && (clops = init_openais_cluster())) {
			max_csid_len = OPENAIS_CSID_LEN;
			max_cluster_message = OPENAIS_MAX_CLUSTER_MESSAGE;
			max_cluster_member_name_len = OPENAIS_MAX_CLUSTER_MEMBER_NAME_LEN;
			syslog(LOG_NOTICE, "Cluster LVM daemon started - connected to OpenAIS");
		}
#endif
#ifdef USE_SINGLENODE
	if (!clops)
		if (cluster_iface == IF_SINGLENODE && (clops = init_singlenode_cluster())) {
			max_csid_len = SINGLENODE_CSID_LEN;
			max_cluster_message = SINGLENODE_MAX_CLUSTER_MESSAGE;
			max_cluster_member_name_len = MAX_CLUSTER_MEMBER_NAME_LEN;
			syslog(LOG_NOTICE, "Cluster LVM daemon started - running in single-node mode");
		}
#endif

	if (!clops) {
		DEBUGLOG("Can't initialise cluster interface\n");
		log_error("Can't initialise cluster interface\n");
		child_init_signal_and_exit(DFAIL_CLUSTER_IF);
		/* NOTREACHED */
	}
	DEBUGLOG("Cluster ready, doing some more initialisation\n");

	/* Save our CSID */
	clops->get_our_csid(our_csid);

	/* Initialise the FD list head */
	local_client_head.fd = clops->get_main_cluster_fd();
	local_client_head.type = CLUSTER_MAIN_SOCK;
	local_client_head.callback = clops->cluster_fd_callback;

	/* Add the local socket to the list */
	newfd = malloc(sizeof(struct local_client));
	if (!newfd) {
		child_init_signal_and_exit(DFAIL_MALLOC);
		/* NOTREACHED */
	}

	newfd->fd = local_sock;
	newfd->removeme = 0;
	newfd->type = LOCAL_RENDEZVOUS;
	newfd->callback = local_rendezvous_callback;
	newfd->next = local_client_head.next;
	local_client_head.next = newfd;

	/* This needs to be started after cluster initialisation
	   as it may need to take out locks */
	DEBUGLOG("starting LVM thread\n");

	/* Don't let anyone else to do work until we are started */
	pthread_create(&lvm_thread, &stack_attr, lvm_thread_fn, &lvm_params);

	/* Don't start until the LVM thread is ready */
	pthread_barrier_wait(&lvm_start_barrier);

	/* Tell the rest of the cluster our version number */
	if (clops->cluster_init_completed)
		clops->cluster_init_completed();

	DEBUGLOG("clvmd ready for work\n");
	child_init_signal(SUCCESS);

	/* Try to shutdown neatly */
	signal(SIGTERM, sigterm_handler);
	signal(SIGINT, sigterm_handler);

	/* Do some work */
	main_loop(local_sock, cmd_timeout);

	pthread_mutex_lock(&lvm_thread_mutex);
	pthread_cond_signal(&lvm_thread_cond);
	pthread_mutex_unlock(&lvm_thread_mutex);
	if ((errno = pthread_join(lvm_thread, NULL)))
		log_sys_error("pthread_join", "");

	close_local_sock(local_sock);
	destroy_lvm();

	for (newfd = local_client_head.next; newfd != NULL;) {
		delfd = newfd;
		newfd = newfd->next;
		if (delfd->fd == local_sock)
			delfd->fd = -1;
		/*
		 * FIXME:
		 * needs cleanup code from read_from_local_sock() for now
		 * break of 'clvmd' may access already free memory here.
		 */
		safe_close(&(delfd->fd));
		free(delfd);
	}

	ret = 0;
out:
	dm_hash_destroy(lvm_params.excl_uuid);

	return ret;
}

/* Called when the cluster layer has completed initialisation.
   We send the version message */
void clvmd_cluster_init_completed(void)
{
	send_version_message();
}

/* Data on a connected socket */
static int local_sock_callback(struct local_client *thisfd, char *buf, int len,
			       const char *csid,
			       struct local_client **new_client)
{
	*new_client = NULL;
	return read_from_local_sock(thisfd);
}

/* Data on a connected socket */
static int local_rendezvous_callback(struct local_client *thisfd, char *buf,
				     int len, const char *csid,
				     struct local_client **new_client)
{
	/* Someone connected to our local socket, accept it. */

	struct sockaddr_un socka;
	struct local_client *newfd;
	socklen_t sl = sizeof(socka);
	int client_fd = accept(thisfd->fd, (struct sockaddr *) &socka, &sl);

	if (client_fd == -1 && errno == EINTR)
		return 1;

	if (client_fd >= 0) {
		newfd = malloc(sizeof(struct local_client));
		if (!newfd) {
			if (close(client_fd))
                                log_sys_error("close", "socket");
			return 1;
		}

		if (fcntl(client_fd, F_SETFD, 1))
			DEBUGLOG("setting CLOEXEC on client fd failed: %s\n", strerror(errno));

		newfd->fd = client_fd;
		newfd->type = LOCAL_SOCK;
		newfd->xid = 0;
		newfd->removeme = 0;
		newfd->callback = local_sock_callback;
		newfd->bits.localsock.replies = NULL;
		newfd->bits.localsock.expected_replies = 0;
		newfd->bits.localsock.cmd = NULL;
		newfd->bits.localsock.in_progress = FALSE;
		newfd->bits.localsock.sent_out = FALSE;
		newfd->bits.localsock.threadid = 0;
		newfd->bits.localsock.finished = 0;
		newfd->bits.localsock.cleanup_needed = 0;
		newfd->bits.localsock.pipe_client = NULL;
		newfd->bits.localsock.private = NULL;
		newfd->bits.localsock.all_success = 1;
		DEBUGLOG("Got new connection on fd %d\n", newfd->fd);
		*new_client = newfd;
	}
	return 1;
}

static int local_pipe_callback(struct local_client *thisfd, char *buf,
			       int maxlen, const char *csid,
			       struct local_client **new_client)
{
	int len;
	char buffer[PIPE_BUF];
	struct local_client *sock_client = thisfd->bits.pipe.client;
	int status = -1;	/* in error by default */

	len = read(thisfd->fd, buffer, sizeof(int));
	if (len == -1 && errno == EINTR)
		return 1;

	if (len == sizeof(int)) {
		memcpy(&status, buffer, sizeof(int));
	}

	DEBUGLOG("read on PIPE %d: %d bytes: status: %d\n",
		 thisfd->fd, len, status);

	/* EOF on pipe or an error, close it */
	if (len <= 0) {
		void *ret = &status;
		if (close(thisfd->fd))
			log_sys_error("close", "local_pipe");

		/* Clear out the cross-link */
		if (thisfd->bits.pipe.client != NULL)
			thisfd->bits.pipe.client->bits.localsock.pipe_client =
			    NULL;

		/* Reap child thread */
		if (thisfd->bits.pipe.threadid) {
			if ((errno = pthread_join(thisfd->bits.pipe.threadid,
						  &ret)))
				log_sys_error("pthread_join", "");

			thisfd->bits.pipe.threadid = 0;
			if (thisfd->bits.pipe.client != NULL)
				thisfd->bits.pipe.client->bits.localsock.
				    threadid = 0;
		}
		return -1;
	} else {
		DEBUGLOG("background routine status was %d, sock_client=%p\n",
			 status, sock_client);
		/* But has the client gone away ?? */
		if (sock_client == NULL) {
			DEBUGLOG("Got PIPE response for dead client, ignoring it\n");
		} else {
			/* If error then just return that code */
			if (status)
				send_local_reply(sock_client, status,
						 sock_client->fd);
			else {
				/* FIXME: closer inspect this code since state is write thread protected */
				pthread_mutex_lock(&sock_client->bits.localsock.mutex);
				if (sock_client->bits.localsock.state == POST_COMMAND) {
					pthread_mutex_unlock(&sock_client->bits.localsock.mutex);
					send_local_reply(sock_client, 0,
							 sock_client->fd);
				} else {
					/* PRE_COMMAND finished. */
					pthread_mutex_unlock(&sock_client->bits.localsock.mutex);
					if ((status = distribute_command(sock_client)))
						send_local_reply(sock_client, EFBIG,
								 sock_client->fd);
				}
			}
		}
	}
	return len;
}

/* If a noed is up, look for it in the reply array, if it's not there then
   add one with "ETIMEDOUT".
   NOTE: This won't race with real replies because they happen in the same thread.
*/
static void timedout_callback(struct local_client *client, const char *csid,
			      int node_up)
{
	if (node_up) {
		struct node_reply *reply;
		char nodename[max_cluster_member_name_len];

		clops->name_from_csid(csid, nodename);
		DEBUGLOG("Checking for a reply from %s\n", nodename);
		pthread_mutex_lock(&client->bits.localsock.reply_mutex);

		reply = client->bits.localsock.replies;
		while (reply && strcmp(reply->node, nodename) != 0) {
			reply = reply->next;
		}

		pthread_mutex_unlock(&client->bits.localsock.reply_mutex);

		if (!reply) {
			DEBUGLOG("Node %s timed-out\n", nodename);
			add_reply_to_list(client, ETIMEDOUT, csid,
					  "Command timed out", 18);
		}
	}
}

/* Called when the request has timed out on at least one node. We fill in
   the remaining node entries with ETIMEDOUT and return.

   By the time we get here the node that caused
   the timeout could have gone down, in which case we will never get the expected
   number of replies that triggers the post command so we need to do it here
*/
static void request_timed_out(struct local_client *client)
{
	DEBUGLOG("Request timed-out. padding\n");
	clops->cluster_do_node_callback(client, timedout_callback);

	if (client->bits.localsock.num_replies !=
	    client->bits.localsock.expected_replies) {
		/* Post-process the command */
		if (client->bits.localsock.threadid) {
			pthread_mutex_lock(&client->bits.localsock.mutex);
			client->bits.localsock.state = POST_COMMAND;
			pthread_cond_signal(&client->bits.localsock.cond);
			pthread_mutex_unlock(&client->bits.localsock.mutex);
		}
	}
}

/* This is where the real work happens */
static void main_loop(int local_sock, int cmd_timeout)
{
	sigset_t ss;

	DEBUGLOG("Using timeout of %d seconds\n", cmd_timeout);

	sigemptyset(&ss);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGTERM);
	pthread_sigmask(SIG_UNBLOCK, &ss, NULL);
	/* Main loop */
	while (!quit) {
		fd_set in;
		int select_status;
		struct local_client *thisfd;
		struct timeval tv = { cmd_timeout, 0 };
		int quorate = clops->is_quorate();

		/* Wait on the cluster FD and all local sockets/pipes */
		local_client_head.fd = clops->get_main_cluster_fd();
		FD_ZERO(&in);
		for (thisfd = &local_client_head; thisfd != NULL;
		     thisfd = thisfd->next) {

			if (thisfd->removeme)
				continue;

			/* if the cluster is not quorate then don't listen for new requests */
			if ((thisfd->type != LOCAL_RENDEZVOUS &&
			     thisfd->type != LOCAL_SOCK) || quorate)
				FD_SET(thisfd->fd, &in);
		}

		select_status = select(FD_SETSIZE, &in, NULL, NULL, &tv);

		if (reread_config) {
			int saved_errno = errno;

			reread_config = 0;
			DEBUGLOG("got SIGHUP\n");
			if (clops->reread_config)
				clops->reread_config();
			errno = saved_errno;
		}

		if (select_status > 0) {
			struct local_client *lastfd = NULL;
			char csid[MAX_CSID_LEN];
			char buf[max_cluster_message];

			for (thisfd = &local_client_head; thisfd != NULL;
			     thisfd = thisfd->next) {

				if (thisfd->removeme && !cleanup_zombie(thisfd)) {
					struct local_client *free_fd;
					lastfd->next = thisfd->next;
					free_fd = thisfd;

					DEBUGLOG("removeme set for fd %d\n", free_fd->fd);

					/* Queue cleanup, this also frees the client struct */
					add_to_lvmqueue(free_fd, NULL, 0, NULL);
					break;
				}

				if (FD_ISSET(thisfd->fd, &in)) {
					struct local_client *newfd = NULL;
					int ret;

					/* Do callback */
					ret =
					    thisfd->callback(thisfd, buf,
							     sizeof(buf), csid,
							     &newfd);
					/* Ignore EAGAIN */
					if (ret < 0 && (errno == EAGAIN ||
							errno == EINTR)) continue;

					/* Got error or EOF: Remove it from the list safely */
					if (ret <= 0) {
						int type = thisfd->type;

						/* If the cluster socket shuts down, so do we */
						if (type == CLUSTER_MAIN_SOCK ||
						    type == CLUSTER_INTERNAL)
							goto closedown;

						DEBUGLOG("ret == %d, errno = %d. removing client\n",
							 ret, errno);
						thisfd->removeme = 1;
						break;
					}

					/* New client...simply add it to the list */
					if (newfd) {
						newfd->next = thisfd->next;
						thisfd->next = newfd;
						break;
					}
				}
				lastfd = thisfd;
			}
		}

		/* Select timed out. Check for clients that have been waiting too long for a response */
		if (select_status == 0) {
			time_t the_time = time(NULL);

			for (thisfd = &local_client_head; thisfd != NULL;
			     thisfd = thisfd->next) {
				if (thisfd->type == LOCAL_SOCK
				    && thisfd->bits.localsock.sent_out
				    && thisfd->bits.localsock.sent_time +
				    cmd_timeout < the_time
				    && thisfd->bits.localsock.
				    expected_replies !=
				    thisfd->bits.localsock.num_replies) {
					/* Send timed out message + replies we already have */
					DEBUGLOG
					    ("Request timed-out (send: %ld, now: %ld)\n",
					     thisfd->bits.localsock.sent_time,
					     the_time);

					thisfd->bits.localsock.all_success = 0;

					request_timed_out(thisfd);
				}
			}
		}
		if (select_status < 0) {
			if (errno == EINTR)
				continue;

#ifdef DEBUG
			perror("select error");
			exit(-1);
#endif
		}
	}

      closedown:
	clops->cluster_closedown();
	if (quit)
		DEBUGLOG("SIGTERM received\n");
}

static __attribute__ ((noreturn)) void wait_for_child(int c_pipe, int timeout)
{
	int child_status;
	int sstat;
	fd_set fds;
	struct timeval tv = {timeout, 0};

	FD_ZERO(&fds);
	FD_SET(c_pipe, &fds);

	sstat = select(c_pipe+1, &fds, NULL, NULL, timeout? &tv: NULL);
	if (sstat == 0) {
		fprintf(stderr, "clvmd startup timed out\n");
		exit(DFAIL_TIMEOUT);
	}
	if (sstat == 1) {
		if (read(c_pipe, &child_status, sizeof(child_status)) !=
		    sizeof(child_status)) {

			fprintf(stderr, "clvmd failed in initialisation\n");
			exit(DFAIL_INIT);
		}
		else {
			switch (child_status) {
			case SUCCESS:
				break;
			case DFAIL_INIT:
				fprintf(stderr, "clvmd failed in initialisation\n");
				break;
			case DFAIL_LOCAL_SOCK:
				fprintf(stderr, "clvmd could not create local socket\n");
				fprintf(stderr, "Another clvmd is probably already running\n");
				break;
			case DFAIL_CLUSTER_IF:
				fprintf(stderr, "clvmd could not connect to cluster manager\n");
				fprintf(stderr, "Consult syslog for more information\n");
				break;
			case DFAIL_MALLOC:
				fprintf(stderr, "clvmd failed, not enough memory\n");
				break;
			default:
				fprintf(stderr, "clvmd failed, error was %d\n", child_status);
				break;
			}
			exit(child_status);
		}
	}
	fprintf(stderr, "clvmd startup, select failed: %s\n", strerror(errno));
	exit(DFAIL_INIT);
}

/*
 * Fork into the background and detach from our parent process.
 * In the interests of user-friendliness we wait for the daemon
 * to complete initialisation before returning its status
 * the the user.
 */
static void be_daemon(int timeout)
{
	int devnull = open("/dev/null", O_RDWR);
	if (devnull == -1) {
		perror("Can't open /dev/null");
		exit(3);
	}

	if (pipe(child_pipe)) {
		perror("Error creating pipe");
		exit(3);
	}

	switch (fork()) {
	case -1:
		perror("clvmd: can't fork");
		exit(2);

	case 0:		/* Child */
		(void) close(child_pipe[0]);
		break;

	default:       /* Parent */
		(void) close(child_pipe[1]);
		wait_for_child(child_pipe[0], timeout);
	}

	/* Detach ourself from the calling environment */
	if (close(0) || close(1) || close(2)) {
		perror("Error closing terminal FDs");
		exit(4);
	}
	setsid();

	if (dup2(devnull, 0) < 0 || dup2(devnull, 1) < 0
	    || dup2(devnull, 2) < 0) {
		perror("Error setting terminal FDs to /dev/null");
		log_error("Error setting terminal FDs to /dev/null: %m");
		exit(5);
	}
	if ((devnull > STDERR_FILENO) && close(devnull)) {
		log_sys_error("close", "/dev/null");
		exit(7);
	}
	if (chdir("/")) {
		log_error("Error setting current directory to /: %m");
		exit(6);
	}
}

static int verify_message(char *buf, int len)
{
	struct clvm_header *h = (struct clvm_header *)buf;

	if (len < sizeof(struct clvm_header)) {
		log_error("verify_message short len %d", len);
		return -1;
	}

	switch (h->cmd) {
	case CLVMD_CMD_REPLY:
	case CLVMD_CMD_VERSION:
	case CLVMD_CMD_GOAWAY:
	case CLVMD_CMD_TEST:
	case CLVMD_CMD_LOCK:
	case CLVMD_CMD_UNLOCK:
	case CLVMD_CMD_LOCK_LV:
	case CLVMD_CMD_LOCK_VG:
	case CLVMD_CMD_LOCK_QUERY:
	case CLVMD_CMD_REFRESH:
	case CLVMD_CMD_GET_CLUSTERNAME:
	case CLVMD_CMD_SET_DEBUG:
	case CLVMD_CMD_VG_BACKUP:
	case CLVMD_CMD_RESTART:
	case CLVMD_CMD_SYNC_NAMES:
		break;
	default:
		log_error("verify_message bad cmd %x", h->cmd);
		return -1;
	};

	/* TODO: we may be able to narrow len/flags/clientid/arglen checks based on cmd */

	if (h->flags & ~(CLVMD_FLAG_LOCAL | CLVMD_FLAG_SYSTEMLV | CLVMD_FLAG_NODEERRS)) {
		log_error("verify_message bad flags %x", h->flags);
		return -1;
	}

	if (h->arglen > max_cluster_message) {
		log_error("verify_message bad arglen %x max %d", h->arglen, max_cluster_message);
		return -1;
	}

	return 0;
}

static void dump_message(char *buf, int len)
{
	unsigned char row[8];
	char str[9];
	int i, j, pos;

	if (len > 128)
		len = 128;

	pos = 0;
	memset(row, 0, sizeof(row));

	for (i = 0; i < len; i++) {
		row[pos++] = buf[i];

		if ((pos == 8) || (i + 1 == len)) {
			memset(str, 0, sizeof(str));

			for (j = 0; j < 8; j++) {
				if (isprint(row[j]))
					str[j] = row[j];
				else
					str[j] = ' ';
			}

			log_error("%02x %02x %02x %02x %02x %02x %02x %02x [%s]",
				  row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7], str);
			pos = 0;
			memset(row, 0, sizeof(row));
		}
	}
}

static int cleanup_zombie(struct local_client *thisfd)
{
	int *status;

	if (thisfd->type != LOCAL_SOCK)
		return 0;

	if (!thisfd->bits.localsock.cleanup_needed)
		return 0;

	DEBUGLOG("EOF on local socket: inprogress=%d\n",
		 thisfd->bits.localsock.in_progress);

	thisfd->bits.localsock.finished = 1;

	/* If the client went away in mid command then tidy up */
	if (thisfd->bits.localsock.in_progress) {
		pthread_kill(thisfd->bits.localsock.threadid, SIGUSR2);
		if (pthread_mutex_trylock(&thisfd->bits.localsock.mutex))
			goto bail;
		thisfd->bits.localsock.state = POST_COMMAND;
		pthread_cond_signal(&thisfd->bits.localsock.cond);
		pthread_mutex_unlock(&thisfd->bits.localsock.mutex);

		/* Free any unsent buffers */
		free_reply(thisfd);
	}

	/* Kill the subthread & free resources */
	if (thisfd->bits.localsock.threadid) {
		DEBUGLOG("Waiting for child thread\n");
		pthread_mutex_lock(&thisfd->bits.localsock.mutex);
		thisfd->bits.localsock.state = PRE_COMMAND;
		pthread_cond_signal(&thisfd->bits.localsock.cond);
		pthread_mutex_unlock(&thisfd->bits.localsock.mutex);

		if ((errno = pthread_join(thisfd->bits.localsock.threadid,
					  (void **) &status)))
			log_sys_error("pthread_join", "");

		DEBUGLOG("Joined child thread\n");

		thisfd->bits.localsock.threadid = 0;
		pthread_cond_destroy(&thisfd->bits.localsock.cond);
		pthread_mutex_destroy(&thisfd->bits.localsock.mutex);

		/* Remove the pipe client */
		if (thisfd->bits.localsock.pipe_client != NULL) {
			struct local_client *newfd;
			struct local_client *lastfd = NULL;
			struct local_client *free_fd = NULL;

			(void) close(thisfd->bits.localsock.pipe_client->fd);	/* Close pipe */
			(void) close(thisfd->bits.localsock.pipe);

			/* Remove pipe client */
			for (newfd = &local_client_head; newfd != NULL;
			     newfd = newfd->next) {
				if (thisfd->bits.localsock.
				    pipe_client == newfd) {
					thisfd->bits.localsock.
					    pipe_client = NULL;

					lastfd->next = newfd->next;
					free_fd = newfd;
					newfd->next = lastfd;
					free(free_fd);
					break;
				}
				lastfd = newfd;
			}
		}
	}

	/* Free the command buffer */
	free(thisfd->bits.localsock.cmd);

	/* Clear out the cross-link */
	if (thisfd->bits.localsock.pipe_client != NULL)
		thisfd->bits.localsock.pipe_client->bits.pipe.client =
		    NULL;

	safe_close(&(thisfd->fd));
	thisfd->bits.localsock.cleanup_needed = 0;
	return 0;
bail:
	return 1;
}

/* Called when we have a read from the local socket.
   was in the main loop but it's grown up and is a big girl now */
static int read_from_local_sock(struct local_client *thisfd)
{
	int len;
	int argslen;
	int missing_len;
	char buffer[PIPE_BUF + 1];

	memset(buffer, 0, sizeof(buffer));

	len = read(thisfd->fd, buffer, sizeof(buffer) - 1);
	if (len == -1 && errno == EINTR)
		return 1;

	DEBUGLOG("Read on local socket %d, len = %d\n", thisfd->fd, len);

	if (len) {
		int rv = verify_message(buffer, len);
		if (rv < 0) {
			log_error("read_from_local_sock from %d len %d bad verify",
				  thisfd->fd, len);
			dump_message(buffer, len);
			/* force error handling below */
			len = 0;
		}
	}

	/* EOF or error on socket */
	if (len <= 0) {
		thisfd->bits.localsock.cleanup_needed = 1;
		cleanup_zombie(thisfd); /* we ignore errors here */
		return 0;
	} else {
		int comms_pipe[2];
		struct local_client *newfd;
		char csid[MAX_CSID_LEN];
		struct clvm_header *inheader;
		int status;

		buffer[len] = 0; /* Ensure \0 terminated */
		inheader = (struct clvm_header *) buffer;

		/* Fill in the client ID */
		inheader->clientid = htonl(thisfd->fd);

		/* If we are already busy then return an error */
		if (thisfd->bits.localsock.in_progress) {
			struct clvm_header reply = {
				.cmd = CLVMD_CMD_REPLY,
				.status = EBUSY
			};
			send_message(&reply, sizeof(reply), our_csid,
				     thisfd->fd,
				     "Error sending EBUSY reply to local user");
			return len;
		}

		/* See if we have the whole message */
		argslen =
		    len - strlen(inheader->node) - sizeof(struct clvm_header);
		missing_len = inheader->arglen - argslen;

		if (missing_len < 0)
			missing_len = 0;

		/* We need at least sizeof(struct clvm_header) bytes in buffer */
		if (len < (int)sizeof(struct clvm_header) || argslen < 0 ||
		    missing_len > MAX_MISSING_LEN) {
			struct clvm_header reply = {
				.cmd = CLVMD_CMD_REPLY,
				.status = EINVAL
			};
			send_message(&reply, sizeof(reply), our_csid,
				     thisfd->fd,
				     "Error sending EINVAL reply to local user");
			return 0;
		}

		/* Free any old buffer space */
		free(thisfd->bits.localsock.cmd);

		/* Save the message */
		thisfd->bits.localsock.cmd = malloc(len + missing_len);

		if (!thisfd->bits.localsock.cmd) {
			struct clvm_header reply = {
				.cmd = CLVMD_CMD_REPLY,
				.status = ENOMEM
			};
			send_message(&reply, sizeof(reply), our_csid,
				     thisfd->fd,
				     "Error sending ENOMEM reply to local user");
			return 0;
		}
		memcpy(thisfd->bits.localsock.cmd, buffer, len);
		thisfd->bits.localsock.cmd_len = len + missing_len;
		inheader = (struct clvm_header *) thisfd->bits.localsock.cmd;

		/* If we don't have the full message then read the rest now */
		if (missing_len) {
			char *argptr =
			    inheader->node + strlen(inheader->node) + 1;

			while (missing_len > 0) {
				DEBUGLOG("got %d bytes, need another %d (total %d)\n",
					 argslen, missing_len, inheader->arglen);
				len = read(thisfd->fd, argptr + argslen,
					   missing_len);
				if (len == -1 && errno == EINTR)
					continue;
				if (len > 0) {
					missing_len -= len;
					argslen += len;
				} else {
					/* EOF or error on socket */
					DEBUGLOG("EOF on local socket\n");
					free(thisfd->bits.localsock.cmd);
					thisfd->bits.localsock.cmd = NULL;
					return 0;
				}
			}
		}

		/* Initialise and lock the mutex so the subthread will wait after
		   finishing the PRE routine */
		if (!thisfd->bits.localsock.threadid) {
			pthread_mutex_init(&thisfd->bits.localsock.mutex, NULL);
			pthread_cond_init(&thisfd->bits.localsock.cond, NULL);
			pthread_mutex_init(&thisfd->bits.localsock.reply_mutex, NULL);
		}

		/* Only run the command if all the cluster nodes are running CLVMD */
		if (((inheader->flags & CLVMD_FLAG_LOCAL) == 0) &&
		    (check_all_clvmds_running(thisfd) == -1)) {
			thisfd->bits.localsock.expected_replies = 0;
			thisfd->bits.localsock.num_replies = 0;
			send_local_reply(thisfd, EHOSTDOWN, thisfd->fd);
			return len;
		}

		/* Check the node name for validity */
		if (inheader->node[0] && clops->csid_from_name(csid, inheader->node)) {
			/* Error, node is not in the cluster */
			struct clvm_header reply = {
				.cmd = CLVMD_CMD_REPLY,
				.status = ENOENT
			};

			DEBUGLOG("Unknown node: '%s'\n", inheader->node);
			send_message(&reply, sizeof(reply), our_csid,
				     thisfd->fd,
				     "Error sending ENOENT reply to local user");
			thisfd->bits.localsock.expected_replies = 0;
			thisfd->bits.localsock.num_replies = 0;
			thisfd->bits.localsock.in_progress = FALSE;
			thisfd->bits.localsock.sent_out = FALSE;
			return len;
		}

		/* If we already have a subthread then just signal it to start */
		if (thisfd->bits.localsock.threadid) {
			pthread_mutex_lock(&thisfd->bits.localsock.mutex);
			thisfd->bits.localsock.state = PRE_COMMAND;
			pthread_cond_signal(&thisfd->bits.localsock.cond);
			pthread_mutex_unlock(&thisfd->bits.localsock.mutex);
			return len;
		}

		/* Create a pipe and add the reading end to our FD list */
		if (pipe(comms_pipe)) {
			struct clvm_header reply = {
				.cmd = CLVMD_CMD_REPLY,
				.status = EBUSY
			};

			DEBUGLOG("creating pipe failed: %s\n", strerror(errno));
			send_message(&reply, sizeof(reply), our_csid,
				     thisfd->fd,
				     "Error sending EBUSY reply to local user");
			return len;
		}

		newfd = malloc(sizeof(struct local_client));
		if (!newfd) {
			struct clvm_header reply = {
				.cmd = CLVMD_CMD_REPLY,
				.status = ENOMEM
			};

			(void) close(comms_pipe[0]);
			(void) close(comms_pipe[1]);

			send_message(&reply, sizeof(reply), our_csid,
				     thisfd->fd,
				     "Error sending ENOMEM reply to local user");
			return len;
		}
		DEBUGLOG("creating pipe, [%d, %d]\n", comms_pipe[0],
			 comms_pipe[1]);

		if (fcntl(comms_pipe[0], F_SETFD, 1))
			DEBUGLOG("setting CLOEXEC on pipe[0] failed: %s\n", strerror(errno));
		if (fcntl(comms_pipe[1], F_SETFD, 1))
			DEBUGLOG("setting CLOEXEC on pipe[1] failed: %s\n", strerror(errno));

		newfd->fd = comms_pipe[0];
		newfd->removeme = 0;
		newfd->type = THREAD_PIPE;
		newfd->callback = local_pipe_callback;
		newfd->next = thisfd->next;
		newfd->bits.pipe.client = thisfd;
		newfd->bits.pipe.threadid = 0;
		thisfd->next = newfd;

		/* Store a cross link to the pipe */
		thisfd->bits.localsock.pipe_client = newfd;

		thisfd->bits.localsock.pipe = comms_pipe[1];

		/* Make sure the thread has a copy of it's own ID */
		newfd->bits.pipe.threadid = thisfd->bits.localsock.threadid;

		/* Run the pre routine */
		thisfd->bits.localsock.in_progress = TRUE;
		thisfd->bits.localsock.state = PRE_COMMAND;
		DEBUGLOG("Creating pre&post thread\n");
		status = pthread_create(&thisfd->bits.localsock.threadid,
					&stack_attr, pre_and_post_thread, thisfd);
		DEBUGLOG("Created pre&post thread, state = %d\n", status);
	}
	return len;
}

/* Add a file descriptor from the cluster or comms interface to
   our list of FDs for select
*/
int add_client(struct local_client *new_client)
{
	new_client->next = local_client_head.next;
	local_client_head.next = new_client;

	return 0;
}

/* Called when the pre-command has completed successfully - we
   now execute the real command on all the requested nodes */
static int distribute_command(struct local_client *thisfd)
{
	struct clvm_header *inheader =
	    (struct clvm_header *) thisfd->bits.localsock.cmd;
	int len = thisfd->bits.localsock.cmd_len;

	thisfd->xid = global_xid++;
	DEBUGLOG("distribute command: XID = %d, flags=0x%x (%s%s)\n",
		 thisfd->xid, inheader->flags,
		(inheader->flags & CLVMD_FLAG_LOCAL) ? "LOCAL" : "",
		(inheader->flags & CLVMD_FLAG_REMOTE) ? "REMOTE" : "");

	/* Forward it to other nodes in the cluster if needed */
	if (!(inheader->flags & CLVMD_FLAG_LOCAL)) {
		/* if node is empty then do it on the whole cluster */
		if (inheader->node[0] == '\0') {
			thisfd->bits.localsock.expected_replies =
			    clops->get_num_nodes();
			thisfd->bits.localsock.num_replies = 0;
			thisfd->bits.localsock.sent_time = time(NULL);
			thisfd->bits.localsock.in_progress = TRUE;
			thisfd->bits.localsock.sent_out = TRUE;

			/*
			 * Send to local node first, even if CLVMD_FLAG_REMOTE
			 * is set so we still get a reply if this is the
			 * only node.
			 */
			add_to_lvmqueue(thisfd, inheader, len, NULL);

			DEBUGLOG("Sending message to all cluster nodes\n");
			inheader->xid = thisfd->xid;
			send_message(inheader, len, NULL, -1,
				     "Error forwarding message to cluster");
		} else {
                        /* Do it on a single node */
			char csid[MAX_CSID_LEN];

			if (clops->csid_from_name(csid, inheader->node)) {
				/* This has already been checked so should not happen */
				return 0;
			} else {
			        /* OK, found a node... */
				thisfd->bits.localsock.expected_replies = 1;
				thisfd->bits.localsock.num_replies = 0;
				thisfd->bits.localsock.in_progress = TRUE;

				/* Are we the requested node ?? */
				if (memcmp(csid, our_csid, max_csid_len) == 0) {
					DEBUGLOG("Doing command on local node only\n");
					add_to_lvmqueue(thisfd, inheader, len, NULL);
				} else {
					DEBUGLOG("Sending message to single node: %s\n",
						 inheader->node);
					inheader->xid = thisfd->xid;
					send_message(inheader, len,
						     csid, -1,
						     "Error forwarding message to cluster node");
				}
			}
		}
	} else {
		/* Local explicitly requested, ignore nodes */
		thisfd->bits.localsock.in_progress = TRUE;
		thisfd->bits.localsock.expected_replies = 1;
		thisfd->bits.localsock.num_replies = 0;
		add_to_lvmqueue(thisfd, inheader, len, NULL);
	}
	return 0;
}

/* Process a command from a remote node and return the result */
static void process_remote_command(struct clvm_header *msg, int msglen, int fd,
			    	   const char *csid)
{
	char *replyargs;
	char nodename[max_cluster_member_name_len];
	int replylen = 0;
	int buflen = max_cluster_message - sizeof(struct clvm_header) - 1;
	int status;

	/* Get the node name as we /may/ need it later */
	clops->name_from_csid(csid, nodename);

	DEBUGLOG("process_remote_command %s for clientid 0x%x XID %d on node %s\n",
		 decode_cmd(msg->cmd), msg->clientid, msg->xid, nodename);

	/* Check for GOAWAY and sulk */
	if (msg->cmd == CLVMD_CMD_GOAWAY) {

		DEBUGLOG("Told to go away by %s\n", nodename);
		log_error("Told to go away by %s\n", nodename);
		exit(99);
	}

	/* Version check is internal - don't bother exposing it in
	   clvmd-command.c */
	if (msg->cmd == CLVMD_CMD_VERSION) {
		int version_nums[3];
		char node[256];

		memcpy(version_nums, msg->args, sizeof(version_nums));

		clops->name_from_csid(csid, node);
		DEBUGLOG("Remote node %s is version %d.%d.%d\n",
			 node,
			 ntohl(version_nums[0]),
			 ntohl(version_nums[1]), ntohl(version_nums[2]));

		if (ntohl(version_nums[0]) != CLVMD_MAJOR_VERSION) {
			struct clvm_header byebyemsg;
			DEBUGLOG
			    ("Telling node %s to go away because of incompatible version number\n",
			     node);
			log_notice
			    ("Telling node %s to go away because of incompatible version number %d.%d.%d\n",
			     node, ntohl(version_nums[0]),
			     ntohl(version_nums[1]), ntohl(version_nums[2]));

			byebyemsg.cmd = CLVMD_CMD_GOAWAY;
			byebyemsg.status = 0;
			byebyemsg.flags = 0;
			byebyemsg.arglen = 0;
			byebyemsg.clientid = 0;
			clops->cluster_send_message(&byebyemsg, sizeof(byebyemsg),
					     our_csid,
					     "Error Sending GOAWAY message");
		} else {
			clops->add_up_node(csid);
		}
		return;
	}

	/* Allocate a default reply buffer */
	replyargs = malloc(max_cluster_message - sizeof(struct clvm_header));

	if (replyargs != NULL) {
		/* Run the command */
		/* FIXME: usage of init_test() is unprotected */
		status = do_command(NULL, msg, msglen, &replyargs,
				    buflen, &replylen);
	} else {
		status = ENOMEM;
	}

	/* If it wasn't a reply, then reply */
	if (msg->cmd != CLVMD_CMD_REPLY) {
		char *aggreply;

		aggreply =
		    realloc(replyargs, replylen + sizeof(struct clvm_header));
		if (aggreply) {
			struct clvm_header *agghead =
			    (struct clvm_header *) aggreply;

			replyargs = aggreply;
			/* Move it up so there's room for a header in front of the data */
			memmove(aggreply + offsetof(struct clvm_header, args),
				replyargs, replylen);

			agghead->xid = msg->xid;
			agghead->cmd = CLVMD_CMD_REPLY;
			agghead->status = status;
			agghead->flags = 0;
			agghead->clientid = msg->clientid;
			agghead->arglen = replylen;
			agghead->node[0] = '\0';
			send_message(aggreply,
				     sizeof(struct clvm_header) +
				     replylen, csid, fd,
				     "Error sending command reply");
		} else {
			struct clvm_header head;

			DEBUGLOG("Error attempting to realloc return buffer\n");
			/* Return a failure response */
			head.cmd = CLVMD_CMD_REPLY;
			head.status = ENOMEM;
			head.flags = 0;
			head.clientid = msg->clientid;
			head.arglen = 0;
			head.node[0] = '\0';
			send_message(&head, sizeof(struct clvm_header), csid,
				     fd, "Error sending ENOMEM command reply");
			return;
		}
	}

	free(replyargs);
}

/* Add a reply to a command to the list of replies for this client.
   If we have got a full set then send them to the waiting client down the local
   socket */
static void add_reply_to_list(struct local_client *client, int status,
			      const char *csid, const char *buf, int len)
{
	struct node_reply *reply;

	pthread_mutex_lock(&client->bits.localsock.reply_mutex);

	/* Add it to the list of replies */
	reply = malloc(sizeof(struct node_reply));
	if (reply) {
		reply->status = status;
		clops->name_from_csid(csid, reply->node);
		DEBUGLOG("Reply from node %s: %d bytes\n", reply->node, len);

		if (len > 0) {
			reply->replymsg = malloc(len);
			if (!reply->replymsg) {
				reply->status = ENOMEM;
			} else {
				memcpy(reply->replymsg, buf, len);
			}
		} else {
			reply->replymsg = NULL;
		}
		/* Hook it onto the reply chain */
		reply->next = client->bits.localsock.replies;
		client->bits.localsock.replies = reply;
	} else {
		/* It's all gone horribly wrong... */
		pthread_mutex_unlock(&client->bits.localsock.reply_mutex);
		send_local_reply(client, ENOMEM, client->fd);
		return;
	}
	DEBUGLOG("Got %d replies, expecting: %d\n",
		 client->bits.localsock.num_replies + 1,
		 client->bits.localsock.expected_replies);

	/* If we have the whole lot then do the post-process */
	if (++client->bits.localsock.num_replies ==
	    client->bits.localsock.expected_replies) {
		/* Post-process the command */
		if (client->bits.localsock.threadid) {
			pthread_mutex_lock(&client->bits.localsock.mutex);
			client->bits.localsock.state = POST_COMMAND;
			pthread_cond_signal(&client->bits.localsock.cond);
			pthread_mutex_unlock(&client->bits.localsock.mutex);
		}
	}
	pthread_mutex_unlock(&client->bits.localsock.reply_mutex);
}

/* This is the thread that runs the PRE and post commands for a particular connection */
static __attribute__ ((noreturn)) void *pre_and_post_thread(void *arg)
{
	struct local_client *client = (struct local_client *) arg;
	int status;
	int write_status;
	sigset_t ss;
	int pipe_fd = client->bits.localsock.pipe;

	DEBUGLOG("in sub thread: client = %p\n", client);
	pthread_mutex_lock(&client->bits.localsock.mutex);

	/* Ignore SIGUSR1 (handled by master process) but enable
	   SIGUSR2 (kills subthreads) */
	sigemptyset(&ss);
	sigaddset(&ss, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &ss, NULL);

	sigdelset(&ss, SIGUSR1);
	sigaddset(&ss, SIGUSR2);
	pthread_sigmask(SIG_UNBLOCK, &ss, NULL);

	/* Loop around doing PRE and POST functions until the client goes away */
	while (!client->bits.localsock.finished) {
		/* Execute the code */
		/* FIXME: usage of init_test() is unprotected as in do_command() */
		status = do_pre_command(client);

		if (status)
			client->bits.localsock.all_success = 0;

		DEBUGLOG("Writing status %d down pipe %d\n", status, pipe_fd);

		/* Tell the parent process we have finished this bit */
		do {
			write_status = write(pipe_fd, &status, sizeof(int));
			if (write_status == sizeof(int))
				break;
			if (write_status < 0 &&
			    (errno == EINTR || errno == EAGAIN))
				continue;
			log_error("Error sending to pipe: %m\n");
			break;
		} while(1);

		if (status) {
			client->bits.localsock.state = POST_COMMAND;
			goto next_pre;
		}

		/* We may need to wait for the condition variable before running the post command */
		DEBUGLOG("Waiting to do post command - state = %d\n",
			 client->bits.localsock.state);

		if (client->bits.localsock.state != POST_COMMAND &&
		    !client->bits.localsock.finished) {
			pthread_cond_wait(&client->bits.localsock.cond,
					  &client->bits.localsock.mutex);
		}

		DEBUGLOG("Got post command condition...\n");

		/* POST function must always run, even if the client aborts */
		status = 0;
		do_post_command(client);

		do {
			write_status = write(pipe_fd, &status, sizeof(int));
			if (write_status == sizeof(int))
				break;
			if (write_status < 0 &&
			    (errno == EINTR || errno == EAGAIN))
				continue;
			log_error("Error sending to pipe: %m\n");
			break;
		} while(1);
next_pre:
		DEBUGLOG("Waiting for next pre command\n");

		if (client->bits.localsock.state != PRE_COMMAND &&
		    !client->bits.localsock.finished) {
			pthread_cond_wait(&client->bits.localsock.cond,
					  &client->bits.localsock.mutex);
		}

		DEBUGLOG("Got pre command condition...\n");
	}
	pthread_mutex_unlock(&client->bits.localsock.mutex);
	DEBUGLOG("Subthread finished\n");
	pthread_exit((void *) 0);
}

/* Process a command on the local node and store the result */
static int process_local_command(struct clvm_header *msg, int msglen,
				 struct local_client *client,
				 unsigned short xid)
{
	char *replybuf = malloc(max_cluster_message);
	int buflen = max_cluster_message - sizeof(struct clvm_header) - 1;
	int replylen = 0;
	int status;

	DEBUGLOG("process_local_command: %s msg=%p, msglen =%d, client=%p\n",
		 decode_cmd(msg->cmd), msg, msglen, client);

	if (replybuf == NULL)
		return -1;

	/* If remote flag is set, just set a successful status code. */
	if (msg->flags & CLVMD_FLAG_REMOTE)
		status = 0;
	else
		/* FIXME: usage of init_test() is unprotected */
		status = do_command(client, msg, msglen, &replybuf, buflen, &replylen);

	if (status)
		client->bits.localsock.all_success = 0;

	/* If we took too long then discard the reply */
	if (xid == client->xid) {
		add_reply_to_list(client, status, our_csid, replybuf, replylen);
	} else {
		DEBUGLOG
		    ("Local command took too long, discarding xid %d, current is %d\n",
		     xid, client->xid);
	}

	free(replybuf);
	return status;
}

static int process_reply(const struct clvm_header *msg, int msglen, const char *csid)
{
	struct local_client *client = NULL;

	client = find_client(msg->clientid);
	if (!client) {
		DEBUGLOG("Got message for unknown client 0x%x\n",
			 msg->clientid);
		log_error("Got message for unknown client 0x%x\n",
			  msg->clientid);
		return -1;
	}

	if (msg->status)
		client->bits.localsock.all_success = 0;

	/* Gather replies together for this client id */
	if (msg->xid == client->xid) {
		add_reply_to_list(client, msg->status, csid, msg->args,
				  msg->arglen);
	} else {
		DEBUGLOG("Discarding reply with old XID %d, current = %d\n",
			 msg->xid, client->xid);
	}
	return 0;
}

/* Send an aggregated reply back to the client */
static void send_local_reply(struct local_client *client, int status, int fd)
{
	struct clvm_header *clientreply;
	struct node_reply *thisreply = client->bits.localsock.replies;
	char *replybuf;
	char *ptr;
	int message_len = 0;

	DEBUGLOG("Send local reply\n");

	/* Work out the total size of the reply */
	while (thisreply) {
		if (thisreply->replymsg)
			message_len += strlen(thisreply->replymsg) + 1;
		else
			message_len++;

		message_len += strlen(thisreply->node) + 1 + sizeof(int);

		thisreply = thisreply->next;
	}

	/* Add in the size of our header */
	message_len = message_len + sizeof(struct clvm_header);
	if (!(replybuf = malloc(message_len))) {
		DEBUGLOG("Memory allocation fails\n");
		return;
	}

	clientreply = (struct clvm_header *) replybuf;
	clientreply->status = status;
	clientreply->cmd = CLVMD_CMD_REPLY;
	clientreply->node[0] = '\0';
	clientreply->xid = 0;
	clientreply->clientid = 0;
	clientreply->flags = 0;

	ptr = clientreply->args;

	/* Add in all the replies, and free them as we go */
	thisreply = client->bits.localsock.replies;
	while (thisreply) {
		struct node_reply *tempreply = thisreply;

		strcpy(ptr, thisreply->node);
		ptr += strlen(thisreply->node) + 1;

		if (thisreply->status)
			clientreply->flags |= CLVMD_FLAG_NODEERRS;

		memcpy(ptr, &thisreply->status, sizeof(int));
		ptr += sizeof(int);

		if (thisreply->replymsg) {
			strcpy(ptr, thisreply->replymsg);
			ptr += strlen(thisreply->replymsg) + 1;
		} else {
			ptr[0] = '\0';
			ptr++;
		}
		thisreply = thisreply->next;

		free(tempreply->replymsg);
		free(tempreply);
	}

	/* Terminate with an empty node name */
	*ptr = '\0';

	clientreply->arglen = ptr - clientreply->args;

	/* And send it */
	send_message(replybuf, message_len, our_csid, fd,
		     "Error sending REPLY to client");
	free(replybuf);

	/* Reset comms variables */
	client->bits.localsock.replies = NULL;
	client->bits.localsock.expected_replies = 0;
	client->bits.localsock.in_progress = FALSE;
	client->bits.localsock.sent_out = FALSE;
}

/* Just free a reply chain baceuse it wasn't used. */
static void free_reply(struct local_client *client)
{
	/* Add in all the replies, and free them as we go */
	struct node_reply *thisreply = client->bits.localsock.replies;
	while (thisreply) {
		struct node_reply *tempreply = thisreply;

		thisreply = thisreply->next;

		free(tempreply->replymsg);
		free(tempreply);
	}
	client->bits.localsock.replies = NULL;
}

/* Send our version number to the cluster */
static void send_version_message(void)
{
	char message[sizeof(struct clvm_header) + sizeof(int) * 3];
	struct clvm_header *msg = (struct clvm_header *) message;
	int version_nums[3];

	msg->cmd = CLVMD_CMD_VERSION;
	msg->status = 0;
	msg->flags = 0;
	msg->clientid = 0;
	msg->arglen = sizeof(version_nums);

	version_nums[0] = htonl(CLVMD_MAJOR_VERSION);
	version_nums[1] = htonl(CLVMD_MINOR_VERSION);
	version_nums[2] = htonl(CLVMD_PATCH_VERSION);

	memcpy(&msg->args, version_nums, sizeof(version_nums));

	hton_clvm(msg);

	clops->cluster_send_message(message, sizeof(message), NULL,
			     "Error Sending version number");
}

/* Send a message to either a local client or another server */
static int send_message(void *buf, int msglen, const char *csid, int fd,
			const char *errtext)
{
	int len = 0;
	int saved_errno = 0;
	struct timespec delay;
	struct timespec remtime;

	int retry_cnt = 0;

	/* Send remote messages down the cluster socket */
	if (csid == NULL || !ISLOCAL_CSID(csid)) {
		hton_clvm((struct clvm_header *) buf);
		return clops->cluster_send_message(buf, msglen, csid, errtext);
	} else {
		int ptr = 0;

		/* Make sure it all goes */
		do {
			if (retry_cnt > MAX_RETRIES)
			{
				errno = saved_errno;
				log_error("%s", errtext);
				errno = saved_errno;
				break;
			}

			len = write(fd, (char*)buf + ptr, msglen - ptr);

			if (len <= 0) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN ||
				    errno == EIO ||
				    errno == ENOSPC) {
					saved_errno = errno;
					retry_cnt++;

					delay.tv_sec = 0;
					delay.tv_nsec = 100000;
					remtime.tv_sec = 0;
					remtime.tv_nsec = 0;
					(void) nanosleep (&delay, &remtime);

					continue;
				}
				log_error("%s", errtext);
				break;
			}
			ptr += len;
		} while (ptr < msglen);
	}
	return len;
}

static int process_work_item(struct lvm_thread_cmd *cmd)
{
	/* If msg is NULL then this is a cleanup request */
	if (cmd->msg == NULL) {
		DEBUGLOG("process_work_item: free fd %d\n", cmd->client->fd);
		cmd_client_cleanup(cmd->client);
		free(cmd->client);
		return 0;
	}

	if (!cmd->remote) {
		DEBUGLOG("process_work_item: local\n");
		process_local_command(cmd->msg, cmd->msglen, cmd->client,
				      cmd->xid);
	} else {
		DEBUGLOG("process_work_item: remote\n");
		process_remote_command(cmd->msg, cmd->msglen, cmd->client->fd,
				       cmd->csid);
	}
	return 0;
}

/*
 * Routine that runs in the "LVM thread".
 */
static void *lvm_thread_fn(void *arg)
{
	sigset_t ss;
	struct lvm_startup_params *lvm_params = arg;
	struct lvm_thread_cmd *cmd;

	DEBUGLOG("LVM thread function started\n");

	/* Ignore SIGUSR1 & 2 */
	sigemptyset(&ss);
	sigaddset(&ss, SIGUSR1);
	sigaddset(&ss, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &ss, NULL);

	/* Initialise the interface to liblvm */
	init_clvm(lvm_params->excl_uuid);

	/* Allow others to get moving */
	pthread_barrier_wait(&lvm_start_barrier);
	DEBUGLOG("Sub thread ready for work.\n");

	/* Now wait for some actual work */
	pthread_mutex_lock(&lvm_thread_mutex);

	while (!quit) {
		if (dm_list_empty(&lvm_cmd_head)) {
			DEBUGLOG("LVM thread waiting for work\n");
			pthread_cond_wait(&lvm_thread_cond, &lvm_thread_mutex);
		} else {
			cmd = dm_list_item(dm_list_first(&lvm_cmd_head),
					   struct lvm_thread_cmd);
			dm_list_del(&cmd->list);
			pthread_mutex_unlock(&lvm_thread_mutex);

			process_work_item(cmd);
			free(cmd->msg);
			free(cmd);

			pthread_mutex_lock(&lvm_thread_mutex);
		}
	}

	pthread_mutex_unlock(&lvm_thread_mutex);

	pthread_exit(NULL);
}

/* Pass down some work to the LVM thread */
static int add_to_lvmqueue(struct local_client *client, struct clvm_header *msg,
			   int msglen, const char *csid)
{
	struct lvm_thread_cmd *cmd;

	cmd = malloc(sizeof(struct lvm_thread_cmd));
	if (!cmd)
		return ENOMEM;

	if (msglen) {
		cmd->msg = malloc(msglen);
		if (!cmd->msg) {
			log_error("Unable to allocate buffer space\n");
			free(cmd);
			return -1;
		}
		memcpy(cmd->msg, msg, msglen);
	}
	else {
		cmd->msg = NULL;
	}
	cmd->client = client;
	cmd->msglen = msglen;
	cmd->xid = client->xid;

	if (csid) {
		memcpy(cmd->csid, csid, max_csid_len);
		cmd->remote = 1;
	} else {
		cmd->remote = 0;
	}

	DEBUGLOG
	    ("add_to_lvmqueue: cmd=%p. client=%p, msg=%p, len=%d, csid=%p, xid=%d\n",
	     cmd, client, msg, msglen, csid, cmd->xid);
	pthread_mutex_lock(&lvm_thread_mutex);
	dm_list_add(&lvm_cmd_head, &cmd->list);
	pthread_cond_signal(&lvm_thread_cond);
	pthread_mutex_unlock(&lvm_thread_mutex);

	return 0;
}

/* Return 0 if we can talk to an existing clvmd */
static int check_local_clvmd(void)
{
	int local_socket;
	int ret = 0;
	struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };

	if (!dm_strncpy(sockaddr.sun_path, CLVMD_SOCKNAME, sizeof(sockaddr.sun_path))) {
		log_error("%s: clvmd socket name too long.", CLVMD_SOCKNAME);
		return -1;
	}

	/* Open local socket */
	if ((local_socket = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
		log_sys_error("socket", "local socket");
		return -1;
	}

	if (connect(local_socket,(struct sockaddr *) &sockaddr,
		    sizeof(sockaddr))) {
		log_sys_error("connect", "local socket");
		ret = -1;
	}

	if (close(local_socket))
		log_sys_error("close", "local socket");

	return ret;
}

static void close_local_sock(int local_socket)
{
	if (local_socket != -1 && close(local_socket))
		log_sys_error("close", CLVMD_SOCKNAME);

	if (CLVMD_SOCKNAME[0] != '\0' && unlink(CLVMD_SOCKNAME))
		stack;
}

/* Open the local socket, that's the one we talk to libclvm down */
static int open_local_sock(void)
{
	mode_t old_mask;
	int local_socket = -1;
	struct sockaddr_un sockaddr = { .sun_family = AF_UNIX };

	if (!dm_strncpy(sockaddr.sun_path, CLVMD_SOCKNAME, sizeof(sockaddr.sun_path))) {
		log_error("%s: clvmd socket name too long.", CLVMD_SOCKNAME);
		return -1;
	}

	close_local_sock(local_socket);

	(void) dm_prepare_selinux_context(CLVMD_SOCKNAME, S_IFSOCK);
	old_mask = umask(0077);

	/* Open local socket */
	local_socket = socket(PF_UNIX, SOCK_STREAM, 0);
	if (local_socket < 0) {
		log_error("Can't create local socket: %m");
		goto error;
	}

	/* Set Close-on-exec & non-blocking */
	if (fcntl(local_socket, F_SETFD, 1))
		DEBUGLOG("setting CLOEXEC on local_socket failed: %s\n", strerror(errno));
	if (fcntl(local_socket, F_SETFL, fcntl(local_socket, F_GETFL, 0) | O_NONBLOCK))
		DEBUGLOG("setting O_NONBLOCK on local_socket failed: %s\n", strerror(errno));


	if (bind(local_socket, (struct sockaddr *) &sockaddr, sizeof(sockaddr))) {
		log_error("can't bind local socket: %m");
		goto error;
	}
	if (listen(local_socket, 1) != 0) {
		log_error("listen local: %m");
		goto error;
	}

	umask(old_mask);
	(void) dm_prepare_selinux_context(NULL, 0);
	return local_socket;
error:
	close_local_sock(local_socket);
	umask(old_mask);
	(void) dm_prepare_selinux_context(NULL, 0);
	return -1;
}

void process_message(struct local_client *client, char *buf, int len,
		     const char *csid)
{
	char nodename[max_cluster_member_name_len];
	struct clvm_header *inheader;
	int rv;

	inheader = (struct clvm_header *) buf;
	ntoh_clvm(inheader);	/* Byteswap fields */

	rv = verify_message(buf, len);
	if (rv < 0) {
		memset(nodename, 0, sizeof(nodename));
		clops->name_from_csid(csid, nodename);
		log_error("process_message from %s len %d bad verify", nodename, len);
		dump_message(buf, len);
		return;
	}

	if (inheader->cmd == CLVMD_CMD_REPLY)
		process_reply(inheader, len, csid);
	else
		add_to_lvmqueue(client, inheader, len, csid);
}


static void check_all_callback(struct local_client *client, const char *csid,
			       int node_up)
{
	if (!node_up)
		add_reply_to_list(client, EHOSTDOWN, csid, "CLVMD not running",
				  18);
}

/* Check to see if all CLVMDs are running (ie one on
   every node in the cluster).
   If not, returns -1 and prints out a list of errant nodes */
static int check_all_clvmds_running(struct local_client *client)
{
	DEBUGLOG("check_all_clvmds_running\n");
	return clops->cluster_do_node_callback(client, check_all_callback);
}

/* Return a local_client struct given a client ID.
   client IDs are in network byte order */
static struct local_client *find_client(int clientid)
{
	struct local_client *thisfd;
	for (thisfd = &local_client_head; thisfd != NULL; thisfd = thisfd->next) {
		if (thisfd->fd == (int)ntohl(clientid))
			return thisfd;
	}
	return NULL;
}

/* Byte-swapping routines for the header so we
   work in a heterogeneous environment */
static void hton_clvm(struct clvm_header *hdr)
{
	hdr->status = htonl(hdr->status);
	hdr->arglen = htonl(hdr->arglen);
	hdr->xid = htons(hdr->xid);
	/* Don't swap clientid as it's only a token as far as
	   remote nodes are concerned */
}

static void ntoh_clvm(struct clvm_header *hdr)
{
	hdr->status = ntohl(hdr->status);
	hdr->arglen = ntohl(hdr->arglen);
	hdr->xid = ntohs(hdr->xid);
}

/* Handler for SIGUSR2 - sent to kill subthreads */
static void sigusr2_handler(int sig)
{
	DEBUGLOG("SIGUSR2 received\n");
	return;
}

static void sigterm_handler(int sig)
{
	quit = 1;
	return;
}

static void sighup_handler(int sig)
{
	reread_config = 1;
}

int sync_lock(const char *resource, int mode, int flags, int *lockid)
{
	return clops->sync_lock(resource, mode, flags, lockid);
}

int sync_unlock(const char *resource, int lockid)
{
	return clops->sync_unlock(resource, lockid);
}

static if_type_t parse_cluster_interface(char *ifname)
{
	if_type_t iface = IF_AUTO;

	if (!strcmp(ifname, "auto"))
		iface = IF_AUTO;
	if (!strcmp(ifname, "cman"))
		iface = IF_CMAN;
	if (!strcmp(ifname, "openais"))
		iface = IF_OPENAIS;
	if (!strcmp(ifname, "corosync"))
		iface = IF_COROSYNC;
	if (!strcmp(ifname, "singlenode"))
		iface = IF_SINGLENODE;

	return iface;
}

/*
 * Try and find a cluster system in corosync's objdb, if it is running. This is
 * only called if the command-line option is not present, and if it fails
 * we still try the interfaces in order.
 */
static if_type_t get_cluster_type(void)
{
#ifdef HAVE_COROSYNC_CONFDB_H
	confdb_handle_t handle;
	if_type_t type = IF_AUTO;
	int result;
	char buf[255];
	size_t namelen = sizeof(buf);
	hdb_handle_t cluster_handle;
	hdb_handle_t clvmd_handle;
	confdb_callbacks_t callbacks = {
		.confdb_key_change_notify_fn = NULL,
		.confdb_object_create_change_notify_fn = NULL,
		.confdb_object_delete_change_notify_fn = NULL
	};

	result = confdb_initialize (&handle, &callbacks);
        if (result != CS_OK)
		return type;

        result = confdb_object_find_start(handle, OBJECT_PARENT_HANDLE);
	if (result != CS_OK)
		goto out;

        result = confdb_object_find(handle, OBJECT_PARENT_HANDLE, (void *)"cluster", strlen("cluster"), &cluster_handle);
        if (result != CS_OK)
		goto out;

        result = confdb_object_find_start(handle, cluster_handle);
	if (result != CS_OK)
		goto out;

        result = confdb_object_find(handle, cluster_handle, (void *)"clvmd", strlen("clvmd"), &clvmd_handle);
        if (result != CS_OK)
		goto out;

        result = confdb_key_get(handle, clvmd_handle, (void *)"interface", strlen("interface"), buf, &namelen);
        if (result != CS_OK)
		goto out;

	if (namelen >= sizeof(buf))
		namelen = sizeof(buf) - 1;

	buf[namelen] = '\0';
	type = parse_cluster_interface(buf);
	DEBUGLOG("got interface type '%s' from confdb\n", buf);
out:
	confdb_finalize(handle);
	return type;
#else
	return IF_AUTO;
#endif
}
