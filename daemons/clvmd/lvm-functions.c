/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "clvmd-common.h"

#include <pthread.h>

#include "lvm-types.h"
#include "clvm.h"
#include "clvmd-comms.h"
#include "clvmd.h"
#include "lvm-functions.h"

/* LVM2 headers */
#include "toolcontext.h"
#include "lvmcache.h"
#include "lvm-globals.h"
#include "activate.h"
#include "archiver.h"
#include "memlock.h"

#include <syslog.h>

static struct cmd_context *cmd = NULL;
static struct dm_hash_table *lv_hash = NULL;
static pthread_mutex_t lv_hash_lock;
static pthread_mutex_t lvm_lock;
static char last_error[1024];

struct lv_info {
	int lock_id;
	int lock_mode;
};

static const char *decode_full_locking_cmd(uint32_t cmdl)
{
	static char buf[128];
	const char *type;
	const char *scope;
	const char *command;

	switch (cmdl & LCK_TYPE_MASK) {
	case LCK_NULL:
		type = "NULL";
		break;
	case LCK_READ:
		type = "READ";
		break;
	case LCK_PREAD:
		type = "PREAD";
		break;
	case LCK_WRITE:
		type = "WRITE";
		break;
	case LCK_EXCL:
		type = "EXCL";
		break;
	case LCK_UNLOCK:
		type = "UNLOCK";
		break;
	default:
		type = "unknown";
		break;
	}

	switch (cmdl & LCK_SCOPE_MASK) {
	case LCK_VG:
		scope = "VG";
		command = "LCK_VG";
		break;
	case LCK_LV:
		scope = "LV";
		switch (cmdl & LCK_MASK) {
		case LCK_LV_EXCLUSIVE & LCK_MASK:
			command = "LCK_LV_EXCLUSIVE";
			break;
		case LCK_LV_SUSPEND & LCK_MASK:
			command = "LCK_LV_SUSPEND";
			break;
		case LCK_LV_RESUME & LCK_MASK:
			command = "LCK_LV_RESUME";
			break;
		case LCK_LV_ACTIVATE & LCK_MASK:
			command = "LCK_LV_ACTIVATE";
			break;
		case LCK_LV_DEACTIVATE & LCK_MASK:
			command = "LCK_LV_DEACTIVATE";
			break;
		default:
			command = "unknown";
			break;
		}
		break;
	default:
		scope = "unknown";
		command = "unknown";
		break;
	}

	sprintf(buf, "0x%x %s (%s|%s%s%s%s%s)", cmdl, command, type, scope,
		cmdl & LCK_NONBLOCK   ? "|NONBLOCK" : "",
		cmdl & LCK_HOLD       ? "|HOLD" : "",
		cmdl & LCK_CLUSTER_VG ? "|CLUSTER_VG" : "",
		cmdl & LCK_CACHE      ? "|CACHE" : "");

	return buf;
}

/*
 * Only processes 8 bits: excludes LCK_CACHE.
 */
static const char *decode_locking_cmd(unsigned char cmdl)
{
	return decode_full_locking_cmd((uint32_t) cmdl);
}

static const char *decode_flags(unsigned char flags)
{
	static char buf[128];
	int len;

	len = sprintf(buf, "0x%x ( %s%s%s%s%s%s%s)", flags,
		flags & LCK_PARTIAL_MODE	  ? "PARTIAL_MODE|" : "",
		flags & LCK_MIRROR_NOSYNC_MODE	  ? "MIRROR_NOSYNC|" : "",
		flags & LCK_DMEVENTD_MONITOR_MODE ? "DMEVENTD_MONITOR|" : "",
		flags & LCK_ORIGIN_ONLY_MODE ? "ORIGIN_ONLY|" : "",
		flags & LCK_TEST_MODE ? "TEST|" : "",
		flags & LCK_CONVERT ? "CONVERT|" : "",
		flags & LCK_DMEVENTD_MONITOR_IGNORE ? "DMEVENTD_MONITOR_IGNORE|" : "");

	if (len > 1)
		buf[len - 2] = ' ';
	else
		buf[0] = '\0';

	return buf;
}

char *get_last_lvm_error(void)
{
	return last_error;
}

