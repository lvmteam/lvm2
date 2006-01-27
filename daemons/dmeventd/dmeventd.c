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

/*
 * dmeventd - dm event daemon to monitor active mapped devices
 */

#include "libdevmapper.h"
#include "libdevmapper-event.h"
#include "list.h"
#include "dmeventd.h"
//#include "libmultilog.h"
#include "log.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>

#ifdef linux
#include <malloc.h>
#endif

/* FIXME Use dm library */
#define	dbg_malloc(x...)	malloc(x)
#define	dbg_strdup(x...)	strdup(x)
#define	dbg_free(x...)		free(x)

/* List (un)link macros. */
#define	LINK(x, head)		list_add(head, &(x)->list)
#define	LINK_DSO(dso)		LINK(dso, &dso_registry)
#define	LINK_THREAD(thread)	LINK(thread, &thread_registry)

#define	UNLINK(x)		list_del(&(x)->list)
#define	UNLINK_DSO(x)		UNLINK(x)
#define	UNLINK_THREAD(x)	UNLINK(x)

#define DAEMON_NAME "dmeventd"

/* Global mutex for list accesses. */
static pthread_mutex_t mutex;

/* Data kept about a DSO. */
struct dso_data {
	struct list list;

	char *dso_name; /* DSO name (eg, "evms", "dmraid", "lvm2"). */

	void *dso_handle; /* Opaque handle as returned from dlopen(). */
	unsigned int ref_count; /* Library reference count. */

	/*
	 * Event processing.
	 *
	 * The DSO can do whatever appropriate steps if an event happens
	 * such as changing the mapping in case a mirror fails, update
	 * the application metadata etc.
	 */
	void (*process_event)(const char *device, enum dm_event_type event);

	/*
	 * Device registration.
	 *
	 * When an application registers a device for an event, the DSO
	 * can carry out appropriate steps so that a later call to
	 * the process_event() function is sane (eg, read metadata
	 * and activate a mapping).
	 */
	int (*register_device)(const char *device);

	/*
	 * Device unregistration.
	 *
	 * In case all devices of a mapping (eg, RAID10) are unregistered
	 * for events, the DSO can recognize this and carry out appropriate
	 * steps (eg, deactivate mapping, metadata update).
	 */
	int (*unregister_device)(const char *device);
};
static LIST_INIT(dso_registry);

/* Structure to keep parsed register variables from client message. */
struct message_data {
	char *dso_name;		/* Name of DSO. */
	char *device_path;	/* Mapped device path. */
	union {
		char *str;	/* Events string as fetched from message. */
		enum dm_event_type field;	/* Events bitfield. */
	} events;
	union {
		char *str;
		uint32_t secs;
	} timeout;
	struct dm_event_daemon_message *msg;	/* Pointer to message buffer. */
};

/*
 * Housekeeping of thread+device states.
 *
 * One thread per mapped device which can block on it until an event
 * occurs and the event processing function of the DSO gets called.
 */
struct thread_status {
	struct list	list;

	pthread_t		thread;

	struct dso_data *dso_data;/* DSO this thread accesses. */
	
	char *device_path;	/* Mapped device path. */
	int event_nr;           /* event number */
	int processing;         /* Set when event is being processed */
	enum dm_event_type events;	/* bitfield for event filter. */
	enum dm_event_type current_events;/* bitfield for occured events. */
	enum dm_event_type processed_events;/* bitfield for processed events. */
	time_t next_time;
	uint32_t timeout;
	struct list timeout_list;
};
static LIST_INIT(thread_registry);
static LIST_INIT(thread_registry_unused);

static int timeout_running;
static LIST_INIT(timeout_registry);
static pthread_mutex_t timeout_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t timeout_cond = PTHREAD_COND_INITIALIZER;

/* Allocate/free the status structure for a monitoring thread. */
static struct thread_status *alloc_thread_status(struct message_data *data,
						 struct dso_data *dso_data)
{
	struct thread_status *ret = (typeof(ret)) dbg_malloc(sizeof(*ret));

