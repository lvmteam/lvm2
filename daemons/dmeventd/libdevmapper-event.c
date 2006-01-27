 /*
 * Copyright (C) 2005 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
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
#include "libdevmapper-event.h"
//#include "libmultilog.h"
#include "dmeventd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

/* Set by any of the external fxns the first time one of them is called */
/* FIXME Unused */
// static int _logging = 0;

/* Fetch a string off src and duplicate it into *dest. */
/* FIXME: move to seperate module to share with the daemon. */
static const char delimiter = ' ';
static char *fetch_string(char **src)
{
	char *p, *ret;

	if ((p = strchr(*src, delimiter)))
		*p = 0;

	if ((ret = strdup(*src)))
		*src += strlen(ret) + 1;

	if (p)
		*p = delimiter;

	return ret;
}

/* Parse a device message from the daemon. */
static int parse_message(struct dm_event_daemon_message *msg, char **dso_name,
			 char **device, enum dm_event_type *events)
{
	char *p = msg->msg;

	if ((*dso_name = fetch_string(&p)) &&
	    (*device   = fetch_string(&p))) {
		*events = atoi(p);

		return 0;
	}

	return -ENOMEM;
}

/*
 * daemon_read
 * @fifos
 * @msg
 *
 * Read message from daemon.
 *
 * Returns: 0 on failure, 1 on success
 */
static int daemon_read(struct dm_event_fifos *fifos, struct dm_event_daemon_message *msg)
{
	int bytes = 0, ret = 0;
	fd_set fds;

	memset(msg, 0, sizeof(*msg));
	while (bytes < sizeof(*msg)) {
		do {
			/* Watch daemon read FIFO for input. */
			FD_ZERO(&fds);
			FD_SET(fifos->server, &fds);
			ret = select(fifos->server+1, &fds, NULL, NULL, NULL);
			if (ret < 0 && errno != EINTR) {
				/* FIXME Log error */
				return 0;
			}
		} while (ret < 1);

		ret = read(fifos->server, msg, sizeof(*msg) - bytes);
		if (ret < 0) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			else {
				/* FIXME Log error */
				return 0;
			}
		}

		bytes += ret;
	}

	return bytes == sizeof(*msg);
}

/* Write message to daemon. */
static int daemon_write(struct dm_event_fifos *fifos, struct dm_event_daemon_message *msg)
{
	int bytes = 0, ret = 0;
	fd_set fds;

	while (bytes < sizeof(*msg)) {
		do {
			/* Watch daemon write FIFO to be ready for output. */
			FD_ZERO(&fds);
			FD_SET(fifos->client, &fds);
			ret = select(fifos->client +1, NULL, &fds, NULL, NULL);
			if ((ret < 0) && (errno != EINTR)) {
				/* FIXME Log error */
				return 0;
			}
		} while (ret < 1);

		ret = write(fifos->client, msg, sizeof(*msg) - bytes);
		if (ret < 0) {
			if ((errno == EINTR) || (errno == EAGAIN))
				continue;
			else {
				/* fixme: log error */
				return 0;
			}
		}

		bytes += ret;
	}

	return bytes == sizeof(*msg);
}

static int daemon_talk(struct dm_event_fifos *fifos, struct dm_event_daemon_message *msg,
		       int cmd, char *dso_name, char *device,
		       enum dm_event_type events, uint32_t timeout)
{
	memset(msg, 0, sizeof(*msg));

	/*
	 * Set command and pack the arguments
	 * into ASCII message string.
	 */
	msg->opcode.cmd = cmd;

	if (sizeof(msg->msg) <= snprintf(msg->msg, sizeof(msg->msg),
					 "%s %s %u %"PRIu32,
					 dso_name ? dso_name : "",
					 device ? device : "",
					 events, timeout)) {
		stack;
		return -ENAMETOOLONG;
	}

	/*
	 * Write command and message to and
	 * read status return code from daemon.
	 */
	if (!daemon_write(fifos, msg)) {
		stack;
		return -EIO;
	}

	if (!daemon_read(fifos, msg)) {
		stack;
		return -EIO;
	}

	return msg->opcode.status;
}

static volatile sig_atomic_t daemon_running = 0;

static void daemon_running_signal_handler(int sig)
{
	daemon_running = 1;
}

/*
 * start_daemon
 *
 * This function forks off a process (dmeventd) that will handle
 * the events.  A signal must be returned from the child to
 * indicate when it is ready to handle requests.  The parent
 * (this function) returns 1 if there is a daemon running. 
 *
 * Returns: 1 on success, 0 otherwise
 */
