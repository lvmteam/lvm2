/*
 * Copyright (C) 2008,2009 Red Hat, Inc. All rights reserved.
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
#include "lvm.h"
#include "toolcontext.h"
#include "locking.h"

lvm_t lvm_create(const char *system_dir)
{
	struct cmd_context *cmd;

	/* FIXME: logging bound to handle
	 */

	/* create context */
	/* FIXME: split create_toolcontext */
	cmd = create_toolcontext(1, system_dir);
	if (!cmd)
		return NULL;
	/*
	 * FIXME: if an non memory error occured, return the cmd (maybe some
	 * cleanup needed).
	 */

	/* initialization from lvm_run_command */
	init_error_message_produced(0);

	/* FIXME: locking_type config option needed? */
	/* initialize locking */
	if (!init_locking(-1, cmd)) {
		/* FIXME: use EAGAIN as error code here */
		log_error("Locking initialisation failed.");
		lvm_destroy((lvm_t) cmd);
		return NULL;
	}

	return (lvm_t) cmd;
}

void lvm_destroy(lvm_t libh)
{
	/* FIXME: error handling */
	destroy_toolcontext((struct cmd_context *)libh);
}

int lvm_reload_config(lvm_t libh)
{
	/* FIXME: re-init locking needed here? */
	return refresh_toolcontext((struct cmd_context *)libh);
}
