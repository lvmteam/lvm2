/*
 * Copyright (C) 2005-2007 Red Hat, Inc. All rights reserved.
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

/*
 * dmeventd - dm event daemon to monitor active mapped devices
 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "configure.h"
#include "libdevmapper.h"
#include "libdevmapper-event.h"
#include "dmeventd.h"
//#include "libmultilog.h"
#include "dm-logging.h"

#include <stdarg.h>
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
#include <arpa/inet.h>		/* for htonl, ntohl */
#include <fcntl.h>		/* for musl libc */

#ifdef __linux__
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
#  define SD_RUNTIME_UNIT_FILE_DIR DEFAULT_DM_RUN_DIR "/systemd/system/"
#  define SD_ACTIVATION_ENV_VAR_NAME "SD_ACTIVATION"
#  define SD_LISTEN_PID_ENV_VAR_NAME "LISTEN_PID"
#  define SD_LISTEN_FDS_ENV_VAR_NAME "LISTEN_FDS"
#  define SD_LISTEN_FDS_START 3
#  define SD_FD_FIFO_SERVER SD_LISTEN_FDS_START
#  define SD_FD_FIFO_CLIENT (SD_LISTEN_FDS_START + 1)

#endif

/* FIXME We use syslog for now, because multilog is not yet implemented */
#include <syslog.h>

static volatile sig_atomic_t _exit_now = 0;	/* set to '1' when signal is given to exit */

/* List (un)link macros. */
#define	LINK(x, head)		dm_list_add(head, &(x)->list)
#define	LINK_DSO(dso)		LINK(dso, &_dso_registry)
#define	LINK_THREAD(thread)	LINK(thread, &_thread_registry)

#define	UNLINK(x)		dm_list_del(&(x)->list)
#define	UNLINK_DSO(x)		UNLINK(x)
#define	UNLINK_THREAD(x)	UNLINK(x)

#define DAEMON_NAME "dmeventd"

/*
  Global mutex for thread list access. Has to be held when:
  - iterating thread list
  - adding or removing elements from thread list
  - changing or reading thread_status's fields:
    processing, status, events
  Use _lock_mutex() and _unlock_mutex() to hold/release it
*/
static pthread_mutex_t _global_mutex;

/*
  There are three states a thread can attain (see struct
  thread_status, field int status):

  - DM_THREAD_RUNNING: thread has started up and is either working or
  waiting for events... transitions to either SHUTDOWN or DONE
  - DM_THREAD_SHUTDOWN: thread is still doing something, but it is
  supposed to terminate (and transition to DONE) as soon as it
  finishes whatever it was doing at the point of flipping state to
  SHUTDOWN... the thread is still on the thread list
  - DM_THREAD_DONE: thread has terminated and has been moved over to
  unused thread list, cleanup pending
 */
#define DM_THREAD_RUNNING  0
#define DM_THREAD_SHUTDOWN 1
#define DM_THREAD_DONE     2

#define THREAD_STACK_SIZE (300*1024)

int dmeventd_debug = 0;
static int _systemd_activation = 0;
static int _foreground = 0;
static int _restart = 0;
static char **_initial_registrations = 0;

/* FIXME Make configurable at runtime */
#ifdef DEBUG
#  define DEBUGLOG(fmt, args...) debuglog("[Thr %x]: " fmt, (int)pthread_self(), ## args)
void debuglog(const char *fmt, ... ) __attribute__ ((format(printf, 1, 2)));

void debuglog(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(LOG_DEBUG, fmt, ap);
	va_end(ap);
}

static const char *decode_cmd(uint32_t cmd)
{
	switch (cmd) {
	case DM_EVENT_CMD_ACTIVE:			return "ACTIVE";
	case DM_EVENT_CMD_REGISTER_FOR_EVENT:		return "REGISTER_FOR_EVENT";
	case DM_EVENT_CMD_UNREGISTER_FOR_EVENT:		return "UNREGISTER_FOR_EVENT";
	case DM_EVENT_CMD_GET_REGISTERED_DEVICE:	return "GET_REGISTERED_DEVICE";
	case DM_EVENT_CMD_GET_NEXT_REGISTERED_DEVICE:	return "GET_NEXT_REGISTERED_DEVICE";
	case DM_EVENT_CMD_SET_TIMEOUT:			return "SET_TIMEOUT";
	case DM_EVENT_CMD_GET_TIMEOUT:			return "GET_TIMEOUT";
	case DM_EVENT_CMD_HELLO:			return "HELLO";
	case DM_EVENT_CMD_DIE:				return "DIE";
	case DM_EVENT_CMD_GET_STATUS:			return "GET_STATUS";
	case DM_EVENT_CMD_GET_PARAMETERS:		return "GET_PARAMETERS";
	default:					return "unknown";
	}
}

#else
#  define DEBUGLOG(fmt, args...) do { } while (0)
#endif

/* Data kept about a DSO. */
struct dso_data {
	struct dm_list list;

	char *dso_name;		/* DSO name (eg, "evms", "dmraid", "lvm2"). */

	void *dso_handle;	/* Opaque handle as returned from dlopen(). */
	unsigned int ref_count;	/* Library reference count. */

	/*
	 * Event processing.
	 *
	 * The DSO can do whatever appropriate steps if an event
	 * happens such as changing the mapping in case a mirror
	 * fails, update the application metadata etc.
	 *
	 * This function gets a dm_task that is a result of
	 * DM_DEVICE_WAITEVENT ioctl (results equivalent to
	 * DM_DEVICE_STATUS). It should not destroy it.
	 * The caller must dispose of the task.
	 */
	void (*process_event)(struct dm_task *dmt, enum dm_event_mask event, void **user);

	/*
	 * Device registration.
	 *
	 * When an application registers a device for an event, the DSO
	 * can carry out appropriate steps so that a later call to
	 * the process_event() function is sane (eg, read metadata
	 * and activate a mapping).
	 */
	int (*register_device)(const char *device, const char *uuid, int major,
			       int minor, void **user);

	/*
	 * Device unregistration.
	 *
	 * In case all devices of a mapping (eg, RAID10) are unregistered
	 * for events, the DSO can recognize this and carry out appropriate
	 * steps (eg, deactivate mapping, metadata update).
	 */
	int (*unregister_device)(const char *device, const char *uuid,
				 int major, int minor, void **user);
};
static DM_LIST_INIT(_dso_registry);

/* Structure to keep parsed register variables from client message. */
struct message_data {
	char *id;
	char *dso_name;		/* Name of DSO. */
	char *device_uuid;	/* Mapped device path. */
	char *events_str;	/* Events string as fetched from message. */
	enum dm_event_mask events_field;	/* Events bitfield. */
	char *timeout_str;
	uint32_t timeout_secs;
	struct dm_event_daemon_message *msg;	/* Pointer to message buffer. */
};

/*
 * Housekeeping of thread+device states.
 *
 * One thread per mapped device which can block on it until an event
 * occurs and the event processing function of the DSO gets called.
 */
struct thread_status {
	struct dm_list list;

	pthread_t thread;

	struct dso_data *dso_data;	/* DSO this thread accesses. */

	struct {
		char *uuid;
		char *name;
		int major, minor;
	} device;
	uint32_t event_nr;	/* event number */
	int processing;		/* Set when event is being processed */

	int status;		/* see DM_THREAD_{RUNNING,SHUTDOWN,DONE}
				   constants above */
	enum dm_event_mask events;	/* bitfield for event filter. */
	enum dm_event_mask current_events;	/* bitfield for occured events. */
	struct dm_task *current_task;
	time_t next_time;
	uint32_t timeout;
	struct dm_list timeout_list;
	void *dso_private; /* dso per-thread status variable */
};
static DM_LIST_INIT(_thread_registry);
static DM_LIST_INIT(_thread_registry_unused);