	if (ret) {
		if (!memset(ret, 0, sizeof(*ret)) ||
		    !(ret->device_path = dbg_strdup(data->device_path))) {
			dbg_free(ret);
			ret = NULL;
		} else {
			ret->dso_data = dso_data;
			ret->events   = data->events.field;
			ret->timeout  = data->timeout.secs;
			list_init(&ret->timeout_list);
		}
	}

	return ret;
}

static void free_thread_status(struct thread_status *thread)
{
	dbg_free(thread->device_path);
	dbg_free(thread);
}

/* Allocate/free DSO data. */
static struct dso_data *alloc_dso_data(struct message_data *data)
{
	struct dso_data *ret = (typeof(ret)) dbg_malloc(sizeof(*ret));

	if (ret) {
		if (!memset(ret, 0, sizeof(*ret)) ||
		    !(ret->dso_name = dbg_strdup(data->dso_name))) {
			dbg_free(ret);
			ret = NULL;
		}
	}

	return ret;
}

static void free_dso_data(struct dso_data *data)
{
	dbg_free(data->dso_name);
	dbg_free(data);
}

/* FIXME: Factor out. */
static char *dm_basename(char *str)
{
	char *p = strrchr(str, '/');

	return p ? p + 1 : str;
}

/*
 * Fetch a string off src and duplicate it into *ptr.
 * Pay attention to 0 lenght strings.
 */