/*
 * Hash lock info helpers
 */
static struct lv_info *lookup_info(const char *resource)
{
	struct lv_info *lvi;

	pthread_mutex_lock(&lv_hash_lock);
	lvi = dm_hash_lookup(lv_hash, resource);
	pthread_mutex_unlock(&lv_hash_lock);

	return lvi;
}

static int insert_info(const char *resource, struct lv_info *lvi)
{
	int ret;

	pthread_mutex_lock(&lv_hash_lock);
	ret = dm_hash_insert(lv_hash, resource, lvi);
	pthread_mutex_unlock(&lv_hash_lock);

	return ret;
}

static void remove_info(const char *resource)
{
	pthread_mutex_lock(&lv_hash_lock);
	dm_hash_remove(lv_hash, resource);
	pthread_mutex_unlock(&lv_hash_lock);
}

/*
 * Return the mode a lock is currently held at (or -1 if not held)
 */
static int get_current_lock(char *resource)
{
	struct lv_info *lvi;

	if ((lvi = lookup_info(resource)))
		return lvi->lock_mode;

	return -1;
}


void init_lvhash(void)
{
	/* Create hash table for keeping LV locks & status */
	lv_hash = dm_hash_create(1024);
	pthread_mutex_init(&lv_hash_lock, NULL);
	pthread_mutex_init(&lvm_lock, NULL);
}

/* Called at shutdown to tidy the lockspace */
void destroy_lvhash(void)
{
	struct dm_hash_node *v;
	struct lv_info *lvi;
	char *resource;
	int status;

	pthread_mutex_lock(&lv_hash_lock);

	dm_hash_iterate(v, lv_hash) {
		lvi = dm_hash_get_data(lv_hash, v);
		resource = dm_hash_get_key(lv_hash, v);

		if ((status = sync_unlock(resource, lvi->lock_id)))
			DEBUGLOG("unlock_all. unlock failed(%d): %s\n",
				 status,  strerror(errno));
		free(lvi);
	}

	dm_hash_destroy(lv_hash);
	lv_hash = NULL;

	pthread_mutex_unlock(&lv_hash_lock);
}

/* Gets a real lock and keeps the info in the hash table */
static int hold_lock(char *resource, int mode, int flags)
{
	int status;
	int saved_errno;
	struct lv_info *lvi;

	if (test_mode())
		return 0;

	/* Mask off invalid options */
	flags &= LCKF_NOQUEUE | LCKF_CONVERT;

	lvi = lookup_info(resource);

	if (lvi) {
		if (lvi->lock_mode == mode) {
			DEBUGLOG("hold_lock, lock mode %d already held\n",
				 mode);
			return 0;
		}
		if ((lvi->lock_mode == LCK_EXCL) && (mode == LCK_WRITE)) {
			DEBUGLOG("hold_lock, lock already held LCK_EXCL, "
				 "ignoring LCK_WRITE request\n");
			return 0;
		}
	}

	/* Only allow explicit conversions */
	if (lvi && !(flags & LCKF_CONVERT)) {
		errno = EBUSY;
		return -1;
	}
	if (lvi) {
		/* Already exists - convert it */
		status =
		    sync_lock(resource, mode, flags, &lvi->lock_id);
		saved_errno = errno;
		if (!status)
			lvi->lock_mode = mode;

		if (status) {
			DEBUGLOG("hold_lock. convert to %d failed: %s\n", mode,
				 strerror(errno));
		}
		errno = saved_errno;
	} else {
		lvi = malloc(sizeof(struct lv_info));
		if (!lvi) {
			errno = ENOMEM;
			return -1;
		}

		lvi->lock_mode = mode;
		status = sync_lock(resource, mode, flags & ~LCKF_CONVERT, &lvi->lock_id);
		saved_errno = errno;
		if (status) {
			free(lvi);
			DEBUGLOG("hold_lock. lock at %d failed: %s\n", mode,
				 strerror(errno));
		} else
			if (!insert_info(resource, lvi)) {
				errno = ENOMEM;
				return -1;
			}

		errno = saved_errno;
	}
	return status;
}