static int _timeout_running;
static DM_LIST_INIT(_timeout_registry);
static pthread_mutex_t _timeout_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t _timeout_cond = PTHREAD_COND_INITIALIZER;

/* Allocate/free the status structure for a monitoring thread. */
static struct thread_status *_alloc_thread_status(const struct message_data *data,
						  struct dso_data *dso_data)
{
	struct thread_status *ret;

	if (!(ret = dm_zalloc(sizeof(*ret))))
		return NULL;

	if (!(ret->device.uuid = dm_strdup(data->device_uuid))) {
		dm_free(ret);
		return NULL;
	}

	ret->dso_data = dso_data;
	ret->events = data->events_field;
	ret->timeout = data->timeout_secs;
	dm_list_init(&ret->timeout_list);

	return ret;
}

static void _lib_put(struct dso_data *data);
static void _free_thread_status(struct thread_status *thread)
{
	_lib_put(thread->dso_data);
	if (thread->current_task)
		dm_task_destroy(thread->current_task);
	dm_free(thread->device.uuid);
	dm_free(thread->device.name);
	dm_free(thread);
}

/* Allocate/free DSO data. */
static struct dso_data *_alloc_dso_data(struct message_data *data)
{
	struct dso_data *ret = (typeof(ret)) dm_zalloc(sizeof(*ret));

	if (!ret)
		return NULL;

	if (!(ret->dso_name = dm_strdup(data->dso_name))) {
		dm_free(ret);
		return NULL;
	}

	return ret;
}

/*
 * Create a device monitoring thread.
 * N.B.  Error codes returned are positive.
 */
static int _pthread_create_smallstack(pthread_t *t, void *(*fun)(void *), void *arg)
{
	int r;
	pthread_t tmp;
	pthread_attr_t attr;

	/*
	 * From pthread_attr_init man page:
	 * POSIX.1-2001 documents an ENOMEM error for pthread_attr_init(); on
	 * Linux these functions always succeed (but portable and future-proof
	 * applications should nevertheless handle a possible error return).
	 */
	if ((r = pthread_attr_init(&attr)) != 0)
		return r;

	/*
	 * We use a smaller stack since it gets preallocated in its entirety
	 */
	pthread_attr_setstacksize(&attr, THREAD_STACK_SIZE);

	/*
	 * If no-one will be waiting, we need to detach.
	 */
	if (!t) {
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		t = &tmp;
	}

	r = pthread_create(t, &attr, fun, arg);

	pthread_attr_destroy(&attr);

	return r;
}

static void _free_dso_data(struct dso_data *data)
{
	dm_free(data->dso_name);
	dm_free(data);
}

/*
 * Fetch a string off src and duplicate it into *ptr.
 * Pay attention to zero-length strings.
 */
/* FIXME? move to libdevmapper to share with the client lib (need to
   make delimiter a parameter then) */
static int _fetch_string(char **ptr, char **src, const int delimiter)
{
	int ret = 0;
	char *p;
	size_t len;

	if ((p = strchr(*src, delimiter)))
		*p = 0;

	if ((*ptr = dm_strdup(*src))) {
		if ((len = strlen(*ptr)))
			*src += len;
		else {
			dm_free(*ptr);
			*ptr = NULL;
		}

		(*src)++;
		ret = 1;
	}

	if (p)
		*p = delimiter;

	return ret;
}

/* Free message memory. */
static void _free_message(struct message_data *message_data)
{
	dm_free(message_data->id);
	dm_free(message_data->dso_name);
	dm_free(message_data->device_uuid);
	dm_free(message_data->events_str);
	dm_free(message_data->timeout_str);
}

/* Parse a register message from the client. */
static int _parse_message(struct message_data *message_data)
{
	int ret = 0;
	struct dm_event_daemon_message *msg = message_data->msg;
	char *p = msg->data;

	if (!msg->data)
		return 0;

	/*
	 * Retrieve application identifier, mapped device
	 * path and events # string from message.
	 */
	if (_fetch_string(&message_data->id, &p, ' ') &&
	    _fetch_string(&message_data->dso_name, &p, ' ') &&
	    _fetch_string(&message_data->device_uuid, &p, ' ') &&
	    _fetch_string(&message_data->events_str, &p, ' ') &&
	    _fetch_string(&message_data->timeout_str, &p, ' ')) {
		if (message_data->events_str)
			message_data->events_field =
				atoi(message_data->events_str);
		if (message_data->timeout_str)
			message_data->timeout_secs =
				atoi(message_data->timeout_str)
				? : DM_EVENT_DEFAULT_TIMEOUT;
		ret = 1;
	}

	dm_free(msg->data);
	msg->data = NULL;

	return ret;
}

/* Global mutex to lock access to lists et al. See _global_mutex
   above. */
static int _lock_mutex(void)
{
	return pthread_mutex_lock(&_global_mutex);
}

static int _unlock_mutex(void)
{
	return pthread_mutex_unlock(&_global_mutex);
}

/* Check, if a device exists. */
static int _fill_device_data(struct thread_status *ts)
{
	struct dm_task *dmt;
	struct dm_info dmi;
	int ret = 0;

	if (!ts->device.uuid)
		return 0;

	if (!(dmt = dm_task_create(DM_DEVICE_INFO)))
		return 0;

	if (!dm_task_set_uuid(dmt, ts->device.uuid))
		goto fail;

	if (!dm_task_run(dmt))
		goto fail;

	dm_free(ts->device.name);
	if (!(ts->device.name = dm_strdup(dm_task_get_name(dmt))))
		goto fail;

	if (!dm_task_get_info(dmt, &dmi))
		goto fail;

	ts->device.major = dmi.major;
	ts->device.minor = dmi.minor;
	ret = 1;
fail:
	dm_task_destroy(dmt);

	return ret;
}

/*
 * Find an existing thread for a device.
 *
 * Mutex must be held when calling this.
 */
static struct thread_status *_lookup_thread_status(struct message_data *data)
{
	struct thread_status *thread;

	dm_list_iterate_items(thread, &_thread_registry)
		if (!strcmp(data->device_uuid, thread->device.uuid))
			return thread;

	return NULL;
}

static int _get_status(struct message_data *message_data)
{
	struct dm_event_daemon_message *msg = message_data->msg;
	struct thread_status *thread;
	int i = 0, j;
	int ret = -ENOMEM;
	int count;
	int size = 0, current;
	size_t len;
	char **buffers;
	char *message;

	_lock_mutex();
	count = dm_list_size(&_thread_registry);
	buffers = alloca(sizeof(char*) * count);
	dm_list_iterate_items(thread, &_thread_registry) {
		if ((current = dm_asprintf(buffers + i, "0:%d %s %s %u %" PRIu32 ";",
					   i, thread->dso_data->dso_name,
					   thread->device.uuid, thread->events,
					   thread->timeout)) < 0) {
			_unlock_mutex();
			goto out;
		}
		++i;
		size += current; /* count with trailing '\0' */
	}
	_unlock_mutex();

	len = strlen(message_data->id);
	msg->size = size + len + 1;
	dm_free(msg->data);
	if (!(msg->data = dm_malloc(msg->size)))
		goto out;

	memcpy(msg->data, message_data->id, len);
	message = msg->data + len;
	*message++ = ' ';
	for (j = 0; j < i; ++j) {
		len = strlen(buffers[j]);
		memcpy(message, buffers[j], len);
		message += len;
	}

	ret = 0;
 out:
	for (j = 0; j < i; ++j)
		dm_free(buffers[j]);

	return ret;
}

static int _get_parameters(struct message_data *message_data) {
	struct dm_event_daemon_message *msg = message_data->msg;
	int size;

	dm_free(msg->data);
	if ((size = dm_asprintf(&msg->data, "%s pid=%d daemon=%s exec_method=%s",
				message_data->id, getpid(),
				_foreground ? "no" : "yes",
				_systemd_activation ? "systemd" : "direct")) < 0) {
		stack;
		return -ENOMEM;
	}

	msg->size = (uint32_t) size;

	return 0;
}