static int start_daemon(void)
{
	int pid, ret=0;
	void *old_hand;
	sigset_t set, oset;

	/* Must be able to acquire signal */
	old_hand = signal(SIGUSR1, &daemon_running_signal_handler);
	if (old_hand == SIG_ERR) {
		log_error("Unable to setup signal handler.");
		return 0;
	}

	if (sigemptyset(&set) || sigaddset(&set, SIGUSR1)) {
		log_error("Unable to fill signal set.");
	} else if (sigprocmask(SIG_UNBLOCK, &set, &oset)) {
		log_error("Can't unblock the potentially blocked signal SIGUSR1");
	}
	
	pid = fork();

	if (pid < 0)
		log_error("Unable to fork.\n");
	else if (pid) { /* parent waits for child to get ready for requests */
		int status;

		/* FIXME Better way to do this? */
		while (!waitpid(pid, &status, WNOHANG) && !daemon_running)
			sleep(1);

		if (daemon_running) {
			ret = 1;
		} else {
			switch (WEXITSTATUS(status)) {
			case EXIT_LOCKFILE_INUSE:
				/*
				 * Note, this is ok... we still have daemon
				 * that we can communicate with...
				 */
				log_print("Starting dmeventd failed: "
					  "dmeventd already running.\n");
				ret = 1;
				break;
			default:
				log_error("Unable to start dmeventd.\n");
				break;
			}
		}
		/*
		 * Sometimes, a single process may perform multiple calls
		 * that result in a daemon starting and exiting.  If we
		 * don't reset this, the second (or greater) time the daemon
		 * is started will cause this logic not to work.
		 */
		daemon_running = 0;
	} else {
		signal(SIGUSR1, SIG_IGN); /* don't care about error */

		/* dmeventd function is responsible for properly setting **
		** itself up.  It must never return - only exit.  This is**
		** why it is followed by an EXIT_FAILURE                 */
		dmeventd();
		exit(EXIT_FAILURE);
	}

	/* FIXME What if old_hand is SIG_ERR? */
	if (signal(SIGUSR1, old_hand) == SIG_ERR)
		log_error("Unable to reset signal handler.");

	if (sigprocmask(SIG_SETMASK, &oset, NULL))
		log_error("Unable to reset signal mask.");

	return ret;
}

/* Initialize client. */
static int init_client(struct dm_event_fifos *fifos)
{
	/* FIXME Is fifo the most suitable method? */
	/* FIXME Why not share comms/daemon code with something else e.g. multipath? */

	/* init fifos */
	memset(fifos, 0, sizeof(*fifos));
	fifos->client_path = DM_EVENT_FIFO_CLIENT;
	fifos->server_path = DM_EVENT_FIFO_SERVER;

	/* FIXME The server should be responsible for these, not the client. */
	/* Create fifos */
	if (((mkfifo(fifos->client_path, 0600) == -1) && errno != EEXIST) ||
	    ((mkfifo(fifos->server_path, 0600) == -1) && errno != EEXIST)) {
		log_error("%s: Failed to create a fifo.\n", __func__);
		return 0;
	}

	/* FIXME Warn/abort if perms are wrong - not something to fix silently. */
	/* If they were already there, make sure permissions are ok. */
	if (chmod(fifos->client_path, 0600)) {
		log_error("Unable to set correct file permissions on %s",
			fifos->client_path);
		return 0;
	}

	if (chmod(fifos->server_path, 0600)) {
		log_error("Unable to set correct file permissions on %s",
			fifos->server_path);
		return 0;
	}

	/*
	 * Open the fifo used to read from the daemon.
	 * Allows daemon to create its write fifo...
	 */
	if ((fifos->server = open(fifos->server_path, O_RDWR)) < 0) {
		log_error("%s: open server fifo %s\n",
			__func__, fifos->server_path);
		stack;
		return 0;
	}

	/* Lock out anyone else trying to do communication with the daemon. */
	/* FIXME Why failure not retry?  How do multiple processes communicate? */
	if (flock(fifos->server, LOCK_EX) < 0){
		log_error("%s: flock %s\n", __func__, fifos->server_path);
		close(fifos->server);
		return 0;
	}

	/* Anyone listening?  If not, errno will be ENXIO */
	while ((fifos->client = open(fifos->client_path,
				  O_WRONLY | O_NONBLOCK)) < 0) {
		if (errno != ENXIO) {
			log_error("%s: Can't open client fifo %s: %s\n",
				  __func__, fifos->client_path, strerror(errno));
			close(fifos->server);
			stack;
			return 0;
		}
		
		/* FIXME Unnecessary if daemon was started before calling this */
		if (!start_daemon()) {
			stack;
			return 0;
		}
	}
	
	return 1;
}

