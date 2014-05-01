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