/* Unlock and remove it from the hash table */
static int hold_unlock(char *resource)
{
	struct lv_info *lvi;
	int status;
	int saved_errno;

	if (test_mode())
		return 0;

	if (!(lvi = lookup_info(resource))) {
		DEBUGLOG("hold_unlock, lock not already held\n");
		return 0;
	}

	status = sync_unlock(resource, lvi->lock_id);
	saved_errno = errno;
	if (!status) {
		remove_info(resource);
		free(lvi);
	} else {
		DEBUGLOG("hold_unlock. unlock failed(%d): %s\n", status,
			 strerror(errno));
	}

	errno = saved_errno;
	return status;
}

/* Watch the return codes here.
   liblvm API functions return 1(true) for success, 0(false) for failure and don't set errno.
   libdlm API functions return 0 for success, -1 for failure and do set errno.
   These functions here return 0 for success or >0 for failure (where the retcode is errno)
*/

/* Activate LV exclusive or non-exclusive */
static int do_activate_lv(char *resource, unsigned char command, unsigned char lock_flags, int mode)
{
	int oldmode;
	int status;
	int activate_lv;
	int exclusive = 0;
	struct lvinfo lvi;

	/* Is it already open ? */
	oldmode = get_current_lock(resource);
	if (oldmode == mode && (command & LCK_CLUSTER_VG)) {
		DEBUGLOG("do_activate_lv, lock already held at %d\n", oldmode);
		return 0;	/* Nothing to do */
	}

	/* Does the config file want us to activate this LV ? */
	if (!lv_activation_filter(cmd, resource, &activate_lv, NULL))
		return EIO;

	if (!activate_lv)
		return 0;	/* Success, we did nothing! */

	/* Do we need to activate exclusively? */
	if ((activate_lv == 2) || (mode == LCK_EXCL)) {
		exclusive = 1;
		mode = LCK_EXCL;
	}

	/*
	 * Try to get the lock if it's a clustered volume group.
	 * Use lock conversion only if requested, to prevent implicit conversion
	 * of exclusive lock to shared one during activation.
	 */
	if (command & LCK_CLUSTER_VG) {
		status = hold_lock(resource, mode, LCKF_NOQUEUE | (lock_flags & LCK_CONVERT ? LCKF_CONVERT:0));
		if (status) {
			/* Return an LVM-sensible error for this.
			 * Forcing EIO makes the upper level return this text
			 * rather than the strerror text for EAGAIN.
			 */
			if (errno == EAGAIN) {
				sprintf(last_error, "Volume is busy on another node");
				errno = EIO;
			}
			return errno;
		}
	}

	/* If it's suspended then resume it */
	if (!lv_info_by_lvid(cmd, resource, 0, &lvi, 0, 0))
		goto error;

	if (lvi.suspended) {
		critical_section_inc(cmd, "resuming");
		if (!lv_resume(cmd, resource, 0, NULL)) {
			critical_section_dec(cmd, "resumed");
			goto error;
		}
	}

	/* Now activate it */
	if (!lv_activate(cmd, resource, exclusive, NULL))
		goto error;

	return 0;

error:
	if (oldmode == -1 || oldmode != mode)
		(void)hold_unlock(resource);
	return EIO;
}

/* Resume the LV if it was active */
static int do_resume_lv(char *resource, unsigned char command, unsigned char lock_flags)
{
	int oldmode, origin_only, exclusive, revert;

	/* Is it open ? */
	oldmode = get_current_lock(resource);
	if (oldmode == -1 && (command & LCK_CLUSTER_VG)) {
		DEBUGLOG("do_resume_lv, lock not already held\n");
		return 0;	/* We don't need to do anything */
	}
	origin_only = (lock_flags & LCK_ORIGIN_ONLY_MODE) ? 1 : 0;
	exclusive = (oldmode == LCK_EXCL) ? 1 : 0;
	revert = (lock_flags & LCK_REVERT_MODE) ? 1 : 0;

	if (!lv_resume_if_active(cmd, resource, origin_only, exclusive, revert, NULL))
		return EIO;

	return 0;
}

