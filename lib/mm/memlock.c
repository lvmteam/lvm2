/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
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
#include "memlock.h"
#include "defaults.h"
#include "config.h"
#include "toolcontext.h"

#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/resource.h>

#ifndef DEVMAPPER_SUPPORT

void memlock_inc_daemon(struct cmd_context *cmd)
{
	return;
}

void memlock_dec_daemon(struct cmd_context *cmd)
{
	return;
}

void critical_section_inc(struct cmd_context *cmd, const char *reason)
{
	return;
}

void critical_section_dec(struct cmd_context *cmd, const char *reason)
{
	return;
}

int critical_section(void)
{
	return 0;
}
void memlock_init(struct cmd_context *cmd)
{
	return;
}

void memlock_unlock(struct cmd_context *cmd)
{
	return;
}

void memlock_reset(void)
{
	return;
}

#else				/* DEVMAPPER_SUPPORT */

static size_t _size_stack;
static size_t _size_malloc_tmp;
static size_t _size_malloc = 2000000;

static void *_malloc_mem = NULL;
static int _mem_locked = 0;
static int _critical_section = 0;
static int _memlock_count_daemon = 0;
static int _priority;
static int _default_priority;

/* list of maps, that are unconditionaly ignored */
static const char * const _ignore_maps[] = {
    "[vdso]",
    "[vsyscall]",
};

/* default blacklist for maps */
static const char * const _blacklist_maps[] = {
    "locale/locale-archive",
    "/LC_MESSAGES/",
    "gconv/gconv-modules.cache",
    "/libreadline.so.",	/* not using readline during mlock */
    "/libncurses.so.",	/* not using readline during mlock */
    "/libtinfo.so.",	/* not using readline during mlock */
    "/libdl-",		/* not using dlopen,dlsym during mlock */
    /* "/libdevmapper-event.so" */
};

typedef enum { LVM_MLOCK, LVM_MUNLOCK } lvmlock_t;

static unsigned _use_mlockall;
static int _maps_fd;
static size_t _maps_len = 8192; /* Initial buffer size for reading /proc/self/maps */
static char *_maps_buffer;
static char _procselfmaps[PATH_MAX] = "";
#define SELF_MAPS "/self/maps"

static size_t _mstats; /* statistic for maps locking */

static void _touch_memory(void *mem, size_t size)
{
	size_t pagesize = lvm_getpagesize();
	char *pos = mem;
	char *end = pos + size - sizeof(long);

	while (pos < end) {
		*(long *) pos = 1;
		pos += pagesize;
	}
}

static void _allocate_memory(void)
{
	void *stack_mem, *temp_malloc_mem;

	if ((stack_mem = alloca(_size_stack)))
		_touch_memory(stack_mem, _size_stack);

	if ((temp_malloc_mem = malloc(_size_malloc_tmp)))
		_touch_memory(temp_malloc_mem, _size_malloc_tmp);

	if ((_malloc_mem = malloc(_size_malloc)))
		_touch_memory(_malloc_mem, _size_malloc);

	free(temp_malloc_mem);
}

static void _release_memory(void)
{
	free(_malloc_mem);
}

/*
 * mlock/munlock memory areas from /proc/self/maps
 * format described in kernel/Documentation/filesystem/proc.txt
 */
static int _maps_line(const struct dm_config_node *cn, lvmlock_t lock,
		      const char *line, size_t *mstats)
{
	const struct dm_config_value *cv;
	long from, to;
	int pos;
	unsigned i;
	char fr, fw, fx, fp;
	size_t sz;
	const char *lock_str = (lock == LVM_MLOCK) ? "mlock" : "munlock";

	if (sscanf(line, "%lx-%lx %c%c%c%c%n",
		   &from, &to, &fr, &fw, &fx, &fp, &pos) != 6) {
		log_error("Failed to parse maps line: %s", line);
		return 0;
	}

	/* Select readable maps */
	if (fr != 'r') {
		log_debug("%s area unreadable %s : Skipping.", lock_str, line);
		return 1;
	}

	/* always ignored areas */
	for (i = 0; i < sizeof(_ignore_maps) / sizeof(_ignore_maps[0]); ++i)
		if (strstr(line + pos, _ignore_maps[i])) {
			log_debug("%s ignore filter '%s' matches '%s': Skipping.",
				  lock_str, _ignore_maps[i], line);
			return 1;
		}

	sz = to - from;
	if (!cn) {
		/* If no blacklist configured, use an internal set */
		for (i = 0; i < sizeof(_blacklist_maps) / sizeof(_blacklist_maps[0]); ++i)
			if (strstr(line + pos, _blacklist_maps[i])) {
				log_debug("%s default filter '%s' matches '%s': Skipping.",
					  lock_str, _blacklist_maps[i], line);
				return 1;
			}
	} else {
		for (cv = cn->v; cv; cv = cv->next) {
			if ((cv->type != DM_CFG_STRING) || !cv->v.str[0])
				continue;
			if (strstr(line + pos, cv->v.str)) {
				log_debug("%s_filter '%s' matches '%s': Skipping.",
					  lock_str, cv->v.str, line);
				return 1;
			}
		}
	}

#ifdef VALGRIND_POOL
	/*
	 * Valgrind is continually eating memory while executing code
	 * so we need to deactivate check of locked memory size
         */
	sz -= sz; /* = 0, but avoids getting warning about dead assigment */

#endif
	*mstats += sz;
	log_debug("%s %10ldKiB %12lx - %12lx %c%c%c%c%s", lock_str,
		  ((long)sz + 1023) / 1024, from, to, fr, fw, fx, fp, line + pos);

	if (lock == LVM_MLOCK) {
		if (mlock((const void*)from, sz) < 0) {
			log_sys_error("mlock", line);
			return 0;
		}
	} else {
		if (munlock((const void*)from, sz) < 0) {
			log_sys_error("munlock", line);
			return 0;
		}
	}

	return 1;
}