static void dtr_client(struct dm_event_fifos *fifos)
{
	if (flock(fifos->server, LOCK_UN))
		log_error("flock unlock %s\n", fifos->server_path);

	close(fifos->client);
	close(fifos->server);
}

/* Check, if a block device exists. */
static int device_exists(char *device)
{
	struct stat st_buf;
	char path2[PATH_MAX];

	if (!device)
		return 0;

	if (device[0] == '/') /* absolute path */
		return !stat(device, &st_buf) && S_ISBLK(st_buf.st_mode);

	if (PATH_MAX <= snprintf(path2, PATH_MAX, "%s/%s", dm_dir(), device))
		return 0;

	return !stat(path2, &st_buf) && S_ISBLK(st_buf.st_mode);
}

/* Handle the event (de)registration call and return negative error codes. */
static int do_event(int cmd, struct dm_event_daemon_message *msg,
		    char *dso_name, char *device, enum dm_event_type events,
		    uint32_t timeout)
{
	int ret;
	struct dm_event_fifos fifos;

	/* FIXME Start the daemon here if it's not running e.g. exclusive lock file */
	/* FIXME Move this to separate 'dm_event_register_handler' - if no daemon here, fail */
	if (!init_client(&fifos)) {
		stack;
		return -ESRCH;
	}

	/* FIXME Use separate 'dm_event_register_handler' function to pass in dso? */
	ret = daemon_talk(&fifos, msg, cmd, dso_name, device, events, timeout);

	/* what is the opposite of init? */
	dtr_client(&fifos);
	
	return ret;
}

/* FIXME remove dso_name - use handle instead */
/* FIXME Use uuid not path! */
/* External library interface. */
int dm_event_register(char *dso_name, char *device_path,
		      enum dm_event_type events)
{
	int ret;
	struct dm_event_daemon_message msg;

	if (!device_exists(device_path)) {
		log_error("%s: device not found");
		return 0;
	}

	if ((ret = do_event(DM_EVENT_CMD_REGISTER_FOR_EVENT, &msg,
			    dso_name, device_path, events, 0)) < 0) {
		log_error("%s: event registration failed: %s", device_path,
			  strerror(-ret));
		return 0;
	}

	return 1;
}

int dm_event_unregister(char *dso_name, char *device_path,
			enum dm_event_type events)
{
	int ret;
	struct dm_event_daemon_message msg;

	if (!device_exists(device_path)) {
		log_error("%s: device not found");
		return 0;
	}

	if ((ret = do_event(DM_EVENT_CMD_UNREGISTER_FOR_EVENT, &msg,
			    dso_name, device_path, events, 0)) < 0) {
		log_error("%s: event deregistration failed: %s", device_path,
			  strerror(-ret));
		return 0;
	}

	return 1;
}

int dm_event_get_registered_device(char **dso_name, char **device_path,
			     enum dm_event_type *events, int next)
{
	int ret;
	char *dso_name_arg = NULL, *device_path_arg = NULL;
	struct dm_event_daemon_message msg;

	if (!(ret = do_event(next ? DM_EVENT_CMD_GET_NEXT_REGISTERED_DEVICE :
				    DM_EVENT_CMD_GET_REGISTERED_DEVICE,
			     &msg, *dso_name, *device_path, *events, 0)))
		ret = parse_message(&msg, &dso_name_arg, &device_path_arg,
				    events);

	if (next){
		if (*dso_name)
			free(*dso_name);
		if (*device_path)
			free(*device_path);
		*dso_name = dso_name_arg;
		*device_path = device_path_arg;
	} else {
		if (!(*dso_name))
			*dso_name = dso_name_arg;
		if (!(*device_path))
			*device_path = device_path_arg;
	}

	return ret;
}

int dm_event_set_timeout(char *device_path, uint32_t timeout)
{
	struct dm_event_daemon_message msg;

	if (!device_exists(device_path))
		return -ENODEV;
	return do_event(DM_EVENT_CMD_SET_TIMEOUT, &msg,
			NULL, device_path, 0, timeout);
}

int dm_event_get_timeout(char *device_path, uint32_t *timeout)
{
	int ret;
	struct dm_event_daemon_message msg;

	if (!device_exists(device_path))
		return -ENODEV;
	if (!(ret = do_event(DM_EVENT_CMD_GET_TIMEOUT, &msg, NULL, device_path, 0, 0)))
		*timeout = atoi(msg.msg);
	return ret;
}
/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