/* Suspend the device if active */
static int do_suspend_lv(char *resource, unsigned char command, unsigned char lock_flags)
{
	int oldmode;
	unsigned origin_only = (lock_flags & LCK_ORIGIN_ONLY_MODE) ? 1 : 0;
	unsigned exclusive;

	/* Is it open ? */
	oldmode = get_current_lock(resource);
	if (oldmode == -1 && (command & LCK_CLUSTER_VG)) {
		DEBUGLOG("do_suspend_lv, lock not already held\n");
		return 0; /* Not active, so it's OK */
	}

	exclusive = (oldmode == LCK_EXCL) ? 1 : 0;

	/* Always call lv_suspend to read commited and precommited data */
	if (!lv_suspend_if_active(cmd, resource, origin_only, exclusive, NULL, NULL))
		return EIO;

	return 0;
}

static int do_deactivate_lv(char *resource, unsigned char command, unsigned char lock_flags)
{
	int oldmode;
	int status;

	/* Is it open ? */
	oldmode = get_current_lock(resource);
	if (oldmode == -1 && (command & LCK_CLUSTER_VG)) {
		DEBUGLOG("do_deactivate_lock, lock not already held\n");
		return 0;	/* We don't need to do anything */
	}

	if (!lv_deactivate(cmd, resource, NULL))
		return EIO;

	if (command & LCK_CLUSTER_VG) {
		status = hold_unlock(resource);
		if (status)
			return errno;
	}

	return 0;
}

const char *do_lock_query(char *resource)
{
	int mode;
	const char *type = NULL;

	mode = get_current_lock(resource);
	switch (mode) {
		case LCK_NULL: type = "NL"; break;
		case LCK_READ: type = "CR"; break;
		case LCK_PREAD:type = "PR"; break;
		case LCK_WRITE:type = "PW"; break;
		case LCK_EXCL: type = "EX"; break;
	}

	DEBUGLOG("do_lock_query: resource '%s', mode %i (%s)\n", resource, mode, type ?: "?");

	return type;
}

/* This is the LOCK_LV part that happens on all nodes in the cluster -
   it is responsible for the interaction with device-mapper and LVM */
int do_lock_lv(unsigned char command, unsigned char lock_flags, char *resource)
{
	int status = 0;

	DEBUGLOG("do_lock_lv: resource '%s', cmd = %s, flags = %s, critical_section = %d\n",
		 resource, decode_locking_cmd(command), decode_flags(lock_flags), critical_section());

	if (!cmd->config_initialized || config_files_changed(cmd)) {
		/* Reinitialise various settings inc. logging, filters */
		if (do_refresh_cache()) {
			log_error("Updated config file invalid. Aborting.");
			return EINVAL;
		}
	}

	pthread_mutex_lock(&lvm_lock);
	if (lock_flags & LCK_MIRROR_NOSYNC_MODE)
		init_mirror_in_sync(1);

	if (lock_flags & LCK_DMEVENTD_MONITOR_IGNORE)
		init_dmeventd_monitor(DMEVENTD_MONITOR_IGNORE);
	else {
		if (lock_flags & LCK_DMEVENTD_MONITOR_MODE)
			init_dmeventd_monitor(1);
		else
			init_dmeventd_monitor(0);
	}

	cmd->partial_activation = (lock_flags & LCK_PARTIAL_MODE) ? 1 : 0;

	/* clvmd should never try to read suspended device */
	init_ignore_suspended_devices(1);

	switch (command & LCK_MASK) {
	case LCK_LV_EXCLUSIVE:
		status = do_activate_lv(resource, command, lock_flags, LCK_EXCL);
		break;

	case LCK_LV_SUSPEND:
		status = do_suspend_lv(resource, command, lock_flags);
		break;

	case LCK_UNLOCK:
	case LCK_LV_RESUME:	/* if active */
		status = do_resume_lv(resource, command, lock_flags);
		break;

	case LCK_LV_ACTIVATE:
		status = do_activate_lv(resource, command, lock_flags, LCK_READ);
		break;

	case LCK_LV_DEACTIVATE:
		status = do_deactivate_lv(resource, command, lock_flags);
		break;

	default:
		DEBUGLOG("Invalid LV command 0x%x\n", command);
		status = EINVAL;
		break;
	}

	if (lock_flags & LCK_MIRROR_NOSYNC_MODE)
		init_mirror_in_sync(0);

	cmd->partial_activation = 0;

	/* clean the pool for another command */
	dm_pool_empty(cmd->mem);
	pthread_mutex_unlock(&lvm_lock);

	DEBUGLOG("Command return is %d, critical_section is %d\n", status, critical_section());
	return status;
}