static int _memlock_maps(struct cmd_context *cmd, lvmlock_t lock, size_t *mstats)
{
	const struct dm_config_node *cn;
	char *line, *line_end;
	size_t len;
	ssize_t n;
	int ret = 1;

	if (_use_mlockall) {
#ifdef MCL_CURRENT
		if (lock == LVM_MLOCK) {
			if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
				log_sys_error("mlockall", "");
				return 0;
			}
		} else {
			if (munlockall()) {
				log_sys_error("munlockall", "");
				return 0;
			}
		}
		return 1;
#else
		return 0;
#endif
	}

	/* Force libc.mo load */
	if (lock == LVM_MLOCK)
		(void)strerror(0);
	/* Reset statistic counters */
	*mstats = 0;

	/* read mapping into a single memory chunk without reallocation
	 * in the middle of reading maps file */
	for (len = 0;;) {
		if (!_maps_buffer || len >= _maps_len) {
			if (_maps_buffer)
				_maps_len *= 2;
			if (!(_maps_buffer = dm_realloc(_maps_buffer, _maps_len))) {
				log_error("Allocation of maps buffer failed");
				return 0;
			}
		}
		lseek(_maps_fd, 0, SEEK_SET);
		for (len = 0 ; len < _maps_len; len += n) {
			if (!(n = read(_maps_fd, _maps_buffer + len, _maps_len - len))) {
				_maps_buffer[len] = '\0';
				break; /* EOF */
			}
			if (n == -1)
				return_0;
		}
		if (len < _maps_len)  /* fits in buffer */
			break;
	}

	line = _maps_buffer;
	cn = find_config_tree_node(cmd, "activation/mlock_filter");

	while ((line_end = strchr(line, '\n'))) {
		*line_end = '\0'; /* remove \n */
		if (!_maps_line(cn, lock, line, mstats))
			ret = 0;
		line = line_end + 1;
	}

	log_debug("%socked %ld bytes",
		  (lock == LVM_MLOCK) ? "L" : "Unl", (long)*mstats);

	return ret;
}

/* Stop memory getting swapped out */
static void _lock_mem(struct cmd_context *cmd)
{
	_allocate_memory();

	/*
	 * For daemon we need to use mlockall()
	 * so even future adition of thread which may not even use lvm lib
	 * will not block memory locked thread
	 * Note: assuming _memlock_count_daemon is updated before _memlock_count
         */
	_use_mlockall = _memlock_count_daemon ? 1 :
		find_config_tree_bool(cmd, "activation/use_mlockall", DEFAULT_USE_MLOCKALL);

	if (!_use_mlockall) {
		if (!*_procselfmaps &&
		    dm_snprintf(_procselfmaps, sizeof(_procselfmaps),
				"%s" SELF_MAPS, cmd->proc_dir) < 0) {
			log_error("proc_dir too long");
			return;
		}

		if (!(_maps_fd = open(_procselfmaps, O_RDONLY))) {
			log_sys_error("open", _procselfmaps);
			return;
		}
	}

	log_very_verbose("Locking memory");
	if (!_memlock_maps(cmd, LVM_MLOCK, &_mstats))
		stack;

	errno = 0;
	if (((_priority = getpriority(PRIO_PROCESS, 0)) == -1) && errno)
		log_sys_error("getpriority", "");
	else
		if (setpriority(PRIO_PROCESS, 0, _default_priority))
			log_error("setpriority %d failed: %s",
				  _default_priority, strerror(errno));
}