/* Cleanup at exit. */
static void _exit_dm_lib(void)
{
	dm_lib_release();
	dm_lib_exit();
}

static void _exit_timeout(void *unused __attribute__((unused)))
{
	_timeout_running = 0;
	pthread_mutex_unlock(&_timeout_mutex);
}

/* Wake up monitor threads every so often. */
static void *_timeout_thread(void *unused __attribute__((unused)))
{
	struct thread_status *thread;
	struct timespec timeout;
	time_t curr_time;

	DEBUGLOG("Timeout thread starting.");
	timeout.tv_nsec = 0;
	pthread_cleanup_push(_exit_timeout, NULL);
	pthread_mutex_lock(&_timeout_mutex);

	while (!dm_list_empty(&_timeout_registry)) {
		timeout.tv_sec = 0;
		curr_time = time(NULL);

		dm_list_iterate_items_gen(thread, &_timeout_registry, timeout_list) {
			if (thread->next_time <= curr_time) {
				thread->next_time = curr_time + thread->timeout;
				DEBUGLOG("Sending SIGALRM to Thr %x for timeout.", (int) thread->thread);
				pthread_kill(thread->thread, SIGALRM);
			}

			if (thread->next_time < timeout.tv_sec || !timeout.tv_sec)
				timeout.tv_sec = thread->next_time;
		}

		pthread_cond_timedwait(&_timeout_cond, &_timeout_mutex,
				       &timeout);
	}

	DEBUGLOG("Timeout thread finished.");
	pthread_cleanup_pop(1);

	return NULL;
}

static int _register_for_timeout(struct thread_status *thread)
{
	int ret = 0;

	pthread_mutex_lock(&_timeout_mutex);

	if (dm_list_empty(&thread->timeout_list)) {
		thread->next_time = time(NULL) + thread->timeout;
		dm_list_add(&_timeout_registry, &thread->timeout_list);
		if (_timeout_running)
			pthread_cond_signal(&_timeout_cond);
	}

	if (!_timeout_running &&
	    !(ret = _pthread_create_smallstack(NULL, _timeout_thread, NULL)))
		_timeout_running = 1;

	pthread_mutex_unlock(&_timeout_mutex);

	return ret;
}

static void _unregister_for_timeout(struct thread_status *thread)
{
	pthread_mutex_lock(&_timeout_mutex);
	if (!dm_list_empty(&thread->timeout_list)) {
		dm_list_del(&thread->timeout_list);
		dm_list_init(&thread->timeout_list);
		if (dm_list_empty(&_timeout_registry))
			/* No more work -> wakeup to finish quickly */
			pthread_cond_signal(&_timeout_cond);
	}
	pthread_mutex_unlock(&_timeout_mutex);
}

__attribute__((format(printf, 4, 5)))
static void _no_intr_log(int level, const char *file, int line,
			const char *f, ...)
{
	va_list ap;

	if (errno == EINTR)
		return;
	if (level > _LOG_WARN)
		return;

	va_start(ap, f);
	vfprintf((level < _LOG_WARN) ? stderr : stdout, f, ap);
	va_end(ap);

	fputc('\n', (level < _LOG_WARN) ? stderr : stdout);
}

static sigset_t _unblock_sigalrm(void)
{
	sigset_t set, old;

	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	pthread_sigmask(SIG_UNBLOCK, &set, &old);
	return old;
}

#define DM_WAIT_RETRY 0
#define DM_WAIT_INTR 1
#define DM_WAIT_FATAL 2

/* Wait on a device until an event occurs. */
static int _event_wait(struct thread_status *thread, struct dm_task **task)
{
	static unsigned _in_event_counter = 0;
	sigset_t set;
	int ret = DM_WAIT_RETRY;
	struct dm_task *dmt;
	struct dm_info info;
	int ioctl_errno;

	*task = 0;

	DEBUGLOG("Preparing waitevent task for %s", thread->device.uuid);
	if (!(dmt = dm_task_create(DM_DEVICE_WAITEVENT)))
		return DM_WAIT_RETRY;

	thread->current_task = dmt;

	if (!dm_task_set_uuid(dmt, thread->device.uuid) ||
	    !dm_task_set_event_nr(dmt, thread->event_nr))
		goto out;

	_lock_mutex();
	/*
	 * Check if there are already some waiting events,
	 * in this case the logging is unmodified.
	 * TODO: audit libdm thread usage
	 */
	if (!_in_event_counter++)
		dm_log_init(_no_intr_log);
	_unlock_mutex();

	DEBUGLOG("Starting waitevent task for %s", thread->device.uuid);
	/*
	 * This is so that you can break out of waiting on an event,
	 * either for a timeout event, or to cancel the thread.
	 */
	set = _unblock_sigalrm();
	if (dm_task_run(dmt)) {
		thread->current_events |= DM_EVENT_DEVICE_ERROR;
		ret = DM_WAIT_INTR;

		if ((ret = dm_task_get_info(dmt, &info)))
			thread->event_nr = info.event_nr;
	} else {
		ioctl_errno = dm_task_get_errno(dmt);
		if (thread->events & DM_EVENT_TIMEOUT && ioctl_errno == EINTR) {
			thread->current_events |= DM_EVENT_TIMEOUT;
			ret = DM_WAIT_INTR;
		} else if (thread->status == DM_THREAD_SHUTDOWN && ioctl_errno == EINTR)
			ret = DM_WAIT_FATAL;
		else {
			syslog(LOG_NOTICE, "dm_task_run failed, errno = %d, %s",
			       ioctl_errno, strerror(ioctl_errno));
			if (ioctl_errno == ENXIO) {
				syslog(LOG_ERR, "%s disappeared, detaching",
				       thread->device.name);
				ret = DM_WAIT_FATAL;
			}
		}
	}
	DEBUGLOG("Completed waitevent task for %s", thread->device.uuid);

	pthread_sigmask(SIG_SETMASK, &set, NULL);
	_lock_mutex();
	if (--_in_event_counter == 0)
		dm_log_init(NULL);
	_unlock_mutex();

      out:
	if (ret == DM_WAIT_FATAL || ret == DM_WAIT_RETRY) {
		dm_task_destroy(dmt);
		thread->current_task = NULL;
	} else
		*task = dmt;

	return ret;
}

/* Register a device with the DSO. */
static int _do_register_device(struct thread_status *thread)
{
	return thread->dso_data->register_device(thread->device.name,
						 thread->device.uuid,
						 thread->device.major,
						 thread->device.minor,
						 &(thread->dso_private));
}

/* Unregister a device with the DSO. */
static int _do_unregister_device(struct thread_status *thread)
{
	return thread->dso_data->unregister_device(thread->device.name,
						   thread->device.uuid,
						   thread->device.major,
						   thread->device.minor,
						   &(thread->dso_private));
}

/* Process an event in the DSO. */
static void _do_process_event(struct thread_status *thread, struct dm_task *task)
{
	thread->dso_data->process_event(task, thread->current_events, &(thread->dso_private));
}

/* Thread cleanup handler to unregister device. */
static void _monitor_unregister(void *arg)
{
	struct thread_status *thread = arg, *thread_iter;

	DEBUGLOG("_monitor_unregister thread cleanup handler running");
	if (!_do_unregister_device(thread))
		syslog(LOG_ERR, "%s: %s unregister failed\n", __func__,
		       thread->device.name);
	if (thread->current_task) {
		dm_task_destroy(thread->current_task);
		thread->current_task = NULL;
	}

	_lock_mutex();
	if (thread->events & DM_EVENT_TIMEOUT) {
		/* _unregister_for_timeout locks another mutex, we
		   don't want to deadlock so we release our mutex for
		   a bit */
		_unlock_mutex();
		_unregister_for_timeout(thread);
		_lock_mutex();
	}
	/* we may have been relinked to unused registry since we were
	   called, so check that */
	dm_list_iterate_items(thread_iter, &_thread_registry_unused)
		if (thread_iter == thread) {
			thread->status = DM_THREAD_DONE;
			_unlock_mutex();
			return;
		}
	DEBUGLOG("Marking Thr %x as DONE and unused.", (int)thread->thread);
	thread->status = DM_THREAD_DONE;
	UNLINK_THREAD(thread);
	LINK(thread, &_thread_registry_unused);
	_unlock_mutex();
}

