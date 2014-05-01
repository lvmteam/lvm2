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
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "lvm-signal.h"

#include <signal.h>

static sig_t _oldhandler;
static sigset_t _fullsigset, _intsigset;
static volatile sig_atomic_t _handler_installed;

static sigset_t _oldset;
static int _signals_blocked = 0;
static volatile sig_atomic_t _sigint_caught = 0;
static volatile sig_atomic_t _handler_installed2;
static struct sigaction _oldhandler2;
static int _oldmasked;

void remove_ctrl_c_handler(void)
{
	siginterrupt(SIGINT, 0);
	if (!_handler_installed)
		return;

	_handler_installed = 0;

	sigprocmask(SIG_SETMASK, &_fullsigset, NULL);
	if (signal(SIGINT, _oldhandler) == SIG_ERR)
		log_sys_error("signal", "_remove_ctrl_c_handler");
}

static void _trap_ctrl_c(int sig __attribute__((unused)))
{
	remove_ctrl_c_handler();

	log_error("CTRL-c detected: giving up waiting for lock");
}

void install_ctrl_c_handler(void)
{
	_handler_installed = 1;

	if ((_oldhandler = signal(SIGINT, _trap_ctrl_c)) == SIG_ERR) {
		_handler_installed = 0;
		return;
	}

	sigprocmask(SIG_SETMASK, &_intsigset, NULL);
	siginterrupt(SIGINT, 1);
}

int init_signals(int suppress_messages)
{
	if (sigfillset(&_intsigset) || sigfillset(&_fullsigset)) {
		log_sys_error_suppress(suppress_messages, "sigfillset",
				       "init_signals");
		return 0;
	}

	if (sigdelset(&_intsigset, SIGINT)) {
		log_sys_error_suppress(suppress_messages, "sigdelset",
				       "init_signals");
		return 0;
	}

	return 1;
}

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
	struct sigaction handler;
	sigset_t sigs;

	/*
	 * Do not overwrite the backed-up handler data -
	 * just increase nesting count.
	 */
	if (_handler_installed2) {
		_handler_installed2++;
		return;
	}

	/* Grab old sigaction for SIGINT: shall not fail. */
	sigaction(SIGINT, NULL, &handler);
	handler.sa_flags &= ~SA_RESTART; /* Clear restart flag */
	handler.sa_handler = _catch_sigint;

	_handler_installed2 = 1;

	/* Override the signal handler: shall not fail. */
	sigaction(SIGINT, &handler, &_oldhandler2);

	/* Unmask SIGINT.  Remember to mask it again on restore. */
	sigprocmask(0, NULL, &sigs);
	if ((_oldmasked = sigismember(&sigs, SIGINT))) {
		sigdelset(&sigs, SIGINT);
		sigprocmask(SIG_SETMASK, &sigs, NULL);
	}
}

void sigint_restore(void)
{
	if (!_handler_installed2)
		return;

	if (_handler_installed2 > 1) {
		_handler_installed2--;
		return;
	}

	/* Nesting count went down to 0. */
	_handler_installed2 = 0;

	if (_oldmasked) {
		sigset_t sigs;
		sigprocmask(0, NULL, &sigs);
		sigaddset(&sigs, SIGINT);
		sigprocmask(SIG_SETMASK, &sigs, NULL);
	}

	sigaction(SIGINT, &_oldhandler2, NULL);
}

void block_signals(uint32_t flags __attribute__((unused)))
{
	sigset_t set;

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
	/* Don't unblock signals while any locks are held */
	if (!_signals_blocked)
		return;

	if (sigprocmask(SIG_SETMASK, &_oldset, NULL)) {
		log_sys_error("sigprocmask", "_block_signals");
		return;
	}

	_signals_blocked = 0;
}