static void _unlock_mem(struct cmd_context *cmd)
{
	size_t unlock_mstats;

	log_very_verbose("Unlocking memory");

	if (!_memlock_maps(cmd, LVM_MUNLOCK, &unlock_mstats))
		stack;

	if (!_use_mlockall) {
		if (close(_maps_fd))
			log_sys_error("close", _procselfmaps);
		dm_free(_maps_buffer);
		_maps_buffer = NULL;
		if (_mstats < unlock_mstats) {
			if ((_mstats + lvm_getpagesize()) < unlock_mstats)
				log_error(INTERNAL_ERROR
					  "Reserved memory (%ld) not enough: used %ld. Increase activation/reserved_memory?",
					  (long)_mstats, (long)unlock_mstats);
			else
				/* FIXME Believed due to incorrect use of yes_no_prompt while locks held */
				log_debug("Suppressed internal error: Maps lock %ld < unlock %ld, a one-page difference.",
					  (long)_mstats, (long)unlock_mstats);
		}
	}

	if (setpriority(PRIO_PROCESS, 0, _priority))
		log_error("setpriority %u failed: %s", _priority,
			  strerror(errno));
	_release_memory();
}

static void _lock_mem_if_needed(struct cmd_context *cmd)
{
	log_debug("Lock:   Memlock counters: locked:%d critical:%d daemon:%d suspended:%d",
		  _mem_locked, _critical_section, _memlock_count_daemon, dm_get_suspended_counter());
	if (!_mem_locked &&
	    ((_critical_section + _memlock_count_daemon) == 1)) {
		_mem_locked = 1;
		_lock_mem(cmd);
	}
}

static void _unlock_mem_if_possible(struct cmd_context *cmd)
{
	log_debug("Unlock: Memlock counters: locked:%d critical:%d daemon:%d suspended:%d",
		  _mem_locked, _critical_section, _memlock_count_daemon, dm_get_suspended_counter());
	if (_mem_locked &&
	    !_critical_section &&
	    !_memlock_count_daemon) {
		_unlock_mem(cmd);
		_mem_locked = 0;
	}
}

void critical_section_inc(struct cmd_context *cmd, const char *reason)
{
	if (!_critical_section) {
		_critical_section = 1;
		log_debug("Entering critical section (%s).", reason);
	}

	_lock_mem_if_needed(cmd);
}

void critical_section_dec(struct cmd_context *cmd, const char *reason)
{
	if (_critical_section && !dm_get_suspended_counter()) {
		_critical_section = 0;
		log_debug("Leaving critical section (%s).", reason);
	}
}

int critical_section(void)
{
	return _critical_section;
}

/*
 * The memlock_*_daemon functions will force the mlockall() call that we need
 * to stay in memory, but they will have no effect on device scans (unlike
 * normal critical_section_inc/dec). Memory is kept locked as long as either
 * of critical_section or memlock_daemon is in effect.
 */

void memlock_inc_daemon(struct cmd_context *cmd)
{
	++_memlock_count_daemon;
	if (_memlock_count_daemon == 1 && _critical_section > 0)
                log_error(INTERNAL_ERROR "_memlock_inc_daemon used in critical section.");
	log_debug("memlock_count_daemon inc to %d", _memlock_count_daemon);
	_lock_mem_if_needed(cmd);
}

void memlock_dec_daemon(struct cmd_context *cmd)
{
	if (!_memlock_count_daemon)
		log_error(INTERNAL_ERROR "_memlock_count_daemon has dropped below 0.");
	--_memlock_count_daemon;
	log_debug("memlock_count_daemon dec to %d", _memlock_count_daemon);
	_unlock_mem_if_possible(cmd);
}

void memlock_init(struct cmd_context *cmd)
{
	/* When threaded, caller already limited stack size so just use the default. */
	_size_stack = 1024 * (cmd->threaded ? DEFAULT_RESERVED_STACK :
					      find_config_tree_int(cmd, "activation/reserved_stack",
								   DEFAULT_RESERVED_STACK));
	_size_malloc_tmp = find_config_tree_int(cmd,
					   "activation/reserved_memory",
					   DEFAULT_RESERVED_MEMORY) * 1024;
	_default_priority = find_config_tree_int(cmd,
					    "activation/process_priority",
					    DEFAULT_PROCESS_PRIORITY);
}

void memlock_reset(void)
{
	log_debug("memlock reset.");
	_mem_locked = 0;
	_critical_section = 0;
	_memlock_count_daemon = 0;
}

void memlock_unlock(struct cmd_context *cmd)
{
	_unlock_mem_if_possible(cmd);
}

#endif