static struct dm_task *_get_device_status(struct thread_status *ts)
{
	struct dm_task *dmt = dm_task_create(DM_DEVICE_STATUS);

	if (!dmt)
		return NULL;

	if (!dm_task_set_uuid(dmt, ts->device.uuid)) {
		dm_task_destroy(dmt);
		return NULL;
	}

	if (!dm_task_run(dmt)) {
		dm_task_destroy(dmt);
		return NULL;
	}

	return dmt;
}

/* Device monitoring thread. */
static void *_monitor_thread(void *arg)
{
	struct thread_status *thread = arg;
	int wait_error;
	struct dm_task *task;

	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
	pthread_cleanup_push(_monitor_unregister, thread);

	/* Wait for do_process_request() to finish its task. */
	_lock_mutex();
	thread->status = DM_THREAD_RUNNING;
	_unlock_mutex();

	/* Loop forever awaiting/analyzing device events. */
	while (1) {
		thread->current_events = 0;

		wait_error = _event_wait(thread, &task);
		if (wait_error == DM_WAIT_RETRY) {
			usleep(100); /* avoid busy loop */
			continue;
		}

		if (wait_error == DM_WAIT_FATAL)
			break;

		/* Timeout occurred, task is not filled properly.
		 * We get device status here for processing it in DSO.
		 */
		if (wait_error == DM_WAIT_INTR &&
		    thread->current_events & DM_EVENT_TIMEOUT) {
			dm_task_destroy(task);
			task = _get_device_status(thread);
			/* FIXME: syslog fail here ? */
			if (!(thread->current_task = task))
				continue;
		}

		/*
		 * We know that wait succeeded and stored a
		 * pointer to dm_task with device status into task.
		 */

		/*
		 * Check against filter.
		 *
		 * If there's current events delivered from _event_wait() AND
		 * the device got registered for those events AND
		 * those events haven't been processed yet, call
		 * the DSO's process_event() handler.
		 */
		_lock_mutex();
		if (thread->status == DM_THREAD_SHUTDOWN) {
			_unlock_mutex();
			break;
		}

		if (thread->events & thread->current_events) {
			thread->processing = 1;
			_unlock_mutex();

			_do_process_event(thread, task);
			dm_task_destroy(task);
			thread->current_task = NULL;

			_lock_mutex();
			thread->processing = 0;
			_unlock_mutex();
		} else {
			_unlock_mutex();
			dm_task_destroy(task);
			thread->current_task = NULL;
		}
	}

	DEBUGLOG("Finished _monitor_thread");
	pthread_cleanup_pop(1);

	return NULL;
}

/* Create a device monitoring thread. */
static int _create_thread(struct thread_status *thread)
{
	return _pthread_create_smallstack(&thread->thread, _monitor_thread, thread);
}

static int _terminate_thread(struct thread_status *thread)
{
	DEBUGLOG("Sending SIGALRM to terminate Thr %x.", (int)thread->thread);
	return pthread_kill(thread->thread, SIGALRM);
}

/* DSO reference counting. Call with _global_mutex locked! */
static void _lib_get(struct dso_data *data)
{
	data->ref_count++;
}

static void _lib_put(struct dso_data *data)
{
	if (!--data->ref_count) {
		dlclose(data->dso_handle);
		UNLINK_DSO(data);
		_free_dso_data(data);
	}
}

/* Find DSO data. */
static struct dso_data *_lookup_dso(struct message_data *data)
{
	struct dso_data *dso_data, *ret = NULL;

	dm_list_iterate_items(dso_data, &_dso_registry)
		if (!strcmp(data->dso_name, dso_data->dso_name)) {
			_lib_get(dso_data);
			ret = dso_data;
			break;
		}

	return ret;
}

/* Lookup DSO symbols we need. */
static int _lookup_symbol(void *dl, void **symbol, const char *name)
{
	if ((*symbol = dlsym(dl, name)))
		return 1;

	return 0;
}

static int lookup_symbols(void *dl, struct dso_data *data)
{
	return _lookup_symbol(dl, (void *) &data->process_event,
			     "process_event") &&
	    _lookup_symbol(dl, (void *) &data->register_device,
			  "register_device") &&
	    _lookup_symbol(dl, (void *) &data->unregister_device,
			  "unregister_device");
}

/* Load an application specific DSO. */
static struct dso_data *_load_dso(struct message_data *data)
{
	void *dl;
	struct dso_data *ret;

	if (!(dl = dlopen(data->dso_name, RTLD_NOW))) {
		const char *dlerr = dlerror();
		syslog(LOG_ERR, "dmeventd %s dlopen failed: %s", data->dso_name,
		       dlerr);
		data->msg->size =
		    dm_asprintf(&(data->msg->data), "%s %s dlopen failed: %s",
				data->id, data->dso_name, dlerr);
		return NULL;
	}

	if (!(ret = _alloc_dso_data(data))) {
		dlclose(dl);
		return NULL;
	}

	if (!(lookup_symbols(dl, ret))) {
		_free_dso_data(ret);
		dlclose(dl);
		return NULL;
	}

	/*
	 * Keep handle to close the library once
	 * we've got no references to it any more.
	 */
	ret->dso_handle = dl;
	_lib_get(ret);

	_lock_mutex();
	LINK_DSO(ret);
	_unlock_mutex();

	return ret;
}

/* Return success on daemon active check. */
static int _active(struct message_data *message_data)
{
	return 0;
}

/*
 * Register for an event.
 *
 * Only one caller at a time here, because we use
 * a FIFO and lock it against multiple accesses.
 */
static int _register_for_event(struct message_data *message_data)
{
	int ret = 0;
	struct thread_status *thread, *thread_new = NULL;
	struct dso_data *dso_data;

	if (!(dso_data = _lookup_dso(message_data)) &&
	    !(dso_data = _load_dso(message_data))) {
		stack;
#ifdef ELIBACC
		ret = -ELIBACC;
#else
		ret = -ENODEV;
#endif
		goto out;
	}

	/* Preallocate thread status struct to avoid deadlock. */
	if (!(thread_new = _alloc_thread_status(message_data, dso_data))) {
		stack;
		ret = -ENOMEM;
		goto out;
	}

	if (!_fill_device_data(thread_new)) {
		stack;
		ret = -ENODEV;
		goto out;
	}

	/* If creation of timeout thread fails (as it may), we fail
	   here completely. The client is responsible for either
	   retrying later or trying to register without timeout
	   events. However, if timeout thread cannot be started, it
	   usually means we are so starved on resources that we are
	   almost as good as dead already... */
	if ((thread_new->events & DM_EVENT_TIMEOUT) &&
	    (ret = -_register_for_timeout(thread_new)))
		goto out;

	_lock_mutex();
	if (!(thread = _lookup_thread_status(message_data))) {
		_unlock_mutex();

		if (!(ret = _do_register_device(thread_new)))
			goto out;

		thread = thread_new;
		thread_new = NULL;

		/* Try to create the monitoring thread for this device. */
		_lock_mutex();
		if ((ret = -_create_thread(thread))) {
			_unlock_mutex();
			_do_unregister_device(thread);
			_free_thread_status(thread);
			goto out;
		}

		LINK_THREAD(thread);
	}

	/* Or event # into events bitfield. */
	thread->events |= message_data->events_field;
	_unlock_mutex();

      out:
	/*
	 * Deallocate thread status after releasing
	 * the lock in case we haven't used it.
	 */
	if (thread_new)
		_free_thread_status(thread_new);

	return ret;
}