/* Functions to do on the local node only BEFORE the cluster-wide stuff above happens */
int pre_lock_lv(unsigned char command, unsigned char lock_flags, char *resource)
{
	/* Nearly all the stuff happens cluster-wide. Apart from SUSPEND. Here we get the
	   lock out on this node (because we are the node modifying the metadata)
	   before suspending cluster-wide.
	   LCKF_CONVERT is used always, local node is going to modify metadata
	 */
	if ((command & (LCK_SCOPE_MASK | LCK_TYPE_MASK)) == LCK_LV_SUSPEND &&
	    (command & LCK_CLUSTER_VG)) {
		DEBUGLOG("pre_lock_lv: resource '%s', cmd = %s, flags = %s\n",
			 resource, decode_locking_cmd(command), decode_flags(lock_flags));

		if (hold_lock(resource, LCK_WRITE, LCKF_NOQUEUE | LCKF_CONVERT))
			return errno;
	}
	return 0;
}

/* Functions to do on the local node only AFTER the cluster-wide stuff above happens */
int post_lock_lv(unsigned char command, unsigned char lock_flags,
		 char *resource)
{
	int status;
	unsigned origin_only = (lock_flags & LCK_ORIGIN_ONLY_MODE) ? 1 : 0;

	/* Opposite of above, done on resume after a metadata update */
	if ((command & (LCK_SCOPE_MASK | LCK_TYPE_MASK)) == LCK_LV_RESUME &&
	    (command & LCK_CLUSTER_VG)) {
		int oldmode;

		DEBUGLOG
		    ("post_lock_lv: resource '%s', cmd = %s, flags = %s\n",
		     resource, decode_locking_cmd(command), decode_flags(lock_flags));

		/* If the lock state is PW then restore it to what it was */
		oldmode = get_current_lock(resource);
		if (oldmode == LCK_WRITE) {
			struct lvinfo lvi;

			pthread_mutex_lock(&lvm_lock);
			status = lv_info_by_lvid(cmd, resource, origin_only, &lvi, 0, 0);
			pthread_mutex_unlock(&lvm_lock);
			if (!status)
				return EIO;

			if (lvi.exists) {
				if (hold_lock(resource, LCK_READ, LCKF_CONVERT))
					return errno;
			} else if (hold_unlock(resource))
				return errno;
		}
	}
	return 0;
}

/* Check if a VG is in use by LVM1 so we don't stomp on it */
int do_check_lvm1(const char *vgname)
{
	int status;

	status = check_lvm1_vg_inactive(cmd, vgname);

	return status == 1 ? 0 : EBUSY;
}

int do_refresh_cache(void)
{
	DEBUGLOG("Refreshing context\n");
	log_notice("Refreshing context");

	pthread_mutex_lock(&lvm_lock);

	if (!refresh_toolcontext(cmd)) {
		pthread_mutex_unlock(&lvm_lock);
		return -1;
	}

	init_full_scan_done(0);
	init_ignore_suspended_devices(1);
	lvmcache_label_scan(cmd, 2);
	dm_pool_empty(cmd->mem);

	pthread_mutex_unlock(&lvm_lock);

	return 0;
}

/*
 * Handle VG lock - drop metadata or update lvmcache state
 */
