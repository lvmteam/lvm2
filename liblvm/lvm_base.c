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
#include "toolcontext.h"
#include "locking.h"
#include "lvm-version.h"
#include "metadata-exported.h"
#include "lvm2app.h"

const char *lvm_library_get_version(void)
{
	return LVM_VERSION;
}

lvm_t lvm_init(const char *system_dir)
{
	struct cmd_context *cmd;

	/* FIXME: logging bound to handle
	 */

	if (!udev_init_library_context())
		stack;

	/* create context */
	/* FIXME: split create_toolcontext */
	/* FIXME: make all globals configurable */
	cmd = create_toolcontext(0, system_dir, 1);
	if (!cmd)
		return NULL;

	if (stored_errno())
		return (lvm_t) cmd;

	/*
	 * FIXME: if an non memory error occured, return the cmd (maybe some
	 * cleanup needed).
	 */

	/* initialization from lvm_run_command */
	init_error_message_produced(0);

	/* FIXME: locking_type config option needed? */
	/* initialize locking */
	if (!init_locking(-1, cmd, 0)) {
		/* FIXME: use EAGAIN as error code here */
		lvm_quit((lvm_t) cmd);
		return NULL;
	}
	/*
	 * FIXME: Use cmd->cmd_line as audit trail for liblvm calls.  Used in
	 * archive() call.  Possible example:
	 * cmd_line = "lvm_vg_create: vg1\nlvm_vg_extend vg1 /dev/sda1\n"
	 */
	cmd->cmd_line = "liblvm";

	return (lvm_t) cmd;
}

void lvm_quit(lvm_t libh)
{
	destroy_toolcontext((struct cmd_context *)libh);
	udev_fin_library_context();
}

int lvm_config_reload(lvm_t libh)
{
	/* FIXME: re-init locking needed here? */
	if (!refresh_toolcontext((struct cmd_context *)libh))
		return -1;
	return 0;
}

/*
 * FIXME: submit a patch to document the --config option
 */
int lvm_config_override(lvm_t libh, const char *config_settings)
{
	struct cmd_context *cmd = (struct cmd_context *)libh;
	if (override_config_tree_from_string(cmd, config_settings))
		return -1;
	return 0;
}

int lvm_errno(lvm_t libh)
{
	return stored_errno();
}

const char *lvm_errmsg(lvm_t libh)
{
	return stored_errmsg();
}

const char *lvm_vgname_from_pvid(lvm_t libh, const char *pvid)
{
	struct cmd_context *cmd = (struct cmd_context *)libh;
	struct id id;

	if (!id_read_format(&id, pvid)) {
		log_error(INTERNAL_ERROR "Unable to convert uuid");
		return NULL;
	}
	return find_vgname_from_pvid(cmd, (char *)id.uuid);
}

const char *lvm_vgname_from_device(lvm_t libh, const char *device)
{
	struct cmd_context *cmd = (struct cmd_context *)libh;
	return find_vgname_from_pvname(cmd, device);
}