/*
 * Unregister for an event.
 *
 * Only one caller at a time here as with register_for_event().
 */
static int _unregister_for_event(struct message_data *message_data)
{
	int ret = 0;
	struct thread_status *thread;

	/*
	 * Clear event in bitfield and deactivate
	 * monitoring thread in case bitfield is 0.
	 */
	_lock_mutex();

	if (!(thread = _lookup_thread_status(message_data))) {
		_unlock_mutex();
		ret = -ENODEV;
		goto out;
	}

	if (thread->status == DM_THREAD_DONE) {
		/* the thread has terminated while we were not
		   watching */
		_unlock_mutex();
		return 0;
	}

	thread->events &= ~message_data->events_field;

	if (!(thread->events & DM_EVENT_TIMEOUT)) {
		_unlock_mutex();
		_unregister_for_timeout(thread);
		_lock_mutex();
	}
	/*
	 * In case there's no events to monitor on this device ->
	 * unlink and terminate its monitoring thread.
	 */
	if (!thread->events) {
		DEBUGLOG("Marking Thr %x unused (no events).", (int)thread->thread);
		UNLINK_THREAD(thread);
		LINK(thread, &_thread_registry_unused);
	}
	_unlock_mutex();

      out:
	return ret;
}

/*
 * Get registered device.
 *
 * Only one caller at a time here as with register_for_event().
 */
static int _registered_device(struct message_data *message_data,
			     struct thread_status *thread)
{
	int r;
	struct dm_event_daemon_message *msg = message_data->msg;
	unsigned events = ((thread->status == DM_THREAD_RUNNING) &&
			   thread->events) ? thread->events :
			    thread->events | DM_EVENT_REGISTRATION_PENDING;

	dm_free(msg->data);

	if ((r = dm_asprintf(&(msg->data), "%s %s %s %u",
			     message_data->id,
			     thread->dso_data->dso_name,
			     thread->device.uuid, events)) < 0)
		return -ENOMEM;

	msg->size = (uint32_t) r;

	return 0;
}

static int _want_registered_device(char *dso_name, char *device_uuid,
				   struct thread_status *thread)
{
	/* If DSO names and device paths are equal. */
	if (dso_name && device_uuid)
		return !strcmp(dso_name, thread->dso_data->dso_name) &&
		    !strcmp(device_uuid, thread->device.uuid);

	/* If DSO names are equal. */
	if (dso_name)
		return !strcmp(dso_name, thread->dso_data->dso_name);

	/* If device paths are equal. */
	if (device_uuid)
		return !strcmp(device_uuid, thread->device.uuid);

	return 1;
}

static int _get_registered_dev(struct message_data *message_data, int next)
{
	struct thread_status *thread, *hit = NULL;
	int ret = -ENOENT;

	_lock_mutex();

	/* Iterate list of threads checking if we want a particular one. */
	dm_list_iterate_items(thread, &_thread_registry)
		if (_want_registered_device(message_data->dso_name,
					    message_data->device_uuid,
					    thread)) {
			hit = thread;
			break;
		}

	/*
	 * If we got a registered device and want the next one ->
	 * fetch next conforming element off the list.
	 */
	if (hit && !next)
		goto reg;

	/*
	 * If we didn't get a match, try the threads waiting to be deleted.
	 * FIXME Do something similar if 'next' is set.
	 */
	if (!hit && !next)
		dm_list_iterate_items(thread, &_thread_registry_unused)
			if (_want_registered_device(message_data->dso_name,
						    message_data->device_uuid, thread)) {
				hit = thread;
				goto reg;
			}

	if (!hit)
		goto out;

	while (1) {
		if (dm_list_end(&_thread_registry, &thread->list))
			goto out;

		thread = dm_list_item(thread->list.n, struct thread_status);
		if (_want_registered_device(message_data->dso_name, NULL, thread)) {
			hit = thread;
			break;
		}
	}

      reg:
	ret = _registered_device(message_data, hit);

      out:
	_unlock_mutex();

	return ret;
}

static int _get_registered_device(struct message_data *message_data)
{
	return _get_registered_dev(message_data, 0);
}

static int _get_next_registered_device(struct message_data *message_data)
{
	return _get_registered_dev(message_data, 1);
}

static int _set_timeout(struct message_data *message_data)
{
	struct thread_status *thread;

	_lock_mutex();
	if ((thread = _lookup_thread_status(message_data)))
		thread->timeout = message_data->timeout_secs;
	_unlock_mutex();

	return thread ? 0 : -ENODEV;
}

static int _get_timeout(struct message_data *message_data)
{
	struct thread_status *thread;
	struct dm_event_daemon_message *msg = message_data->msg;

	dm_free(msg->data);

	_lock_mutex();
	if ((thread = _lookup_thread_status(message_data))) {
		msg->size = dm_asprintf(&(msg->data), "%s %" PRIu32,
					message_data->id, thread->timeout);
	} else
		msg->data = NULL;

	_unlock_mutex();

	return thread ? 0 : -ENODEV;
}

/* Open fifos used for client communication. */
static int _open_fifos(struct dm_event_fifos *fifos)
{
	struct stat st;

	/* Create client fifo. */
	(void) dm_prepare_selinux_context(fifos->client_path, S_IFIFO);
	if ((mkfifo(fifos->client_path, 0600) == -1) && errno != EEXIST) {
		syslog(LOG_ERR, "%s: Failed to create client fifo %s: %m.\n",
		       __func__, fifos->client_path);
		(void) dm_prepare_selinux_context(NULL, 0);
		goto fail;
	}

	/* Create server fifo. */
	(void) dm_prepare_selinux_context(fifos->server_path, S_IFIFO);
	if ((mkfifo(fifos->server_path, 0600) == -1) && errno != EEXIST) {
		syslog(LOG_ERR, "%s: Failed to create server fifo %s: %m.\n",
		       __func__, fifos->server_path);
		(void) dm_prepare_selinux_context(NULL, 0);
		goto fail;
	}

	(void) dm_prepare_selinux_context(NULL, 0);

	/* Warn about wrong permissions if applicable */
	if ((!stat(fifos->client_path, &st)) && (st.st_mode & 0777) != 0600)
		syslog(LOG_WARNING, "Fixing wrong permissions on %s: %m.\n",
		       fifos->client_path);

	if ((!stat(fifos->server_path, &st)) && (st.st_mode & 0777) != 0600)
		syslog(LOG_WARNING, "Fixing wrong permissions on %s: %m.\n",
		       fifos->server_path);

	/* If they were already there, make sure permissions are ok. */
	if (chmod(fifos->client_path, 0600)) {
		syslog(LOG_ERR, "Unable to set correct file permissions on %s: %m.\n",
		       fifos->client_path);
		goto fail;
	}

	if (chmod(fifos->server_path, 0600)) {
		syslog(LOG_ERR, "Unable to set correct file permissions on %s: %m.\n",
		       fifos->server_path);
		goto fail;
	}

	/* Need to open read+write or we will block or fail */
	if ((fifos->server = open(fifos->server_path, O_RDWR)) < 0) {
		syslog(LOG_ERR, "Failed to open fifo server %s: %m.\n",
		       fifos->server_path);
		goto fail;
	}

	if (fcntl(fifos->server, F_SETFD, FD_CLOEXEC) < 0) {
		syslog(LOG_ERR, "Failed to set FD_CLOEXEC for fifo server %s: %m.\n",
		       fifos->server_path);
		goto fail;
	}

	/* Need to open read+write for select() to work. */
	if ((fifos->client = open(fifos->client_path, O_RDWR)) < 0) {
		syslog(LOG_ERR, "Failed to open fifo client %s: %m", fifos->client_path);
		goto fail;
	}

	if (fcntl(fifos->client, F_SETFD, FD_CLOEXEC) < 0) {
		syslog(LOG_ERR, "Failed to set FD_CLOEXEC for fifo client %s: %m.\n",
		       fifos->client_path);
		goto fail;
	}

	return 1;
fail:
	if (fifos->server >= 0 && close(fifos->server))
		syslog(LOG_ERR, "Failed to close fifo server %s: %m", fifos->server_path);

	if (fifos->client >= 0 && close(fifos->client))
		syslog(LOG_ERR, "Failed to close fifo client %s: %m", fifos->client_path);

	return 0;
}

