/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2014 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib/misc/lib.h"
#include "lib/misc/lvm-signal.h"
#include "lib/mm/memlock.h"

#include <signal.h>

static sigset_t _oldset;
static int _signals_blocked = 0;
static volatile sig_atomic_t _sigint_caught = 0;
static volatile sig_atomic_t _handler_installed = 0;

/* Support 3 level nesting, increase if needed more */
#define MAX_SIGINTS 3

struct ar_sigs {
	int sig;
	const char name[8];
	int oldmasked[MAX_SIGINTS];
	struct sigaction oldhandler[MAX_SIGINTS];
};

/* List of signals we want to allow/restore */
static struct ar_sigs _ar_sigs[] = {
	{ SIGINT, "SIGINT" },
	{ SIGTERM, "SIGTERM" },
};

static void _catch_sigint(int unused __attribute__((unused)))
{
	_sigint_caught = 1;
}

int sigint_caught(void) {
	if (_sigint_caught)
		log_error("Interrupted...");

	return _sigint_caught;
}

void sigint_clear(void)
{
	_sigint_caught = 0;
}

/*
 * Temporarily allow keyboard interrupts to be intercepted and noted;
 * saves interrupt handler state for sigint_restore().  Users should
 * use the sigint_caught() predicate to check whether interrupt was
 * requested and act appropriately.  Interrupt flags are never
 * cleared automatically by this code, but the tools clear the flag
 * before running each command in lvm_run_command().  All other places
 * where the flag needs to be cleared need to call sigint_clear().
 */

void sigint_allow(void)
{
	unsigned i, mask = 0;
	struct sigaction handler;
	sigset_t sigs;

	if (memlock_count_daemon())
		return;
	/*
	 * Do not overwrite the backed-up handler data -
	 * just increase nesting count.
	 */
	if (++_handler_installed > MAX_SIGINTS)
		return;

	/* Unmask signals. Remember to mask it again on restore. */
	if (sigprocmask(0, NULL, &sigs))
		log_sys_debug("sigprocmask", "");

	for (i = 0; i < DM_ARRAY_SIZE(_ar_sigs); ++i) {
		/* Grab old sigaction for SIGNAL: shall not fail. */
		if (sigaction(_ar_sigs[i].sig, NULL, &handler))
			log_sys_debug("sigaction", _ar_sigs[i].name);

		handler.sa_flags &= ~SA_RESTART; /* Clear restart flag */
		handler.sa_handler = _catch_sigint;

		/* Override the signal handler: shall not fail. */
		if (sigaction(_ar_sigs[i].sig, &handler, &_ar_sigs[i].oldhandler[_handler_installed  - 1]))
			log_sys_debug("sigaction", _ar_sigs[i].name);

		if ((_ar_sigs[i].oldmasked[_handler_installed - 1] = sigismember(&sigs, _ar_sigs[i].sig))) {
			sigdelset(&sigs, _ar_sigs[i].sig);
			mask = 1;
		}
	}

	if (mask && sigprocmask(SIG_SETMASK, &sigs, NULL))
		log_sys_debug("sigprocmask", "SIG_SETMASK");
}

void sigint_restore(void)
{
	unsigned i, mask = 0;
	sigset_t sigs;

	if (memlock_count_daemon())
		return;

	if (!_handler_installed ||
	    --_handler_installed >= MAX_SIGINTS)
		return;

	/* Nesting count went below MAX_SIGINTS. */
	sigprocmask(0, NULL, &sigs);
	for (i = 0; i < DM_ARRAY_SIZE(_ar_sigs); ++i)
		if (_ar_sigs[i].oldmasked[_handler_installed]) {
			sigaddset(&sigs, _ar_sigs[i].sig);
			mask = 1;
		}

	if (mask && sigprocmask(SIG_SETMASK, &sigs, NULL))
		log_sys_debug("sigprocmask", "SIG_SETMASK");

	for (i = 0; i < DM_ARRAY_SIZE(_ar_sigs); ++i)
		if (sigaction(_ar_sigs[i].sig, &_ar_sigs[i].oldhandler[_handler_installed], NULL))
			log_sys_debug("sigaction", _ar_sigs[i].name);
}

void block_signals(uint32_t flags __attribute__((unused)))
{
	sigset_t set;

	if (memlock_count_daemon())
		return;

	if (_signals_blocked)
		return;

	if (sigfillset(&set)) {
		log_sys_error("sigfillset", "_block_signals");
		return;
	}

	if (sigprocmask(SIG_SETMASK, &set, &_oldset)) {
		log_sys_error("sigprocmask", "_block_signals");
		return;
	}

	_signals_blocked = 1;
}

void unblock_signals(void)
{
	if (memlock_count_daemon())
		return;

	/* Don't unblock signals while any locks are held */
	if (!_signals_blocked)
		return;

	if (sigprocmask(SIG_SETMASK, &_oldset, NULL)) {
		log_sys_error("sigprocmask", "_block_signals");
		return;
	}

	_signals_blocked = 0;
}

/* usleep with enabled signal handler.
 * Returns 1 when there was interruption */
int interruptible_usleep(useconds_t usec)
{
	int r;

	sigint_allow();
	r = usleep(usec);
	sigint_restore();

	return (sigint_caught() || r) ? 1 : 0;
}