/* FIXME: move to separate module to share with the client lib. */
static const char delimiter = ' ';
static int fetch_string(char **ptr, char **src)
{
	int ret = 0;
	char *p;
	size_t len;

	if ((p = strchr(*src, delimiter)))
		*p = 0;

	if ((*ptr = dbg_strdup(*src))) {
		if ((len = strlen(*ptr)))
			*src += len;
		else {
			dbg_free(*ptr);
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
static void free_message(struct message_data *message_data)
{
	if (message_data->dso_name)
		dbg_free(message_data->dso_name);

	if (message_data->device_path)
		dbg_free(message_data->device_path);
}

/* Parse a register message from the client. */
static int parse_message(struct message_data *message_data)
{
	char *p = message_data->msg->msg;

	/*
	 * Retrieve application identifier, mapped device
	 * path and events # string from message.
	 */
	if (fetch_string(&message_data->dso_name, &p) &&
	    fetch_string(&message_data->device_path, &p) &&
	    fetch_string(&message_data->events.str, &p) &&
	    fetch_string(&message_data->timeout.str, &p)) {
		if (message_data->events.str) {
			enum dm_event_type i = atoi(message_data->events.str);

			/*
			 * Free string representaion of events.
			 * Not needed an more.
			 */
			dbg_free(message_data->events.str);
			message_data->events.field = i;
		}
		if (message_data->timeout.str) {
			uint32_t secs = atoi(message_data->timeout.str);
			dbg_free(message_data->timeout.str);
			message_data->timeout.secs = secs ? secs :
							    DM_EVENT_DEFAULT_TIMEOUT;
		}

		return 1;
	}

	return 0;
};

/* Global mutex to lock access to lists et al. */
static int lock_mutex(void)
{
	return pthread_mutex_lock(&mutex);
}

static int unlock_mutex(void)
{
	return pthread_mutex_unlock(&mutex);
}

/* Store pid in pidfile. */
static int storepid(int lf)
{
	int len;
	char pid[8];

	if ((len = snprintf(pid, sizeof(pid), "%u\n", getpid())) < 0)
		return 0;

	if (len > sizeof(pid))
		len = sizeof(pid);

	if (write(lf, pid, len) != len)
		return 0;

	fsync(lf);

	return 1;
}


/* Check, if a device exists. */
static int device_exists(char *device)
{
	struct stat st_buf;

	return !stat(device, &st_buf) && S_ISBLK(st_buf.st_mode);
}

/*
 * Find an existing thread for a device.
 *
 * Mutex must be hold when calling this.
 */
static struct thread_status *lookup_thread_status(struct message_data *data)
{
	struct thread_status *thread;

	list_iterate_items(thread, &thread_registry) {
		if (!strcmp(data->device_path, thread->device_path))
			return thread;
	}

	return NULL;
}


/* Cleanup at exit. */
static void exit_dm_lib(void)
{
	dm_lib_release();
	dm_lib_exit();
}

/* Derive error case from target parameter string. */
/* FIXME Remove? */
static int error_detected(struct thread_status *thread, char *params) __attribute__ ((unused));
static int error_detected(struct thread_status *thread, char *params)
{
	size_t len;
/*
  Leave it to the DSO to decide how to interpret the status info
	if ((len = strlen(params)) &&
	    params[len - 1] == 'F') {
*/
	if (params && (len = strlen(params))) {
		thread->current_events |= DM_EVENT_DEVICE_ERROR;
		return 1;
	}

	return 0;
}

static void exit_timeout(void *unused)
{
	timeout_running = 0;
	pthread_mutex_unlock(&timeout_mutex);
}

/* Wake up monitor threads every so often. */
static void *timeout_thread(void *unused)
{
	struct timespec timeout;
	time_t curr_time;

	timeout.tv_nsec = 0;
	pthread_cleanup_push(exit_timeout, NULL);
	pthread_mutex_lock(&timeout_mutex);

	while (!list_empty(&timeout_registry)) {
		struct thread_status *thread;

		timeout.tv_sec = (time_t)-1;
		curr_time = time(NULL);

		list_iterate_items_gen(thread, &timeout_registry,
				       timeout_list) {
			if (thread->next_time < curr_time) {
				thread->next_time = curr_time + thread->timeout;
				pthread_kill(thread->thread, SIGALRM);
			}

			if (thread->next_time < timeout.tv_sec)
				timeout.tv_sec = thread->next_time;
		}

		pthread_cond_timedwait(&timeout_cond, &timeout_mutex, &timeout);
	}

	pthread_cleanup_pop(1);

	return NULL;
}

static int register_for_timeout(struct thread_status *thread)
{
	int ret = 0;

	pthread_mutex_lock(&timeout_mutex);

	thread->next_time = time(NULL) + thread->timeout;

	if (list_empty(&thread->timeout_list)) {
		list_add(&timeout_registry, &thread->timeout_list);
		if (timeout_running)
			pthread_cond_signal(&timeout_cond);
	}

	if (!timeout_running) {
		pthread_t timeout_id;

		if (!(ret = -pthread_create(&timeout_id, NULL,
					    timeout_thread, NULL)))
			timeout_running = 1;
	}

	pthread_mutex_unlock(&timeout_mutex);

	return ret;
}

static void unregister_for_timeout(struct thread_status *thread)
{
	pthread_mutex_lock(&timeout_mutex);
	if (!list_empty(&thread->timeout_list)) {
		list_del(&thread->timeout_list);
		list_init(&thread->timeout_list);
	}
	pthread_mutex_unlock(&timeout_mutex);
}

static void no_intr_log(int level, const char *file, int line,
		       const char *f, ...)
{
	va_list ap;

	if (errno == EINTR)
		return;
	if (level > _LOG_WARN)
		return;

	va_start(ap, f);

	if (level < _LOG_WARN)
		vfprintf(stderr, f, ap);
	else
		vprintf(f, ap);

	va_end(ap);

	if (level < _LOG_WARN)
		fprintf(stderr, "\n");
	else
		fprintf(stdout, "\n");
}

static sigset_t unblock_sigalrm(void)
{
	sigset_t set, old;

	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	pthread_sigmask(SIG_UNBLOCK, &set, &old);
	return old;
}

/* Wait on a device until an event occurs. */
static int event_wait(struct thread_status *thread)
{
	sigset_t set;
	int ret = 0;
/*
	void *next = NULL;
	char *params, *target_type;
	uint64_t start, length;
*/
	struct dm_task *dmt;
	struct dm_info info;

	if (!(dmt = dm_task_create(DM_DEVICE_WAITEVENT)))
		return 0;

	if (!(ret = dm_task_set_name(dmt, dm_basename(thread->device_path))) ||
	    !(ret = dm_task_set_event_nr(dmt, thread->event_nr)))
		goto out;

	/*
	 * This is so that you can break out of waiting on an event,
	 * either for a timeout event, or to cancel the thread.
	 */
	set = unblock_sigalrm();
	dm_log_init(no_intr_log);
	errno = 0;
	if ((ret = dm_task_run(dmt))) {
/*
		do {
			params = NULL;
			next = dm_get_next_target(dmt, next, &start, &length,
						  &target_type, &params);

			log_error("%s: %s\n", __func__, params);
			if ((ret = error_detected(thread, params)))
				break;
		} while(next);
*/
		thread->current_events |= DM_EVENT_DEVICE_ERROR;
		ret = 1;

		/*
		 * FIXME:  I am setting processed_events to zero here
		 * because it is causing problems.  for example, the
		 * mirror target emits a signal for INSYNC, then
		 * subsequent events (device failures) are not handled
		 */
		thread->processed_events = 0;

		if ((ret = dm_task_get_info(dmt, &info)))
			thread->event_nr = info.event_nr;
	} else if (thread->events & DM_EVENT_TIMEOUT && errno == EINTR) {
		thread->current_events |= DM_EVENT_TIMEOUT;
		ret = 1;
		thread->processed_events = 0;
	}

	pthread_sigmask(SIG_SETMASK, &set, NULL);
	dm_log_init(NULL);

   out:
	dm_task_destroy(dmt);

	return ret;
}

/* Register a device with the DSO. */
static int do_register_device(struct thread_status *thread)
{
	return thread->dso_data->register_device(thread->device_path);
}

/* Unregister a device with the DSO. */
static int do_unregister_device(struct thread_status *thread)
{
	return thread->dso_data->unregister_device(thread->device_path);
}

/* Process an event in the DSO. */
static void do_process_event(struct thread_status *thread)
{
	thread->dso_data->process_event(thread->device_path,
					thread->current_events);
}

/* Thread cleanup handler to unregister device. */
static void monitor_unregister(void *arg)
{
	struct thread_status *thread = arg;

	if (!do_unregister_device(thread))
		log_error("%s: %s unregister failed\n", __func__,
			thread->device_path);
}

/* Device monitoring thread. */
static void *monitor_thread(void *arg)
{
	struct thread_status *thread = arg;

	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
	pthread_cleanup_push(monitor_unregister, thread);

	/* Wait for do_process_reques() to finish its task. */
	lock_mutex();
	unlock_mutex();

	/* Loop forever awaiting/analyzing device events. */
	while (1) {
		thread->current_events = 0;

		if (!event_wait(thread))
			continue;

		/*
		 * Check against filter.
		 *
		 * If there's current events delivered from event_wait() AND
		 * the device got registered for those events AND
		 * those events haven't been processed yet, call
		 * the DSO's process_event() handler.
		 */
		if (thread->events &
		    thread->current_events &
		    ~thread->processed_events) {
			lock_mutex();
			thread->processing = 1;
			unlock_mutex();
			do_process_event(thread);
			thread->processed_events |= thread->current_events;
			lock_mutex();
			thread->processing = 0;
			unlock_mutex();
		}
	}

	pthread_cleanup_pop(0);
}

/* Create a device monitoring thread. */
/* FIXME: call this with mutex hold ? */
static int create_thread(struct thread_status *thread)
{
	return pthread_create(&thread->thread, NULL, monitor_thread, thread);
}

static int terminate_thread(struct thread_status *thread)
{
	int ret;

	if ((ret = pthread_cancel(thread->thread)))
		return ret;

	return pthread_kill(thread->thread, SIGALRM);
}

/* DSO reference counting. */
static void lib_get(struct dso_data *data)
{
	data->ref_count++;
}

static void lib_put(struct dso_data *data)
{
	if (!--data->ref_count) {
		dlclose(data->dso_handle);
		UNLINK_DSO(data);
		free_dso_data(data);
	}
}

/* Find DSO data. */
static struct dso_data *lookup_dso(struct message_data *data)
{
	struct dso_data *dso_data, *ret = NULL;

	lock_mutex();

	list_iterate_items(dso_data, &dso_registry) {
		if (!strcmp(data->dso_name, dso_data->dso_name)) {
			lib_get(dso_data);
			ret = dso_data;
			break;
		}
	}

	unlock_mutex();

	return ret;
}

/* Lookup DSO symbols we need. */
static int lookup_symbol(void *dl, struct dso_data *data,
			 void **symbol, const char *name)
{
	if ((*symbol = dlsym(dl, name)))
		return 1;

	log_error("looking up %s symbol in %s\n", name, data->dso_name);

	return 0;
}

static int lookup_symbols(void *dl, struct dso_data *data)
{
	return lookup_symbol(dl, data, (void*) &data->process_event,
			     "process_event") &&
	       lookup_symbol(dl, data, (void*) &data->register_device,
			     "register_device") &&
	       lookup_symbol(dl, data, (void*) &data->unregister_device,
			     "unregister_device");
}

/* Create a DSO file name based on its name. */
static char *create_dso_file_name(char *dso_name)
{
	char *ret;
	static char prefix[] = "libdmeventd";
	static char suffix[] = ".so";

	if ((ret = dbg_malloc(strlen(prefix) +
			      strlen(dso_name) +
			      strlen(suffix) + 1)))
		sprintf(ret, "%s%s%s", prefix, dso_name, suffix);

	return ret;
}

/* Load an application specific DSO. */
static struct dso_data *load_dso(struct message_data *data)
{
	void *dl;
	struct dso_data *ret = NULL;
	char *dso_file;

	if (!(dso_file = create_dso_file_name(data->dso_name)))
		return NULL;

	if (!(dl = dlopen(dso_file, RTLD_NOW))){
		log_error("%s\n", dlerror());
		goto free_dso_file;
	}

	if (!(ret = alloc_dso_data(data)))
		goto close;

	if (!(lookup_symbols(dl, ret)))
		goto free_all;

	/*
	 * Keep handle to close the library once
	 * we've got no references to it any more.
	 */
	ret->dso_handle = dl;
	lib_get(ret);

	lock_mutex();
	LINK_DSO(ret);
	unlock_mutex();

	goto free_dso_file;

   free_all:
	free_dso_data(ret);

   close:
	dlclose(dl);

   free_dso_file:
	dbg_free(dso_file);

	return ret;
}


/* Return success on daemon active check. */
static int active(struct message_data *message_data)
{
	return 0;
}

/*
 * Register for an event.
 *
 * Only one caller at a time here, because we use
 * a FIFO and lock it against multiple accesses.
 */
static int register_for_event(struct message_data *message_data)
{
	int ret = 0;
	struct thread_status *thread, *thread_new = NULL;
	struct dso_data *dso_data;

	if (!device_exists(message_data->device_path)) {
		stack;
		ret = -ENODEV;
		goto out;
	}

	if (!(dso_data = lookup_dso(message_data)) &&
	    !(dso_data = load_dso(message_data))) {
		stack;
/* FIXME */
#ifdef ELIBACC
		ret = -ELIBACC;
#else
		ret = -ENODEV;
#endif
		goto out;
	}
		
	/* Preallocate thread status struct to avoid deadlock. */
	if (!(thread_new = alloc_thread_status(message_data, dso_data))) {
		stack;
		ret = -ENOMEM;
		goto out;
	}

	lock_mutex();

	if (!(thread = lookup_thread_status(message_data))) {
		unlock_mutex();

		/*
		 * FIXME: better do this asynchronously in the
		 *        monitoring thread ?
		 */
		if (!(ret = do_register_device(thread_new)))
			goto out;

		thread = thread_new;
		thread_new = NULL;

		/* Try to create the monitoring thread for this device. */
		lock_mutex();
		if ((ret = -create_thread(thread))) {
			unlock_mutex();
			do_unregister_device(thread);
			free_thread_status(thread);
			goto out;
		} else
			LINK_THREAD(thread);
	}

	/* Or event # into events bitfield. */
	thread->events |= message_data->events.field;

	unlock_mutex(); 

	/* FIXME - If you fail to register for timeout events, you
	   still monitor all the other events. Is this the right
	   action for newly created devices?  Also, you are still
	   on the timeout registry, so if a timeout thread is
	   successfully started up later, you will start receiving
	   DM_EVENT_TIMEOUT events */
	if (thread->events & DM_EVENT_TIMEOUT)
		ret = -register_for_timeout(thread);

   out:
	/*
	 * Deallocate thread status after releasing
	 * the lock in case we haven't used it.
	 */
	if (thread_new)
		free_thread_status(thread_new);

	return ret;
}

/*
 * Unregister for an event.
 *
 * Only one caller at a time here as with register_for_event().
 */
static int unregister_for_event(struct message_data *message_data)
{
	int ret = 0;
	struct thread_status *thread;

	/*
	 * Clear event in bitfield and deactivate
	 * monitoring thread in case bitfield is 0.
	 */
	lock_mutex();

	if (!(thread = lookup_thread_status(message_data))) {
		unlock_mutex();
		ret = -ENODEV;
		goto out;
	}

	thread->events &= ~message_data->events.field;

	if (!(thread->events & DM_EVENT_TIMEOUT))
		unregister_for_timeout(thread);
	/*
	 * In case there's no events to monitor on this device ->
	 * unlink and terminate its monitoring thread.
	 */
	if (!thread->events) {
		UNLINK_THREAD(thread);
		LINK(thread, &thread_registry_unused);
	}
	unlock_mutex();

   out:
	return ret;
}

/*
 * Get registered device.
 *
 * Only one caller at a time here as with register_for_event().
 */
static int registered_device(struct message_data *message_data,
			     struct thread_status *thread)
{
	struct dm_event_daemon_message *msg = message_data->msg;

	snprintf(msg->msg, sizeof(msg->msg), "%s %s %u", 
		 thread->dso_data->dso_name, thread->device_path,
		 thread->events);

	unlock_mutex();

	return 0;
}

static int want_registered_device(char *dso_name, char *device_path,
				  struct thread_status *thread)
{
	/* If DSO names and device paths are equal. */
	if (dso_name && device_path)
		return !strcmp(dso_name, thread->dso_data->dso_name) &&
		       !strcmp(device_path, thread->device_path);

	/* If DSO names are equal. */
	if (dso_name)
		return !strcmp(dso_name, thread->dso_data->dso_name);
		
	/* If device paths are equal. */
	if (device_path)
		return !strcmp(device_path, thread->device_path);

	return 1;
}

static int _get_registered_device(struct message_data *message_data, int next)
{
	int hit = 0;
	struct thread_status *thread;

	lock_mutex();

	/* Iterate list of threads checking if we want a particular one. */
	list_iterate_items(thread, &thread_registry) {
		if ((hit = want_registered_device(message_data->dso_name,
						  message_data->device_path,
						  thread)))
			break;
	}

	/*
	 * If we got a registered device and want the next one ->
	 * fetch next conforming element off the list.
	 */
	if (hit) {
		if (next) {
			do {
				if (list_end(&thread_registry, &thread->list))
					goto out;
				
				thread = list_item(thread->list.n,
						   struct thread_status);
			} while (!want_registered_device(message_data->dso_name,
							 NULL, thread));
		}

		return registered_device(message_data, thread);
	}

   out:
	unlock_mutex();

	return -ENOENT;
}

static int get_registered_device(struct message_data *message_data)
{
	return _get_registered_device(message_data, 0);
}

static int get_next_registered_device(struct message_data *message_data)
{
	return _get_registered_device(message_data, 1);
}

static int set_timeout(struct message_data *message_data)
{
	struct thread_status *thread;

	lock_mutex();
	if ((thread = lookup_thread_status(message_data)))
		thread->timeout = message_data->timeout.secs; 
	unlock_mutex();

	return thread ? 0 : -ENODEV;
}

static int get_timeout(struct message_data *message_data)
{
	struct thread_status *thread;
	struct dm_event_daemon_message *msg = message_data->msg;

	lock_mutex();
	if ((thread = lookup_thread_status(message_data)))
		snprintf(msg->msg, sizeof(msg->msg),
			 "%"PRIu32, thread->timeout);
	unlock_mutex();

	return thread ? 0 : -ENODEV;
}
	

/* Initialize a fifos structure with path names. */
static int init_fifos(struct dm_event_fifos *fifos)
{
	if (memset(fifos, 0, sizeof(*fifos))) {
		fifos->client_path = DM_EVENT_FIFO_CLIENT;
		fifos->server_path = DM_EVENT_FIFO_SERVER;

		return 0;
	}

	return -ENOMEM;
}

/* Open fifos used for client communication. */
static int open_fifos(struct dm_event_fifos *fifos)
{
	/* Blocks until client is ready to write. */
	if ((fifos->server = open(fifos->server_path, O_WRONLY)) < 0) {
		stack;
		return -EXIT_FIFO_FAILURE;
	}

	/* Need to open read+write for select() to work. */
        if ((fifos->client = open(fifos->client_path, O_RDWR)) < 0) {
		stack;
		close(fifos->server);
		return -EXIT_FIFO_FAILURE;
	}

	return 0;
}

/*
 * Read message from client making sure that data is available
 * and a complete message is read.
 */
static int client_read(struct dm_event_fifos *fifos, struct dm_event_daemon_message *msg)
{
	int bytes = 0, ret = 0;
	fd_set fds;

	errno = 0;
	while (bytes < sizeof(*msg) && errno != EOF) {
		do {
			/* Watch client read FIFO for input. */
			FD_ZERO(&fds);
			FD_SET(fifos->client, &fds);
		} while (select(fifos->client+1, &fds, NULL, NULL, NULL) != 1);

		ret = read(fifos->client, msg, sizeof(*msg) - bytes);
		bytes += ret > 0 ? ret : 0;
	}

	return bytes == sizeof(*msg);
}

/*
 * Write a message to the client making sure that it is ready to write.
 */
static int client_write(struct dm_event_fifos *fifos, struct dm_event_daemon_message *msg)
{
	int bytes = 0, ret = 0;
	fd_set fds;

	errno = 0;
	while (bytes < sizeof(*msg) && errno != EIO) {
		do {
			/* Watch client write FIFO to be ready for output. */
			FD_ZERO(&fds);
			FD_SET(fifos->server, &fds);
		} while (select(fifos->server +1, NULL, &fds, NULL, NULL) != 1);

		ret = write(fifos->server, msg, sizeof(*msg) - bytes);
		bytes += ret > 0 ? ret : 0;
	}

	return bytes == sizeof(*msg);
}

/*
 * Handle a client request.
 *
 * We put the request handling functions into
 * a list because of the growing number.
 */
static int handle_request(struct dm_event_daemon_message *msg,
			  struct message_data *message_data)
{
	static struct {
		unsigned int cmd;
		int (*f)(struct message_data*);
	} requests[] = {
		{ DM_EVENT_CMD_REGISTER_FOR_EVENT,         register_for_event },
		{ DM_EVENT_CMD_UNREGISTER_FOR_EVENT,       unregister_for_event },
		{ DM_EVENT_CMD_GET_REGISTERED_DEVICE,      get_registered_device },
		{ DM_EVENT_CMD_GET_NEXT_REGISTERED_DEVICE, get_next_registered_device },
		{ DM_EVENT_CMD_SET_TIMEOUT,                set_timeout },
		{ DM_EVENT_CMD_GET_TIMEOUT,                get_timeout },
		{ DM_EVENT_CMD_ACTIVE,                     active },
	}, *req;

	for (req = requests; req < requests + sizeof(requests); req++) {
		if (req->cmd == msg->opcode.cmd)
			return req->f(message_data);
	}

	return -EINVAL;
}

/* Process a request passed from the communication thread. */
static int do_process_request(struct dm_event_daemon_message *msg)
{
	int ret;
	static struct message_data message_data;

	/* Parse the message. */
	memset(&message_data, 0, sizeof(message_data));
	message_data.msg = msg;
	if (msg->opcode.cmd != DM_EVENT_CMD_ACTIVE &&
	    !parse_message(&message_data)) {
		stack;
		ret = -EINVAL;
	} else {
log_print("%s: %u \"%s\"\n", __func__, msg->opcode.cmd, message_data.msg->msg);
		ret = handle_request(msg, &message_data);
	}

	free_message(&message_data);

	return ret;
}

/* Only one caller at a time. */
static void process_request(struct dm_event_fifos *fifos)
{
	struct dm_event_daemon_message msg;

	/* FIXME: better error handling */

	/* Read the request from the client. */
	if (!memset(&msg, 0, sizeof(msg)) ||
	    !client_read(fifos, &msg)) {
		stack;
		return;
	}

	msg.opcode.status = do_process_request(&msg);

log_print("%s: status: %s\n", __func__, strerror(-msg.opcode.status));
	if (!client_write(fifos, &msg))
		stack;
}

static void cleanup_unused_threads(void)
{
	int ret;
	struct list *l;
	struct thread_status *thread;

	lock_mutex();
	while ((l = list_first(&thread_registry_unused))) {
		thread = list_item(l, struct thread_status);
		if (thread->processing) {
			goto out;  /* cleanup on the next round */
		}
		list_del(l);

		if (!thread->events) {
			/* turn codes negative -- should we be returning this? */
			if ((ret = -terminate_thread(thread)))
				stack;
			else {
				pthread_join(thread->thread, NULL);
				lib_put(thread->dso_data);
				free_thread_status(thread);
			}
		} else {
			log_error("thread can't be on unused list unless !thread->events");
			LINK_THREAD(thread);
		}

	}
out:
	unlock_mutex();
}

static void sig_alarm(int signum)
{
	pthread_testcancel();
}

/* Init thread signal handling. */
static void init_thread_signals(void)
{
	sigset_t sigset;
	struct sigaction act;
	
	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_alarm;
	sigaction(SIGALRM, &act, NULL);
	sigfillset(&sigset);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);
}

static int daemonize(void)
{
	setsid();
	if (chdir("/"))
		return -EXIT_CHDIR_FAILURE;

/* FIXME: activate again after we're done with tracing.
	if ((close(STDIN_FILENO) < 0) ||
	    (close(STDOUT_FILENO) < 0) ||
	    (close(STDERR_FILENO) < 0))
		return -EXIT_DESC_CLOSE_FAILURE;
*/

	return 0;
}

static int lock_pidfile(void)
{
	int lf;
	char pidfile[] = "/var/run/dmeventd.pid";

	if ((lf = open(pidfile, O_CREAT | O_RDWR, 0644)) < 0)
		return -EXIT_OPEN_PID_FAILURE;

	if (flock(lf, LOCK_EX | LOCK_NB) < 0)
		return -EXIT_LOCKFILE_INUSE;

	if (!storepid(lf))
		return -EXIT_FAILURE;

	return 0;
}

void dmeventd(void)
{
	int ret;
	struct dm_event_fifos fifos;
	// struct sys_log logdata = {DAEMON_NAME, LOG_DAEMON};

	if ((ret = daemonize()))
		exit(-ret);

	/* FIXME: set daemon name. */
	// set_name();

	if ((ret = lock_pidfile()))
		exit(-ret);

	init_thread_signals();

	//multilog_clear_logging();
	//multilog_add_type(std_syslog, &logdata);
	//multilog_init_verbose(std_syslog, _LOG_DEBUG);
	//multilog_async(1);

	if ((ret = init_fifos(&fifos)))
		exit(-ret);

	pthread_mutex_init(&mutex, NULL);

#ifdef MCL_CURRENT
	if (mlockall(MCL_CURRENT | MCL_FUTURE) == -1)
		exit(EXIT_FAILURE);
#endif

	if ((ret = open_fifos(&fifos)))
		exit(-ret);

	/* Signal parent, letting them know we are ready to go. */
	kill(getppid(), SIGUSR1);

	/*
	 * We exit when there are no more devices to watch.
	 * That is, when the last unregister happens.
	 */
	do {
		process_request(&fifos);
		cleanup_unused_threads();
	} while(!list_empty(&thread_registry));

	/*
	 * There may still have been some threads that were doing work,
	 * make sure these are cleaned up
	 *
	 * I don't necessarily like the sleep there, but otherwise,
	 * cleanup_unused_threads could get called many many times.
	 * It's worth noting that the likelyhood of it being called
	 * here is slim.
	 */
	while(!list_empty(&thread_registry_unused)) {
		sleep(1);
		cleanup_unused_threads();
	}

	exit_dm_lib();

#ifdef MCL_CURRENT
	munlockall();
#endif
	pthread_mutex_destroy(&mutex);

	exit(EXIT_SUCCESS);
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