/*
 * Read message from client making sure that data is available
 * and a complete message is read.  Must not block indefinitely.
 */
static int _client_read(struct dm_event_fifos *fifos,
		       struct dm_event_daemon_message *msg)
{
	struct timeval t;
	unsigned bytes = 0;
	int ret = 0;
	fd_set fds;
	size_t size = 2 * sizeof(uint32_t);	/* status + size */
	uint32_t *header = alloca(size);
	char *buf = (char *)header;

	msg->data = NULL;

	errno = 0;
	while (bytes < size && errno != EOF) {
		/* Watch client read FIFO for input. */
		FD_ZERO(&fds);
		FD_SET(fifos->client, &fds);
		t.tv_sec = 1;
		t.tv_usec = 0;
		ret = select(fifos->client + 1, &fds, NULL, NULL, &t);

		if (!ret && !bytes)	/* nothing to read */
			return 0;

		if (!ret)	/* trying to finish read */
			continue;

		if (ret < 0)	/* error */
			return 0;

		ret = read(fifos->client, buf + bytes, size - bytes);
		bytes += ret > 0 ? ret : 0;
		if (header && (bytes == 2 * sizeof(uint32_t))) {
			msg->cmd = ntohl(header[0]);
			msg->size = ntohl(header[1]);
			buf = msg->data = dm_malloc(msg->size);
			size = msg->size;
			bytes = 0;
			header = 0;
		}
	}

	if (bytes != size) {
		dm_free(msg->data);
		msg->data = NULL;
		return 0;
	}

	return 1;
}

/*
 * Write a message to the client making sure that it is ready to write.
 */
static int _client_write(struct dm_event_fifos *fifos,
			struct dm_event_daemon_message *msg)
{
	uint32_t temp[2];
	unsigned bytes = 0;
	int ret = 0;
	fd_set fds;

	size_t size = 2 * sizeof(uint32_t) + ((msg->data) ? msg->size : 0);
	uint32_t *header = dm_malloc(size);
	char *buf = (char *)header;

	if (!header) {
		/* Reply with ENOMEM message */
		header = temp;
		size = sizeof(temp);
		header[0] = htonl(-ENOMEM);
		header[1] = 0;
	} else {
		header[0] = htonl(msg->cmd);
		header[1] = htonl((msg->data) ? msg->size : 0);
		if (msg->data)
			memcpy(buf + 2 * sizeof(uint32_t), msg->data, msg->size);
	}

	while (bytes < size) {
		do {
			/* Watch client write FIFO to be ready for output. */
			FD_ZERO(&fds);
			FD_SET(fifos->server, &fds);
		} while (select(fifos->server + 1, NULL, &fds, NULL, NULL) != 1);

		if ((ret = write(fifos->server, buf + bytes, size - bytes)) > 0)
			bytes += ret;
		else if (errno == EIO)
			break;
	}

	if (header != temp)
		dm_free(header);

	return (bytes == size);
}

/*
 * Handle a client request.
 *
 * We put the request handling functions into
 * a list because of the growing number.
 */
static int _handle_request(struct dm_event_daemon_message *msg,
			  struct message_data *message_data)
{
	switch (msg->cmd) {
	case DM_EVENT_CMD_REGISTER_FOR_EVENT:
		return _register_for_event(message_data);
	case DM_EVENT_CMD_UNREGISTER_FOR_EVENT:
		return _unregister_for_event(message_data);
	case DM_EVENT_CMD_GET_REGISTERED_DEVICE:
		return _get_registered_device(message_data);
	case DM_EVENT_CMD_GET_NEXT_REGISTERED_DEVICE:
		return _get_next_registered_device(message_data);
	case DM_EVENT_CMD_SET_TIMEOUT:
		return _set_timeout(message_data);
	case DM_EVENT_CMD_GET_TIMEOUT:
		return _get_timeout(message_data);
	case DM_EVENT_CMD_ACTIVE:
		return _active(message_data);
	case DM_EVENT_CMD_GET_STATUS:
		return _get_status(message_data);
	/* dmeventd parameters of running dmeventd,
	 * returns 'pid=<pid> daemon=<no/yes> exec_method=<direct/systemd>'
	 * 	pid - pidfile of running dmeventd
	 * 	daemon - running as a daemon or not (foreground)?
	 * 	exec_method - "direct" if executed directly or
	 * 		      "systemd" if executed via systemd
	 */
	case DM_EVENT_CMD_GET_PARAMETERS:
		return _get_parameters(message_data);
	default:
		return -EINVAL;
	}
}

/* Process a request passed from the communication thread. */
static int _do_process_request(struct dm_event_daemon_message *msg)
{
	int ret;
	char *answer;
	struct message_data message_data = { .msg =  msg };

	/* Parse the message. */
	if (msg->cmd == DM_EVENT_CMD_HELLO || msg->cmd == DM_EVENT_CMD_DIE)  {
		ret = 0;
		answer = msg->data;
		if (answer) {
			msg->size = dm_asprintf(&(msg->data), "%s %s %d", answer,
						(msg->cmd == DM_EVENT_CMD_DIE) ? "DYING" : "HELLO",
						DM_EVENT_PROTOCOL_VERSION);
			dm_free(answer);
		}
	} else if (msg->cmd != DM_EVENT_CMD_ACTIVE && !_parse_message(&message_data)) {
		stack;
		ret = -EINVAL;
	} else
		ret = _handle_request(msg, &message_data);

	msg->cmd = ret;
	if (!msg->data)
		msg->size = dm_asprintf(&(msg->data), "%s %s", message_data.id, strerror(-ret));

	_free_message(&message_data);

	return ret;
}

/* Only one caller at a time. */
static void _process_request(struct dm_event_fifos *fifos)
{
	int die;
	struct dm_event_daemon_message msg = { 0 };
#ifdef DEBUG
	const char *cmd;
#endif

	/*
	 * Read the request from the client (client_read, client_write
	 * give true on success and false on failure).
	 */
	if (!_client_read(fifos, &msg))
		return;

	DEBUGLOG("%s (0x%x) processing...", decode_cmd(msg.cmd), msg.cmd);

	die = (msg.cmd == DM_EVENT_CMD_DIE) ? 1 : 0;

	/* _do_process_request fills in msg (if memory allows for
	   data, otherwise just cmd and size = 0) */
	_do_process_request(&msg);

	if (!_client_write(fifos, &msg))
		stack;

	dm_free(msg.data);

	DEBUGLOG("%s (0x%x) completed.", decode_cmd(msg.cmd), msg.cmd);

	if (die) {
		if (unlink(DMEVENTD_PIDFILE))
			perror(DMEVENTD_PIDFILE ": unlink failed");
		_exit(0);
	}
}

static void _process_initial_registrations(void)
{
	int i;
	char *reg;
	struct dm_event_daemon_message msg = { 0 };

	for (i = 0; (reg = _initial_registrations[i]); ++i) {
		msg.cmd = DM_EVENT_CMD_REGISTER_FOR_EVENT;
		if ((msg.size = strlen(reg))) {
			msg.data = reg;
			_do_process_request(&msg);
		}
	}
}