void do_lock_vg(unsigned char command, unsigned char lock_flags, char *resource)
{
	uint32_t lock_cmd = command;
	char *vgname = resource + 2;

	lock_cmd &= (LCK_SCOPE_MASK | LCK_TYPE_MASK | LCK_HOLD);

	/*
	 * Check if LCK_CACHE should be set. All P_ locks except # are cache related.
	 */
	if (strncmp(resource, "P_#", 3) && !strncmp(resource, "P_", 2))
		lock_cmd |= LCK_CACHE;

	DEBUGLOG("do_lock_vg: resource '%s', cmd = %s, flags = %s, critical_section = %d\n",
		 resource, decode_full_locking_cmd(lock_cmd), decode_flags(lock_flags), critical_section());

	/* P_#global causes a full cache refresh */
	if (!strcmp(resource, "P_" VG_GLOBAL)) {
		do_refresh_cache();
		return;
	}

	pthread_mutex_lock(&lvm_lock);
	switch (lock_cmd) {
		case LCK_VG_COMMIT:
			DEBUGLOG("vg_commit notification for VG %s\n", vgname);
			lvmcache_commit_metadata(vgname);
			break;
		case LCK_VG_REVERT:
			DEBUGLOG("vg_revert notification for VG %s\n", vgname);
			lvmcache_drop_metadata(vgname, 1);
			break;
		case LCK_VG_DROP_CACHE:
		default:
			DEBUGLOG("Invalidating cached metadata for VG %s\n", vgname);
			lvmcache_drop_metadata(vgname, 0);
	}
	pthread_mutex_unlock(&lvm_lock);
}

/*
 * Ideally, clvmd should be started before any LVs are active
 * but this may not be the case...
 * I suppose this also comes in handy if clvmd crashes, not that it would!
 */
static int get_initial_state(struct dm_hash_table *excl_uuid)
{
	int lock_mode;
	char lv[64], vg[64], flags[25], vg_flags[25];
	char uuid[65];
	char line[255];
	char *lvs_cmd;
	const char *lvm_binary = getenv("LVM_BINARY") ? : LVM_PATH;
	FILE *lvs;

	if (dm_asprintf(&lvs_cmd, "%s lvs  --config 'log{command_names=0 prefix=\"\"}' "
			"--nolocking --noheadings -o vg_uuid,lv_uuid,lv_attr,vg_attr",
			lvm_binary) < 0)
		return_0;

	/* FIXME: Maybe link and use liblvm2cmd directly instead of fork */
	if (!(lvs = popen(lvs_cmd, "r"))) {
		dm_free(lvs_cmd);
		return 0;
	}

	while (fgets(line, sizeof(line), lvs)) {
	        if (sscanf(line, "%64s %64s %25s %25s\n", vg, lv, flags, vg_flags) == 4) {

			/* States: s:suspended a:active S:dropped snapshot I:invalid snapshot */
		        if (strlen(vg) == 38 &&                         /* is is a valid UUID ? */
			    (flags[4] == 'a' || flags[4] == 's') &&	/* is it active or suspended? */
			    vg_flags[5] == 'c') {			/* is it clustered ? */
				/* Convert hyphen-separated UUIDs into one */
				memcpy(&uuid[0], &vg[0], 6);
				memcpy(&uuid[6], &vg[7], 4);
				memcpy(&uuid[10], &vg[12], 4);
				memcpy(&uuid[14], &vg[17], 4);
				memcpy(&uuid[18], &vg[22], 4);
				memcpy(&uuid[22], &vg[27], 4);
				memcpy(&uuid[26], &vg[32], 6);
				memcpy(&uuid[32], &lv[0], 6);
				memcpy(&uuid[38], &lv[7], 4);
				memcpy(&uuid[42], &lv[12], 4);
				memcpy(&uuid[46], &lv[17], 4);
				memcpy(&uuid[50], &lv[22], 4);
				memcpy(&uuid[54], &lv[27], 4);
				memcpy(&uuid[58], &lv[32], 6);
				uuid[64] = '\0';

				/* Look for this lock in the list of EX locks
				   we were passed on the command-line */
				lock_mode = (dm_hash_lookup(excl_uuid, uuid)) ?
					LCK_EXCL : LCK_READ;

				DEBUGLOG("getting initial lock for %s\n", uuid);
				if (hold_lock(uuid, lock_mode, LCKF_NOQUEUE))
					DEBUGLOG("Failed to hold lock %s\n", uuid);
			}
		}
	}
	if (pclose(lvs))
		DEBUGLOG("lvs pclose failed: %s\n", strerror(errno));

	dm_free(lvs_cmd);

	return 1;
}

