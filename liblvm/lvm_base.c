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
#include "lvm_misc.h"

const char *lvm_library_get_version(void)
{
	return LVM_VERSION;
}

static lvm_t _lvm_init(const char *system_dir)
{
	struct cmd_context *cmd;

	/* FIXME: logging bound to handle
	 */

	if (!udev_init_library_context())
		stack;

	/*
	 * It's not necessary to use name mangling for LVM:
	 *   - the character set used for VG-LV names is subset of udev character set
	 *   - when we check other devices (e.g. device_is_usable fn), we use major:minor, not dm names
	 */
	dm_set_name_mangling_mode(DM_STRING_MANGLING_NONE);

	/* create context */
	/* FIXME: split create_toolcontext */
	/* FIXME: make all globals configurable */
	cmd = create_toolcontext(0, system_dir, 0, 0);
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

	/*
	 * Turn off writing to stdout/stderr.
	 * FIXME Fix lib/ to support a non-interactive mode instead.
	 */
	log_suppress(1);

	return (lvm_t) cmd;
}


lvm_t lvm_init(const char *system_dir)
{
	lvm_t h = NULL;
	struct saved_env e = store_user_env(NULL);
	h = _lvm_init(system_dir);
	restore_user_env(&e);
	return h;
}

void lvm_quit(lvm_t libh)
{
	struct saved_env e = store_user_env((struct cmd_context *)libh);
	fin_locking();
	destroy_toolcontext((struct cmd_context *)libh);
	udev_fin_library_context();
	restore_user_env(&e);
}

int lvm_config_reload(lvm_t libh)
{
	int rc = 0;

	/* FIXME: re-init locking needed here? */
	struct saved_env e = store_user_env((struct cmd_context *)libh);
	if (!refresh_toolcontext((struct cmd_context *)libh))
		rc = -1;
	restore_user_env(&e);
	return rc;
}

/*
 * FIXME: submit a patch to document the --config option
 */
int lvm_config_override(lvm_t libh, const char *config_settings)
{
	int rc = 0;
	struct cmd_context *cmd = (struct cmd_context *)libh;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	if (!override_config_tree_from_string(cmd, config_settings))
		rc = -1;
	restore_user_env(&e);
	return rc;
}

int lvm_config_find_bool(lvm_t libh, const char *config_path, int fail)
{
	int rc = 0;
	struct cmd_context *cmd = (struct cmd_context *)libh;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	rc = dm_config_tree_find_bool(cmd->cft, config_path, fail);
	restore_user_env(&e);
	return rc;
}

int lvm_errno(lvm_t libh)
{
	int rc;
	struct saved_env e = store_user_env((struct cmd_context *)libh);
	rc = stored_errno();
	restore_user_env(&e);
	return rc;
}

const char *lvm_errmsg(lvm_t libh)
{
	const char *rc = NULL;
	struct cmd_context *cmd = (struct cmd_context *)libh;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	const char *msg = stored_errmsg_with_clear();
	if (msg) {
		rc = dm_pool_strdup(cmd->mem, msg);
		free((void *)msg);
	}

	restore_user_env(&e);
	return rc;
}

const char *lvm_vgname_from_pvid(lvm_t libh, const char *pvid)
{
	const char *rc = NULL;
	struct cmd_context *cmd = (struct cmd_context *)libh;
	struct id id;
	struct saved_env e = store_user_env((struct cmd_context *)libh);

	if (id_read_format(&id, pvid)) {
		rc = find_vgname_from_pvid(cmd, (char *)id.uuid);
	} else {
		log_error(INTERNAL_ERROR "Unable to convert uuid");
	}

	restore_user_env(&e);
	return rc;
}

const char *lvm_vgname_from_device(lvm_t libh, const char *device)
{
	const char *rc = NULL;
	struct cmd_context *cmd = (struct cmd_context *)libh;
	struct saved_env e = store_user_env(cmd);
	rc = find_vgname_from_pvname(cmd, device);
	restore_user_env(&e);
	return rc;
}

/*
 * No context to work with, so no ability to save off and restore env is not
 * available and is not needed.
 */
float lvm_percent_to_float(percent_t v)
{
	return dm_percent_to_float(v);
}