static void _cleanup_unused_threads(void)
{
	int ret;
	struct dm_list *l;
	struct thread_status *thread;
	int join_ret = 0;

	_lock_mutex();
	while ((l = dm_list_first(&_thread_registry_unused))) {
		thread = dm_list_item(l, struct thread_status);
		if (thread->processing)
			break;	/* cleanup on the next round */

		if (thread->status == DM_THREAD_RUNNING) {
			thread->status = DM_THREAD_SHUTDOWN;
			break;
		}

		if (thread->status == DM_THREAD_SHUTDOWN) {
			if (!thread->events) {
				/* turn codes negative -- should we be returning this? */
				ret = _terminate_thread(thread);

				if (ret == ESRCH) {
					thread->status = DM_THREAD_DONE;
				} else if (ret) {
					syslog(LOG_ERR, "Unable to terminate thread: %s",
					       strerror(ret));
				}
				break;
			}

			dm_list_del(l);
			syslog(LOG_ERR,
			       "thread can't be on unused list unless !thread->events");
			thread->status = DM_THREAD_RUNNING;
			LINK_THREAD(thread);

			continue;
		}

		if (thread->status == DM_THREAD_DONE) {
			DEBUGLOG("Destroying Thr %x.", (int)thread->thread);
			dm_list_del(l);
			_unlock_mutex();
			join_ret = pthread_join(thread->thread, NULL);
			_free_thread_status(thread);
			_lock_mutex();
		}
	}

	_unlock_mutex();

	if (join_ret)
		syslog(LOG_ERR, "Failed pthread_join: %s\n", strerror(join_ret));
}

static void _sig_alarm(int signum __attribute__((unused)))
{
	/* empty SIG_IGN */;
}

/* Init thread signal handling. */
static void _init_thread_signals(void)
{
	sigset_t my_sigset;
	struct sigaction act = { .sa_handler = _sig_alarm };

	sigaction(SIGALRM, &act, NULL);
	sigfillset(&my_sigset);

	/* These are used for exiting */
	sigdelset(&my_sigset, SIGTERM);
	sigdelset(&my_sigset, SIGINT);
	sigdelset(&my_sigset, SIGHUP);
	sigdelset(&my_sigset, SIGQUIT);

	pthread_sigmask(SIG_BLOCK, &my_sigset, NULL);
}

/*
 * exit_handler
 * @sig
 *
 * Set the global variable which the process should
 * be watching to determine when to exit.
 */
static void _exit_handler(int sig __attribute__((unused)))
{
	_exit_now = 1;
}

#ifdef __linux__
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

static int _handle_preloaded_fifo(int fd, const char *path)
{
	struct stat st_fd, st_path;
	int flags;

	if ((flags = fcntl(fd, F_GETFD)) < 0)
		return 0;

	if (flags & FD_CLOEXEC)
		return 0;

	if (fstat(fd, &st_fd) < 0 || !S_ISFIFO(st_fd.st_mode))
		return 0;

	if (stat(path, &st_path) < 0 ||
	    st_path.st_dev != st_fd.st_dev ||
	    st_path.st_ino != st_fd.st_ino)
		return 0;

	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		return 0;

	return 1;
}

static int _systemd_handover(struct dm_event_fifos *fifos)
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

	/* LISTEN_FDS must be 2 and the fds must be FIFOSs! */
	if (!(e = getenv(SD_LISTEN_FDS_ENV_VAR_NAME)))
		goto out;

	errno = 0;
	env_listen_fds = strtoul(e, &p, 10);
	if (errno || !p || *p || env_listen_fds != 2)
		goto out;

	/* Check and handle the FIFOs passed in */
	r = (_handle_preloaded_fifo(SD_FD_FIFO_SERVER, DM_EVENT_FIFO_SERVER) &&
	     _handle_preloaded_fifo(SD_FD_FIFO_CLIENT, DM_EVENT_FIFO_CLIENT));

	if (r) {
		fifos->server = SD_FD_FIFO_SERVER;
		fifos->server_path = DM_EVENT_FIFO_SERVER;
		fifos->client = SD_FD_FIFO_CLIENT;
		fifos->client_path = DM_EVENT_FIFO_CLIENT;
	}

out:
	unsetenv(SD_ACTIVATION_ENV_VAR_NAME);
	unsetenv(SD_LISTEN_PID_ENV_VAR_NAME);
	unsetenv(SD_LISTEN_FDS_ENV_VAR_NAME);
	return r;
}

#endif

static void _remove_files_on_exit(void)
{
	if (unlink(DMEVENTD_PIDFILE))
		perror(DMEVENTD_PIDFILE ": unlink failed");

	if (!_systemd_activation) {
		if (unlink(DM_EVENT_FIFO_CLIENT))
			perror(DM_EVENT_FIFO_CLIENT " : unlink failed");

		if (unlink(DM_EVENT_FIFO_SERVER))
			perror(DM_EVENT_FIFO_SERVER " : unlink failed");
	}
}

static void _daemonize(void)
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

		if (_exit_now)	/* Child has signaled it is ok - we can exit now */
			exit(EXIT_SUCCESS);

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
		exit(EXIT_CHDIR_FAILURE);

	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
		fd = 256;	/* just have to guess */
	else
		fd = rlim.rlim_cur;

	for (--fd; fd >= 0; fd--) {
#ifdef __linux__
		/* Do not close fds preloaded by systemd! */
		if (_systemd_activation &&
		    (fd == SD_FD_FIFO_SERVER || fd == SD_FD_FIFO_CLIENT))
			continue;
#endif
		(void) close(fd);
	}

	if ((open("/dev/null", O_RDONLY) < 0) ||
	    (open("/dev/null", O_WRONLY) < 0) ||
	    (open("/dev/null", O_WRONLY) < 0))
		exit(EXIT_DESC_OPEN_FAILURE);

	setsid();
}

static int _reinstate_registrations(struct dm_event_fifos *fifos)
{
	static const char _failed_parsing_msg[] = "Failed to parse existing event registration.\n";
	static const char *_delim = " ";
	struct dm_event_daemon_message msg = { 0 };
	char *endp, *dso_name, *dev_name, *mask, *timeout;
	unsigned long mask_value, timeout_value;
	int i, ret;

	ret = daemon_talk(fifos, &msg, DM_EVENT_CMD_HELLO, NULL, NULL, 0, 0);
	dm_free(msg.data);
	msg.data = NULL;

	if (ret) {
		fprintf(stderr, "Failed to communicate with new instance of dmeventd.\n");
		return 0;
	}

	for (i = 0; _initial_registrations[i]; ++i) {
		if (!(strtok(_initial_registrations[i], _delim)) ||
		    !(dso_name = strtok(NULL, _delim)) ||
		    !(dev_name = strtok(NULL, _delim)) ||
		    !(mask = strtok(NULL, _delim)) ||
		    !(timeout = strtok(NULL, _delim))) {
			fprintf(stderr, _failed_parsing_msg);
			continue;
		}

		errno = 0;
		mask_value = strtoul(mask, &endp, 10);
		if (errno || !endp || *endp) {
			fprintf(stderr, _failed_parsing_msg);
			continue;
		}

		errno = 0;
		timeout_value = strtoul(timeout, &endp, 10);
		if (errno || !endp || *endp) {
			fprintf(stderr, _failed_parsing_msg);
			continue;
		}

		if (daemon_talk(fifos, &msg, DM_EVENT_CMD_REGISTER_FOR_EVENT,
				dso_name,
				dev_name,
				(enum dm_event_mask) mask_value,
				timeout_value))
			fprintf(stderr, "Failed to reinstate monitoring for device %s.\n", dev_name);
	}

	return 1;
}