static void lvm2_log_fn(int level, const char *file, int line, int dm_errno,
			const char *message)
{

	/* Send messages to the normal LVM2 logging system too,
	   so we get debug output when it's asked for.
 	   We need to NULL the function ptr otherwise it will just call
	   back into here! */
	init_log_fn(NULL);
	print_log(level, file, line, dm_errno, "%s", message);
	init_log_fn(lvm2_log_fn);

	/*
	 * Ignore non-error messages, but store the latest one for returning
	 * to the user.
	 */
	if (level != _LOG_ERR && level != _LOG_FATAL)
		return;

	strncpy(last_error, message, sizeof(last_error));
	last_error[sizeof(last_error)-1] = '\0';
}

/* This checks some basic cluster-LVM configuration stuff */
static void check_config(void)
{
	int locking_type;

	locking_type = find_config_tree_int(cmd, global_locking_type_CFG, NULL);

	if (locking_type == 3) /* compiled-in cluster support */
		return;

	if (locking_type == 2) { /* External library, check name */
		const char *libname;

		libname = find_config_tree_str(cmd, global_locking_library_CFG, NULL);
		if (libname && strstr(libname, "liblvm2clusterlock.so"))
			return;

		log_error("Incorrect LVM locking library specified in lvm.conf, cluster operations may not work.");
		return;
	}
	log_error("locking_type not set correctly in lvm.conf, cluster operations will not work.");
}

/* Backups up the LVM metadata if it's changed */
void lvm_do_backup(const char *vgname)
{
	struct volume_group * vg;
	int consistent = 0;

	DEBUGLOG("Triggering backup of VG metadata for %s.\n", vgname);

	pthread_mutex_lock(&lvm_lock);

	vg = vg_read_internal(cmd, vgname, NULL /*vgid*/, 1, &consistent);

	if (vg && consistent)
		check_current_backup(vg);
	else
		log_error("Error backing up metadata, can't find VG for group %s", vgname);

	release_vg(vg);
	dm_pool_empty(cmd->mem);

	pthread_mutex_unlock(&lvm_lock);
}

struct dm_hash_node *get_next_excl_lock(struct dm_hash_node *v, char **name)
{
	struct lv_info *lvi;

	*name = NULL;
	if (!v)
		v = dm_hash_get_first(lv_hash);

	do {
		if (v) {
			lvi = dm_hash_get_data(lv_hash, v);
			DEBUGLOG("Looking for EX locks. found %x mode %d\n", lvi->lock_id, lvi->lock_mode);

			if (lvi->lock_mode == LCK_EXCL) {
				*name = dm_hash_get_key(lv_hash, v);
			}
			v = dm_hash_get_next(lv_hash, v);
		}
	} while (v && !*name);

	if (*name)
		DEBUGLOG("returning EXclusive UUID %s\n", *name);
	return v;
}

void lvm_do_fs_unlock(void)
{
	pthread_mutex_lock(&lvm_lock);
	DEBUGLOG("Syncing device names\n");
	fs_unlock();
	pthread_mutex_unlock(&lvm_lock);
}

/* Called to initialise the LVM context of the daemon */
int init_clvm(struct dm_hash_table *excl_uuid)
{
	/* Use LOG_DAEMON for syslog messages instead of LOG_USER */
	init_syslog(LOG_DAEMON);
	openlog("clvmd", LOG_PID, LOG_DAEMON);

	/* Initialise already held locks */
	if (!get_initial_state(excl_uuid))
		log_error("Cannot load initial lock states.");

	if (!(cmd = create_toolcontext(1, NULL, 0, 1))) {
		log_error("Failed to allocate command context");
		return 0;
	}

	if (stored_errno()) {
		destroy_toolcontext(cmd);
		return 0;
	}

	cmd->cmd_line = "clvmd";

	/* Check lvm.conf is setup for cluster-LVM */
	check_config();
	init_ignore_suspended_devices(1);

	/* Trap log messages so we can pass them back to the user */
	init_log_fn(lvm2_log_fn);
	memlock_inc_daemon(cmd);

	return 1;
}

void destroy_lvm(void)
{
	if (cmd) {
		memlock_dec_daemon(cmd);
		destroy_toolcontext(cmd);
	}
	cmd = NULL;
}
