/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 *
 */

#include "log.h"
#include "locking.h"
#include "locking_types.h"
#include "lvm-string.h"
#include "activate.h"

#include <signal.h>

static struct locking_type _locking;

static int _lock_count = 0; 		/* Number of locks held */
static int _signals_ignored = 0;

static void _ignore_signals(void)
{
	int s;

	if (_signals_ignored)
		return;

	for (s = 0; s < NSIG; s++)
		signal(s, SIG_IGN);

	_signals_ignored = 1;

	return;
}

static void _enable_signals(void)
{
	int s;

	/* Don't enable signals while any locks are held */
	if (!_signals_ignored || _lock_count)
		return;

	for (s = 0; s < NSIG; s++)
		signal(s, SIG_DFL);

	_signals_ignored = 0;

	return;
}

static inline void _update_lock_count(int flags)
{
	if ((flags & LCK_TYPE_MASK) == LCK_NONE)
		_lock_count--;
	else
		_lock_count++;
}

/*
 * No locking - currently does nothing.
 */
int no_lock_resource(const char *resource, int flags)
{
	return 1;
}

void no_fin_locking(void)
{
	return;
}

static void _init_no_locking(struct locking_type *locking, 
			     struct config_file *cf)
{
	locking->lock_resource = no_lock_resource;
	locking->fin_locking = no_fin_locking;
}

/*
 * Select a locking type
 */
int init_locking(int type, struct config_file *cf)
{
	switch (type) {
	case 0:
		_init_no_locking(&_locking, cf);
		log_print("WARNING: Locking disabled. Be carefui! "
			  "This could corrupt your metadata.");
		break;
	case 1:
		if (!init_file_locking(&_locking, cf))
			return 0;
		log_very_verbose("File-based locking enabled.");
		break;
/******
	case 2:
		if (!init_other_locking(&_locking, cf))
			return 0;
		log_very_verbose("Other locking enabled.");
		break;
******/
	default:
		log_error("Unknown locking type requested.");
		return 0;
	}

	return 1;
}

void fin_locking(void)
{
	_locking.fin_locking();
}

/*
 * VG locking is by name
 * LV locking is by struct logical_volume
 * FIXME This should take a unique name or id for an LV
 */
int lock_vol(const void *vol, int flags)
{
	char resource[258];

	switch (flags & LCK_SCOPE_MASK) {
	case LCK_VG:
		/*
		 *  Lock a volume group before changing on-disk metadata. 
		 */
		strncpy(resource, (char *) vol, sizeof(resource));
		break;
	case LCK_LV:
		/* 
		 * Suspends LV if it's active. 
		 */
	default:
		log_error("Unrecognised lock scope: %d",
			  flags & LCK_SCOPE_MASK);
		return 0;
	}

	_ignore_signals();

	if (!(_locking.lock_resource(resource, flags))) {
		_enable_signals();
		return 0;
	}

	_update_lock_count(flags);
	_enable_signals();

	return 1;
}