static void restart(void)
{
	struct dm_event_fifos fifos = {
		.server = -1,
		.client = -1,
		/* FIXME Make these either configurable or depend directly on dmeventd_path */
		.client_path = DM_EVENT_FIFO_CLIENT,
		.server_path = DM_EVENT_FIFO_SERVER
	};
	struct dm_event_daemon_message msg = { 0 };
	int i, count = 0;
	char *message;
	int version;
	const char *e;

	/* Get the list of registrations from the running daemon. */
	if (!init_fifos(&fifos)) {
		fprintf(stderr, "WARNING: Could not initiate communication with existing dmeventd.\n");
		exit(EXIT_FAILURE);
	}

	if (!dm_event_get_version(&fifos, &version)) {
		fprintf(stderr, "WARNING: Could not communicate with existing dmeventd.\n");
		goto bad;
	}

	if (version < 1) {
		fprintf(stderr, "WARNING: The running dmeventd instance is too old.\n"
				"Protocol version %d (required: 1). Action cancelled.\n",
				version);
		goto bad;
	}

	if (daemon_talk(&fifos, &msg, DM_EVENT_CMD_GET_STATUS, "-", "-", 0, 0))
		goto bad;

	message = strchr(msg.data, ' ') + 1;
	for (i = 0; msg.data[i]; ++i)
		if (msg.data[i] == ';') {
			msg.data[i] = 0;
			++count;
		}

	if (!(_initial_registrations = dm_malloc(sizeof(char*) * (count + 1)))) {
		fprintf(stderr, "Memory allocation registration failed.\n");
		goto bad;
	}

	for (i = 0; i < count; ++i) {
		if (!(_initial_registrations[i] = dm_strdup(message))) {
			fprintf(stderr, "Memory allocation for message failed.\n");
			goto bad;
		}
		message += strlen(message) + 1;
	}
	_initial_registrations[count] = NULL;

	if (version >= 2) {
		if (daemon_talk(&fifos, &msg, DM_EVENT_CMD_GET_PARAMETERS, "-", "-", 0, 0)) {
			fprintf(stderr, "Failed to acquire parameters from old dmeventd.\n");
			goto bad;
		}
		if (strstr(msg.data, "exec_method=systemd"))
			_systemd_activation = 1;
	}
#ifdef __linux__
	/*
	* If the protocol version is old, just assume that if systemd is running,
	* the dmeventd is also run as a systemd service via fifo activation.
	*/
	if (version < 2) {
		/* This check is copied from sd-daemon.c. */
		struct stat st;
		if (!lstat(SD_RUNTIME_UNIT_FILE_DIR, &st) && !!S_ISDIR(st.st_mode))
			_systemd_activation = 1;
	}
#endif

	if (daemon_talk(&fifos, &msg, DM_EVENT_CMD_DIE, "-", "-", 0, 0)) {
		fprintf(stderr, "Old dmeventd refused to die.\n");
		goto bad;
	}

	if (!_systemd_activation &&
	    ((e = getenv(SD_ACTIVATION_ENV_VAR_NAME)) && strcmp(e, "1")))
		_systemd_activation = 1;

	for (i = 0; i < 10; ++i) {
		if ((access(DMEVENTD_PIDFILE, F_OK) == -1) && (errno == ENOENT))
			break;
		usleep(10);
	}

	if (!_systemd_activation) {
		fini_fifos(&fifos);
		return;
	}

	/* Reopen fifos. */
	fini_fifos(&fifos);
	if (!init_fifos(&fifos)) {
		fprintf(stderr, "Could not initiate communication with new instance of dmeventd.\n");
		exit(EXIT_FAILURE);
	}

	if (!_reinstate_registrations(&fifos)) {
		fprintf(stderr, "Failed to reinstate monitoring with new instance of dmeventd.\n");
		goto bad;
	}

	fini_fifos(&fifos);
	exit(EXIT_SUCCESS);
bad:
	fini_fifos(&fifos);
	exit(EXIT_FAILURE);
}

static void usage(char *prog, FILE *file)
{
	fprintf(file, "Usage:\n"
		"%s [-d [-d [-d]]] [-f] [-h] [-R] [-V] [-?]\n\n"
		"   -d       Log debug messages to syslog (-d, -dd, -ddd)\n"
		"   -f       Don't fork, run in the foreground\n"
		"   -h -?    Show this help information\n"
		"   -R       Restart dmeventd\n"
		"   -V       Show version of dmeventd\n\n", prog);
}

int main(int argc, char *argv[])
{
	signed char opt;
	struct dm_event_fifos fifos = {
		.client = -1,
		.server = -1,
		.client_path = DM_EVENT_FIFO_CLIENT,
		.server_path = DM_EVENT_FIFO_SERVER
	};
	int nothreads;
	//struct sys_log logdata = {DAEMON_NAME, LOG_DAEMON};

	opterr = 0;
	optind = 0;

	while ((opt = getopt(argc, argv, "?fhVdR")) != EOF) {
		switch (opt) {
		case 'h':
			usage(argv[0], stdout);
			exit(EXIT_SUCCESS);
		case '?':
			usage(argv[0], stderr);
			exit(EXIT_SUCCESS);
		case 'R':
			_restart++;
			break;
		case 'f':
			_foreground++;
			break;
		case 'd':
			dmeventd_debug++;
			break;
		case 'V':
			printf("dmeventd version: %s\n", DM_LIB_VERSION);
			exit(EXIT_SUCCESS);
		}
	}

	/*
	 * Switch to C locale to avoid reading large locale-archive file
	 * used by some glibc (on some distributions it takes over 100MB).
	 * Daemon currently needs to use mlockall().
	 */
	if (setenv("LC_ALL", "C", 1))
		perror("Cannot set LC_ALL to C");

	if (_restart)
		restart();

#ifdef __linux__
	_systemd_activation = _systemd_handover(&fifos);
#endif

	if (!_foreground)
		_daemonize();

	openlog("dmeventd", LOG_PID, LOG_DAEMON);

	(void) dm_prepare_selinux_context(DMEVENTD_PIDFILE, S_IFREG);
	if (dm_create_lockfile(DMEVENTD_PIDFILE) == 0)
		exit(EXIT_FAILURE);

	atexit(_remove_files_on_exit);
	(void) dm_prepare_selinux_context(NULL, 0);

	/* Set the rest of the signals to cause '_exit_now' to be set */
	signal(SIGTERM, &_exit_handler);
	signal(SIGINT, &_exit_handler);
	signal(SIGHUP, &_exit_handler);
	signal(SIGQUIT, &_exit_handler);

#ifdef __linux__
	/* Systemd has adjusted oom killer for us already */
	if (!_systemd_activation && !_protect_against_oom_killer())
		syslog(LOG_ERR, "Failed to protect against OOM killer");
#endif

	_init_thread_signals();

	//multilog_clear_logging();
	//multilog_add_type(std_syslog, &logdata);
	//multilog_init_verbose(std_syslog, _LOG_DEBUG);
	//multilog_async(1);

	pthread_mutex_init(&_global_mutex, NULL);

	if (!_systemd_activation && !_open_fifos(&fifos))
		exit(EXIT_FIFO_FAILURE);

	/* Signal parent, letting them know we are ready to go. */
	if (!_foreground)
		kill(getppid(), SIGTERM);
	syslog(LOG_NOTICE, "dmeventd ready for processing.");

	if (_initial_registrations)
		_process_initial_registrations();

	for (;;) {
		if (_exit_now) {
			_exit_now = 0;
			/*
			 * When '_exit_now' is set, signal has been received,
			 * but can not simply exit unless all
			 * threads are done processing.
			 */
			_lock_mutex();
			nothreads = (dm_list_empty(&_thread_registry) &&
				     dm_list_empty(&_thread_registry_unused));
			_unlock_mutex();
			if (nothreads)
				break;
			syslog(LOG_ERR, "There are still devices being monitored.");
			syslog(LOG_ERR, "Refusing to exit.");
		}
		_process_request(&fifos);
		_cleanup_unused_threads();
	}

	_exit_dm_lib();

	pthread_mutex_destroy(&_global_mutex);

	syslog(LOG_NOTICE, "dmeventd shutting down.");
	closelog();

	exit(EXIT_SUCCESS);
}
