/*
 * Copyright (C) 2014-2015 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/metadata/metadata.h"
#include "lib/metadata/segtype.h"
#include "lib/activate/activate.h"
#include "lib/locking/lvmlockd.h"
#include "lib/cache/lvmcache.h"
#include "lib/display/display.h"
#include "lib/device/persist.h"
#include "daemons/lvmlockd/lvmlockd-client.h"

#include <mntent.h>
#include <sys/ioctl.h>

static daemon_handle _lvmlockd;
static const char *_lvmlockd_socket = NULL;
static int _use_lvmlockd = 0;         /* is 1 if command is configured to use lvmlockd */
static int _lvmlockd_connected = 0;   /* is 1 if command is connected to lvmlockd */
static int _lvmlockd_init_failed = 0; /* used to suppress further warnings */

struct lvmlockd_pvs {
	char **path;
	int num;
};

struct owner {
	uint32_t host_id;
	uint32_t generation;
	char *name;
};

void lvmlockd_set_socket(const char *sock)
{
	_lvmlockd_socket = sock;
}

/*
 * Set directly from global/use_lvmlockd
 */
void lvmlockd_set_use(int use)
{
	_use_lvmlockd = use;
}

/*
 * Returns the value of global/use_lvmlockd being used by the command.
 */
int lvmlockd_use(void)
{
	return _use_lvmlockd;
}

/*
 * The command continues even if init and/or connect fail,
 * because the command is allowed to use local VGs without lvmlockd,
 * and is allowed to read lockd VGs without locks from lvmlockd.
 */
void lvmlockd_init(struct cmd_context *cmd)
{
	if (!_use_lvmlockd) {
		/* Should never happen, don't call init when not using lvmlockd. */
		log_error("Should not initialize lvmlockd with use_lvmlockd=0.");
	}

	if (!_lvmlockd_socket) {
		log_warn("WARNING: lvmlockd socket location is not configured.");
		_lvmlockd_init_failed = 1;
	}

	if (!!access(LVMLOCKD_PIDFILE, F_OK)) {
		log_warn("WARNING: lvmlockd process is not running.");
		_lvmlockd_init_failed = 1;
	} else {
		_lvmlockd_init_failed = 0;
	}
}

void lvmlockd_connect(void)
{
	if (!_use_lvmlockd) {
		/* Should never happen, don't call connect when not using lvmlockd. */
		log_error("Should not connect to lvmlockd with use_lvmlockd=0.");
	}

	if (_lvmlockd_connected) {
		/* Should never happen, only call connect once. */
		log_error("lvmlockd is already connected.");
	}

	if (_lvmlockd_init_failed)
		return;

	_lvmlockd = lvmlockd_open(_lvmlockd_socket);

	if (_lvmlockd.socket_fd >= 0 && !_lvmlockd.error) {
		log_debug("Successfully connected to lvmlockd on fd %d.", _lvmlockd.socket_fd);
		_lvmlockd_connected = 1;
	} else {
		log_warn("WARNING: lvmlockd connect failed.");
	}
}

void lvmlockd_disconnect(void)
{
	if (_lvmlockd_connected)
		daemon_close(_lvmlockd);
	_lvmlockd_connected = 0;
}

/* Translate the result strings from lvmlockd to bit flags. */
static void _flags_str_to_lockd_flags(const char *flags_str, uint32_t *lockd_flags)
{
	if (strstr(flags_str, "NO_LOCKSPACES"))
		*lockd_flags |= LD_RF_NO_LOCKSPACES;

	if (strstr(flags_str, "NO_GL_LS"))
		*lockd_flags |= LD_RF_NO_GL_LS;

	if (strstr(flags_str, "NO_LM"))
		*lockd_flags |= LD_RF_NO_LM;

	if (strstr(flags_str, "DUP_GL_LS"))
		*lockd_flags |= LD_RF_DUP_GL_LS;

	if (strstr(flags_str, "WARN_GL_REMOVED"))
		*lockd_flags |= LD_RF_WARN_GL_REMOVED;

	if (strstr(flags_str, "SH_EXISTS"))
		*lockd_flags |= LD_RF_SH_EXISTS;
}

static char *_owner_str(struct owner *owner)
{
	static char log_owner_str[128];

	if (!owner || !owner->host_id)
		return (char *)"";

	log_owner_str[0] = '\0';

	/* Use a --lockopt setting to print all owner details? */

	snprintf(log_owner_str, sizeof(log_owner_str)-1, " (host_id %u)", owner->host_id);
	return log_owner_str;
}

/*
 * evaluate the reply from lvmlockd, check for errors, extract
 * the result and lockd_flags returned by lvmlockd.
 * 0 failure (no result/lockd_flags set)
 * 1 success (result/lockd_flags set)
 */

/*
 * This is an arbitrary number that we know lvmlockd
 * will not return.  daemon_reply_int reverts to this
 * value if it finds no result value.
 */
#define NO_LOCKD_RESULT (-1000)

static int _lockd_result(struct cmd_context *cmd, const char *req_name, daemon_reply reply,
			 int *result, uint32_t *lockd_flags, struct owner *owner)
{
	int reply_result;
	const char *str;

	*result = -1;

	if (reply.error) {
		log_error("lockd %s result: reply error %d", req_name, reply.error);
		return 0;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		log_error("lockd %s result: bad response", req_name);
		return 0;
	}

	reply_result = daemon_reply_int(reply, "op_result", NO_LOCKD_RESULT);
	if (reply_result == NO_LOCKD_RESULT) {
		log_error("lockd %s result: no op_result", req_name);
		return 0;
	}

	*result = reply_result;

	if (lockd_flags) {
		if ((str = daemon_reply_str(reply, "result_flags", NULL)))
			_flags_str_to_lockd_flags(str, lockd_flags);
	}

	if (owner) {
		owner->host_id = (uint32_t)daemon_reply_int(reply, "owner_host_id", 0);
		owner->generation = (uint32_t)daemon_reply_int(reply, "owner_generation", 0);
		if ((str = daemon_reply_str(reply, "owner_name", "none")))
			owner->name = dm_pool_strdup(cmd->mem, str);
	}

	log_debug("lockd %s result: %d", req_name, reply_result);
	return 1;
}

static daemon_reply _lockd_send_with_pvs(const char *req_name,
				const struct lvmlockd_pvs *lock_pvs, ...)
{
	daemon_reply repl = { .error = -1 };
	daemon_request req;
	int i;
	char key[32];
	const char *val;
	va_list ap;

	req = daemon_request_make(req_name);

	va_start(ap, lock_pvs);
	daemon_request_extend_v(req, ap);
	va_end(ap);

	/* Pass PV list */
	if (lock_pvs && lock_pvs->num) {
		if (!daemon_request_extend(req, "path_num = " FMTd64,
					   (int64_t)(lock_pvs)->num, NULL)) {
			log_error("Failed to create pvs request.");
			goto bad;
		}
		for (i = 0; i < lock_pvs->num; i++) {
			snprintf(key, sizeof(key), "path[%d] = %%s", i);
			val = lock_pvs->path[i] ? lock_pvs->path[i] : "none";
			if (!daemon_request_extend(req, key, val, NULL)) {
				log_error("Failed to create pvs request.");
				goto bad;
			}
		}
	}

	repl = daemon_send(_lvmlockd, req);
bad:
	daemon_request_destroy(req);

	return repl;
}

#define _lockd_send(req_name, args...)	\
	_lockd_send_with_pvs(req_name, NULL, ##args)

static int _lockd_retrieve_vg_pv_num(struct volume_group *vg)
{
	struct pv_list *pvl;
	int num = 0;

	dm_list_iterate_items(pvl, &vg->pvs)
		num++;

	return num;
}

static void _lockd_free_pv_list(struct lvmlockd_pvs *lock_pvs)
{
	int i;

	for (i = 0; i < lock_pvs->num; i++)
		free(lock_pvs->path[i]);

	free(lock_pvs->path);
	lock_pvs->path = NULL;
	lock_pvs->num = 0;
}

static void _lockd_retrieve_vg_pv_list(struct volume_group *vg,
				      struct lvmlockd_pvs *lock_pvs)
{
	struct pv_list *pvl;
	int pv_num, i;

	memset(lock_pvs, 0x0, sizeof(*lock_pvs));

	pv_num = _lockd_retrieve_vg_pv_num(vg);
	if (!pv_num) {
		log_error("Fail to any PVs for VG %s", vg->name);
		return;
	}

	/* Allocate buffer for PV list */
	lock_pvs->path = zalloc(sizeof(*lock_pvs->path) * pv_num);
	if (!lock_pvs->path) {
		log_error("Fail to allocate PV list for VG %s", vg->name);
		return;
	}

	i = 0;
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!pvl->pv->dev || dm_list_empty(&pvl->pv->dev->aliases))
			continue;
		lock_pvs->path[i] = strdup(pv_dev_name(pvl->pv));
		if (!lock_pvs->path[i]) {
			log_error("Fail to allocate PV path for VG %s", vg->name);
			_lockd_free_pv_list(lock_pvs);
			return;
		}

		log_debug("VG %s find PV device %s", vg->name, lock_pvs->path[i]);
		lock_pvs->num = ++i;
	}
}

static int _lockd_retrieve_lv_pv_num(struct volume_group *vg,
				    const char *lv_name)
{
	struct logical_volume *lv = find_lv(vg, lv_name);
	struct pv_list *pvl;
	int num;

	if (!lv)
		return 0;

	num = 0;
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (lv_is_on_pv(lv, pvl->pv))
			num++;
	}

	return num;
}

static void _lockd_retrieve_lv_pv_list(struct volume_group *vg,
				      const char *lv_name,
				      struct lvmlockd_pvs *lock_pvs)
{
	struct logical_volume *lv = find_lv(vg, lv_name);
	struct pv_list *pvl;
	int pv_num, i = 0;

	memset(lock_pvs, 0x0, sizeof(*lock_pvs));

	/* Cannot find any existed LV? */
	if (!lv)
		return;

	pv_num = _lockd_retrieve_lv_pv_num(vg, lv_name);
	if (!pv_num) {
		/*
		 * Fixup for 'lvcreate --type error -L1 -n $lv1 $vg', in this
		 * case, the drive path list is empty since it doesn't establish
		 * the structure 'pvseg->lvseg->lv->name'.
		 *
		 * So create drive path list with all drives in the VG.
		 */
		log_error("Fail to find any PVs for %s/%s, try to find PVs from VG instead",
			  vg->name, lv_name);
		_lockd_retrieve_vg_pv_list(vg, lock_pvs);
		return;
	}

	/* Allocate buffer for PV list */
	lock_pvs->path = zalloc(sizeof(*lock_pvs->path) * pv_num);
	if (!lock_pvs->path) {
		log_error("Fail to allocate PV list for %s/%s", vg->name, lv_name);
		return;
	}

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (lv_is_on_pv(lv, pvl->pv)) {
			if (!pvl->pv->dev || dm_list_empty(&pvl->pv->dev->aliases))
				continue;
			lock_pvs->path[i] = strdup(pv_dev_name(pvl->pv));
			if (!lock_pvs->path[i]) {
				log_error("Fail to allocate PV path for LV %s/%s",
					  vg->name, lv_name);
				_lockd_free_pv_list(lock_pvs);
				return;
			}

			log_debug("Find PV device %s for LV %s/%s",
				  lock_pvs->path[i], vg->name, lv_name);
			lock_pvs->num = ++i;
		}
	}
}

/*
 * result/lockd_flags are values returned from lvmlockd.
 *
 * return 0 (failure)
 * return 1 (result/lockd_flags indicate success/failure)
 *
 * return 1 result 0   (success)
 * return 1 result < 0 (failure)
 *
 * caller may ignore result < 0 failure depending on
 * lockd_flags and the specific command/mode.
 *
 * When this function returns 0 (failure), no result/lockd_flags
 * were obtained from lvmlockd.
 *
 * When this function returns 1 (success), result/lockd_flags may
 * have been obtained from lvmlockd.  This lvmlockd result may
 * indicate a locking failure.
 */

static int _lockd_request(struct cmd_context *cmd,
		          const char *req_name,
		          const char *vg_name,
		          const char *vg_lock_type,
		          const char *vg_lock_args,
		          const char *lv_name,
		          const char *lv_uuid,
		          const char *lv_lock_args,
		          const char *mode,
		          const char *opts,
			  const struct lvmlockd_pvs *lock_pvs,
		          int *result,
		          uint32_t *lockd_flags,
			  struct owner *owner)
{
	const char *cmd_name = get_cmd_name();
	daemon_reply reply;
	int pid = getpid();

	*result = 0;
	*lockd_flags = 0;

	if (!strcmp(mode, "na"))
		return 1;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	/* cmd and pid are passed for informational and debugging purposes */

	if (!cmd_name || !cmd_name[0])
		cmd_name = "none";

	if (vg_name && lv_name) {
		reply = _lockd_send_with_pvs(req_name,
					lock_pvs,
					"cmd = %s", cmd_name,
					"pid = " FMTd64, (int64_t) pid,
					"mode = %s", mode,
					"opts = %s", opts ?: "none",
					"vg_name = %s", vg_name,
					"lv_name = %s", lv_name,
					"lv_uuid = %s", lv_uuid,
					"vg_lock_type = %s", vg_lock_type ?: "none",
					"vg_lock_args = %s", vg_lock_args ?: "none",
					"lv_lock_args = %s", lv_lock_args ?: "none",
					NULL);

		if (!_lockd_result(cmd, req_name, reply, result, lockd_flags, owner))
			goto fail;

		/*
		log_debug("lockd %s %s vg %s lv %s result %d %x",
			  req_name, mode, vg_name, lv_name, *result, *lockd_flags);
		*/

	} else if (vg_name) {
		reply = _lockd_send_with_pvs(req_name,
					lock_pvs,
					"cmd = %s", cmd_name,
					"pid = " FMTd64, (int64_t) pid,
					"mode = %s", mode,
					"opts = %s", opts ?: "none",
					"vg_name = %s", vg_name,
					"vg_lock_type = %s", vg_lock_type ?: "none",
					"vg_lock_args = %s", vg_lock_args ?: "none",
					NULL);

		if (!_lockd_result(cmd, req_name, reply, result, lockd_flags, owner))
			goto fail;

		/*
		log_debug("lockd %s %s vg %s result %d %x",
			  req_name, mode, vg_name, *result, *lockd_flags);
		*/

	} else {
		reply = _lockd_send_with_pvs(req_name,
					lock_pvs,
					"cmd = %s", cmd_name,
					"pid = " FMTd64, (int64_t) pid,
					"mode = %s", mode,
					"opts = %s", opts ?: "none",
					"vg_lock_type = %s", vg_lock_type ?: "none",
					NULL);

		if (!_lockd_result(cmd, req_name, reply, result, lockd_flags, owner))
			goto fail;

		log_debug("lockd %s %s result %d %x",
			  req_name, mode, *result, *lockd_flags);
	}

	daemon_reply_destroy(reply);

	/* result/lockd_flags have lvmlockd result */
	return 1;

 fail:
	/* no result was obtained from lvmlockd */

	log_error("lvmlockd %s %s failed no result", req_name, mode);

	daemon_reply_destroy(reply);
	return 0;
}

/*
 * Eventually add an option to specify which pv the lvmlock lv should be placed on.
 */

#define ONE_MB_IN_BYTES 1048576

static int _create_sanlock_lv(struct cmd_context *cmd, struct volume_group *vg,
			      const char *lock_lv_name, int num_mb)
{
	uint64_t lv_size_bytes;
	uint32_t extent_bytes;
	uint32_t total_extents;
	struct logical_volume *lv;
	struct lvcreate_params lp = {
		.activate = CHANGE_ALY,
		.alloc = ALLOC_INHERIT,
		.major = -1,
		.minor = -1,
		.permission = LVM_READ | LVM_WRITE,
		.pvh = &vg->pvs,
		.read_ahead = DM_READ_AHEAD_NONE,
		.stripes = 1,
		.vg_name = vg->name,
		.lv_name = lock_lv_name,
		.zero = 1,
	};

	/*
	 * Make the lvmlock lv a multiple of 8 MB, i.e. a multiple of any
	 * sanlock align_size, to avoid having unused space at the end of the
	 * lvmlock LV.
	 */

	if (num_mb % 8)
		num_mb += (8 - (num_mb % 8));

	lv_size_bytes = (uint64_t)num_mb * ONE_MB_IN_BYTES;  /* size of sanlock LV in bytes */
	extent_bytes = vg->extent_size * SECTOR_SIZE; /* size of one extent in bytes */
	total_extents = dm_div_up(lv_size_bytes, extent_bytes); /* number of extents in sanlock LV */
	lp.extents = total_extents;

	lv_size_bytes = (uint64_t)total_extents * extent_bytes;
	num_mb = lv_size_bytes / ONE_MB_IN_BYTES;
	log_debug("Creating lvmlock LV for sanlock with size %um %llub %u extents",
		  num_mb, (unsigned long long)lv_size_bytes, lp.extents);

	dm_list_init(&lp.tags);

	if (!(lp.segtype = get_segtype_from_string(vg->cmd, SEG_TYPE_NAME_STRIPED)))
		return_0;

	lv = lv_create_single(vg, &lp);
	if (!lv) {
		log_error("Failed to create sanlock lv %s in vg %s", lock_lv_name, vg->name);
		return 0;
	}

	vg->sanlock_lv = lv;

	return 1;
}

static int _remove_sanlock_lv(struct cmd_context *cmd, struct volume_group *vg)
{
	if (!lv_remove(vg->sanlock_lv)) {
		log_error("Failed to remove sanlock LV %s/%s", vg->name, vg->sanlock_lv->name);
		return 0;
	}

	log_debug("sanlock lvmlock LV removed");
	return 1;
}

static int _extend_sanlock_lv(struct cmd_context *cmd, struct volume_group *vg, unsigned extend_mb, char *lvmlock_path)
{
	struct device *dev;
	uint64_t old_size_bytes;
	uint64_t new_size_bytes;
	uint32_t extend_bytes;
	uint32_t extend_sectors;
	uint32_t new_size_sectors;
	struct logical_volume *lv = vg->sanlock_lv;
	struct lvresize_params lp = {
		.sign = SIGN_NONE,
		.size = 0,
		.percent = PERCENT_NONE,
		.resize = LV_EXTEND,
		.force = 1,
	};
	uint64_t i;

	extend_bytes = extend_mb * ONE_MB_IN_BYTES;
	extend_sectors = extend_bytes / SECTOR_SIZE;
	new_size_sectors = lv->size + extend_sectors;
	old_size_bytes = lv->size * SECTOR_SIZE;

	log_debug("Extend sanlock LV from %llus (%llu bytes) to %us (%u bytes)",
		  (unsigned long long)lv->size,
		  (unsigned long long)old_size_bytes,
		  (uint32_t)new_size_sectors,
		  (uint32_t)(new_size_sectors * SECTOR_SIZE));

	lp.size = new_size_sectors;
	lp.pvh = &vg->pvs;

	if (!lv_resize(cmd, lv, &lp)) {
		log_error("Extend sanlock LV %s to size %s failed.",
			  display_lvname(lv), display_size(cmd, lp.size));
		return 0;
	}

	if (!lv_refresh_suspend_resume(lv)) {
		log_error("Failed to refresh sanlock LV %s after extend.", display_lvname(lv));
		return 0;
	}

	new_size_bytes = lv->size * SECTOR_SIZE;

	log_debug("Extend sanlock LV zeroing %u bytes from offset %llu to %llu",
		  (uint32_t)(new_size_bytes - old_size_bytes),
		  (unsigned long long)old_size_bytes,
		  (unsigned long long)new_size_bytes);

	log_debug("Zeroing %u MiB on extended internal lvmlock LV...", extend_mb);

	if (!(dev = dev_cache_get(cmd, lvmlock_path, NULL))) {
		log_error("Extend sanlock LV %s cannot find device.", display_lvname(lv));
		return 0;
	}

	if (!label_scan_open(dev)) {
		log_error("Extend sanlock LV %s cannot open device.", display_lvname(lv));
		return 0;
	}

	for (i = 0; i < extend_mb; i++) {
		if (!dev_write_zeros(dev, old_size_bytes + (i * ONE_MB_IN_BYTES), ONE_MB_IN_BYTES)) {
			log_error("Extend sanlock LV %s cannot zero device at " FMTu64 ".",
				  display_lvname(lv), (old_size_bytes + i * ONE_MB_IN_BYTES));
			label_scan_invalidate(dev);
			return 0;
		}
	}

	label_scan_invalidate(dev);
	return 1;
}

/* When one host does _extend_sanlock_lv, the others need to refresh the size. */

static int _refresh_sanlock_lv(struct cmd_context *cmd, struct volume_group *vg)
{
	if (!lv_refresh_suspend_resume(vg->sanlock_lv)) {
		log_error("Failed to refresh %s.", vg->sanlock_lv->name);
		return 0;
	}

	return 1;
}

/*
 * Called at the beginning of lvcreate in a sanlock VG to ensure
 * that there is space in the sanlock LV for a new lock.  If it's
 * full, then this extends it.
 */

static int _handle_sanlock_lv(struct cmd_context *cmd, struct volume_group *vg)
{
	struct logical_volume *lv = vg->sanlock_lv;
	daemon_reply reply;
	char *lvmlock_name;
	char lvmlock_path[PATH_MAX];
	unsigned extend_mb;
	uint64_t lv_size_bytes;
	uint64_t dm_size_bytes;
	int result;
	int ret;
	int fd;

	if (!_use_lvmlockd)
		return 1;
	if (!_lvmlockd_connected)
		return 0;

	if (!lv) {
		log_error("No internal lvmlock LV found.");
		return 0;
	}

	extend_mb = (unsigned) find_config_tree_int(cmd, global_sanlock_lv_extend_CFG, NULL);

	/*
	 * User can choose to not automatically extend the lvmlock LV
	 * so they can manually extend it.
	 */
	if (!extend_mb)
		return 1;

	lv_size_bytes = lv->size * SECTOR_SIZE;

	if (!(lvmlock_name = dm_build_dm_name(cmd->mem, vg->name, lv->name, NULL)))
		return_0;

	if (dm_snprintf(lvmlock_path, sizeof(lvmlock_path), "%s/%s", dm_dir(), lvmlock_name) < 0) {
		log_error("Handle sanlock LV %s path too long.", lvmlock_name);
		return 0;
	}

	fd = open(lvmlock_path, O_RDONLY);
	if (fd < 0) {
		log_error("Cannot open sanlock LV %s.", lvmlock_path);
		return 0;
	}

	if (ioctl(fd, BLKGETSIZE64, &dm_size_bytes) < 0) {
		log_error("Cannot get size of sanlock LV %s.", lvmlock_path);
		if (close(fd))
			stack;
		return 0;
	}

	if (close(fd))
		stack;

	/*
	 * Another host may have extended the lvmlock LV.
	 * If so the lvmlock LV size in metadata will be
	 * larger than our active lvmlock LV, and we need
	 * to refresh our lvmlock LV to use the new space.
	 */
	if (lv_size_bytes > dm_size_bytes) {
		log_debug("Refresh sanlock lv %llu dm %llu",
			  (unsigned long long)lv_size_bytes,
			  (unsigned long long)dm_size_bytes);

		if (!_refresh_sanlock_lv(cmd, vg))
			return 0;
	}

	log_debug("lockd find_free_lock %s", vg->name);

	/*
	 * Ask lvmlockd/sanlock to look for an unused lock.
	 */
	reply = _lockd_send("find_free_lock",
			"pid = " FMTd64, (int64_t) getpid(),
			"vg_name = %s", vg->name,
			"lv_size_bytes = " FMTd64, (int64_t) lv_size_bytes,
			NULL);

	if (!_lockd_result(cmd, "find_free_lock", reply, &result, NULL, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	/* No space on the lvmlock lv for a new lease. */
	if (result == -EMSGSIZE)
		ret = _extend_sanlock_lv(cmd, vg, extend_mb, lvmlock_path);

	daemon_reply_destroy(reply);

	return ret;
}

static int _activate_sanlock_lv(struct cmd_context *cmd, struct volume_group *vg)
{
	if (!activate_lv(cmd, vg->sanlock_lv)) {
		log_error("Failed to activate sanlock lv %s/%s", vg->name, vg->sanlock_lv->name);
		return 0;
	}

	return 1;
}

static int _deactivate_sanlock_lv(struct cmd_context *cmd, struct volume_group *vg)
{
	if (!deactivate_lv(cmd, vg->sanlock_lv)) {
		log_error("Failed to deactivate sanlock lv %s/%s", vg->name, vg->sanlock_lv->name);
		return 0;
	}

	return 1;
}

static int _init_vg(struct cmd_context *cmd, struct volume_group *vg,
		    const char *lock_type)
{
	daemon_reply reply;
	const char *reply_str;
	const char *vg_lock_args = NULL;
	int result;
	int ret;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	reply = _lockd_send("init_vg",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", lock_type,
				NULL);

	if (!_lockd_result(cmd, "init_vg", reply, &result, NULL, NULL)) {
		ret = 0;
		result = -ELOCKD;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	switch (result) {
	case 0:
		break;
	case -ELOCKD:
		log_error("VG %s init failed: lvmlockd not available", vg->name);
		break;
	case -EARGS:
		log_error("VG %s init failed: invalid parameters for dlm", vg->name);
		break;
	case -EMANAGER:
		log_error("VG %s init failed: lock manager %s is not running",
			  vg->name, lock_type);
		break;
	case -EPROTONOSUPPORT:
		log_error("VG %s init failed: lock manager %s is not supported by lvmlockd",
			  vg->name, lock_type);
		break;
	case -EEXIST:
		log_error("VG %s init failed: a lockspace with the same name exists", vg->name);
		break;
	default:
		log_error("VG %s init failed: %d", vg->name, result);
	}

	if (!ret)
		goto out;

	if (!(reply_str = daemon_reply_str(reply, "vg_lock_args", NULL))) {
		log_error("VG %s init failed: lock_args not returned", vg->name);
		ret = 0;
		goto out;
	}

	if (!(vg_lock_args = dm_pool_strdup(cmd->mem, reply_str))) {
		log_error("VG %s init failed: lock_args alloc failed", vg->name);
		ret = 0;
		goto out;
	}

	vg->lock_type = lock_type;
	vg->lock_args = vg_lock_args;

	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("VG %s init failed: vg_write vg_commit", vg->name);
		ret = 0;
		goto out;
	}

	ret = 1;
out:
	daemon_reply_destroy(reply);
	return ret;
}

static int _init_vg_dlm(struct cmd_context *cmd, struct volume_group *vg)
{
	return _init_vg(cmd, vg, "dlm");
}

static int _init_vg_idm(struct cmd_context *cmd, struct volume_group *vg)
{
	return _init_vg(cmd, vg, "idm");
}

static int _init_vg_sanlock(struct cmd_context *cmd, struct volume_group *vg, int lv_lock_count)
{
	daemon_reply reply;
	const char *reply_str;
	const char *vg_lock_args = NULL;
	const char *opts = NULL;
	struct pv_list *pvl;
	uint32_t sector_size = 0;
	uint32_t align_size = 0;
	unsigned int physical_block_size, logical_block_size;
	int host_id;
	int num_mb = 0;
	int result;
	int ret;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	/*
	 * We need the sector size to know what size to create the LV,
	 * but we're not sure what PV the LV will be allocated from, so
	 * just get the sector size of the first PV.
	 */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!dev_get_direct_block_sizes(pvl->pv->dev, &physical_block_size, &logical_block_size))
			continue;
		if ((physical_block_size == 4096) || (logical_block_size == 4096))
			sector_size = 4096;
	}
	if (!sector_size)
		sector_size = 512;

	log_debug("Using sector size %u for sanlock LV", sector_size);

	host_id = find_config_tree_int(cmd, local_host_id_CFG, NULL);

	/*
	 * Starting size of lvmlock LV is 256MB/512MB/1GB depending
	 * on sector_size/align_size, and max valid host_id depends
	 * on sector_size/align_size.
	 */

	if (sector_size == 4096) {
		align_size = find_config_tree_int(cmd, global_sanlock_align_size_CFG, NULL);

		if (align_size == 1) {
			num_mb = 256;
			if (host_id < 1 || host_id > 250) {
				log_error("Invalid host_id %d, use 1-250 (sanlock_align_size is 1MiB).", host_id);
				return 0;
			}
		} else if (align_size == 2) {
			num_mb = 512;
			if (host_id < 1 || host_id > 500) {
				log_error("Invalid host_id %d, use 1-500 (sanlock_align_size is 2MiB).", host_id);
				return 0;
			}
		} else if (align_size == 4) {
			num_mb = 1024;
			if (host_id < 1 || host_id > 1000) {
				log_error("Invalid host_id %d, use 1-1000 (sanlock_align_size is 4MiB).", host_id);
				return 0;
			}
		} else if (align_size == 8) {
			num_mb = 1024;
			if (host_id < 1 || host_id > 2000) {
				log_error("Invalid host_id %d, use 1-2000 (sanlock_align_size is 8MiB).", host_id);
				return 0;
			}
		} else {
			log_error("Invalid sanlock_align_size %u, use 1,2,4,8.", align_size);
			return 0;
		}
	} else if (sector_size == 512) {
		num_mb = 256;
		if (host_id < 1 || host_id > 2000) {
			log_error("Invalid host_id %d, use 1-2000.", host_id);
			return 0;
		}
	} else {
		log_error("Unsupported sector size %u.", sector_size);
		return 0;
	}

	/*
	 * Creating the sanlock LV writes the VG containing the new lvmlock
	 * LV, then activates the lvmlock LV.  The lvmlock LV must be active
	 * before we ask lvmlockd to initialize the VG because sanlock needs
	 * to initialize leases on the lvmlock LV.
	 *
	 * When converting an existing VG to sanlock, the sanlock lv needs to
	 * be large enough to hold leases for all existing lvs needing locks.
	 * One sanlock lease uses 1MB/8MB for 512/4K sector size devices, so
	 * increase the initial size by 1MB/8MB for each existing lv.
	 */

	if (lv_lock_count) {
		if (sector_size == 512)
			num_mb += lv_lock_count;
		else if (sector_size == 4096)
			num_mb += 8 * lv_lock_count;
	}

	if (!_create_sanlock_lv(cmd, vg, LOCKD_SANLOCK_LV_NAME, num_mb)) {
		log_error("Failed to create internal lv.");
		return 0;
	}

	/*
	 * N.B. this passes the sanlock lv name as vg_lock_args
	 * even though it is only part of the final args string
	 * which will be returned from lvmlockd.
	 */

	reply = _lockd_send("init_vg",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", "sanlock",
				"vg_lock_args = %s", vg->sanlock_lv->name,
				"align_mb = " FMTd64, (int64_t) align_size,
				"opts = %s", opts ?: "none",
				NULL);

	if (!_lockd_result(cmd, "init_vg", reply, &result, NULL, NULL)) {
		ret = 0;
		result = -ELOCKD;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	switch (result) {
	case 0:
		break;
	case -ELOCKD:
		log_error("VG %s init failed: lvmlockd not available", vg->name);
		break;
	case -EARGS:
		log_error("VG %s init failed: invalid parameters for sanlock", vg->name);
		break;
	case -EDEVOPEN:
		log_error("VG %s init failed: sanlock cannot open device /dev/mapper/%s-%s", vg->name, vg->name, LOCKD_SANLOCK_LV_NAME);
		log_error("Check that sanlock has permission to access disks.");
		break;
	case -EMANAGER:
		log_error("VG %s init failed: lock manager sanlock is not running", vg->name);
		break;
	case -EPROTONOSUPPORT:
		log_error("VG %s init failed: lock manager sanlock is not supported by lvmlockd", vg->name);
		break;
	case -EMSGSIZE:
		log_error("VG %s init failed: no disk space for leases", vg->name);
		break;
	case -EEXIST:
		log_error("VG %s init failed: a lockspace with the same name exists", vg->name);
		break;
	default:
		log_error("VG %s init failed: %d", vg->name, result);
	}

	if (!ret)
		goto out;

	if (!(reply_str = daemon_reply_str(reply, "vg_lock_args", NULL))) {
		log_error("VG %s init failed: lock_args not returned", vg->name);
		ret = 0;
		goto out;
	}

	if (!(vg_lock_args = dm_pool_strdup(cmd->mem, reply_str))) {
		log_error("VG %s init failed: lock_args alloc failed", vg->name);
		ret = 0;
		goto out;
	}

	lv_set_hidden(vg->sanlock_lv);
	vg->sanlock_lv->status |= LOCKD_SANLOCK_LV;

	vg->lock_type = "sanlock";
	vg->lock_args = vg_lock_args;

	if (!vg_write(vg) || !vg_commit(vg)) {
		log_error("VG %s init failed: vg_write vg_commit", vg->name);
		ret = 0;
		goto out;
	}

	ret = 1;
out:
	if (!ret) {
		/*
		 * The usleep delay gives sanlock time to close the lock lv,
		 * and usually avoids having an annoying error printed.
		 */
		usleep(1000000);
		_deactivate_sanlock_lv(cmd, vg);
		_remove_sanlock_lv(cmd, vg);
		if (!vg_write(vg) || !vg_commit(vg))
			stack;
	}

	daemon_reply_destroy(reply);
	return ret;
}

/* called after vg_remove on disk */

static int _free_vg(struct cmd_context *cmd, struct volume_group *vg)
{
	daemon_reply reply;
	uint32_t lockd_flags = 0;
	int result;
	int ret;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	reply = _lockd_send("free_vg",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", vg->lock_type,
				"vg_lock_args = %s", vg->lock_args,
				NULL);

	if (!_lockd_result(cmd, "free_vg", reply, &result, &lockd_flags, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	if (!ret)
		log_error("%s: lock type %s lvmlockd result %d",
			  __func__, vg->lock_type, result);

	daemon_reply_destroy(reply);

	return 1;
}

static int _free_vg_dlm(struct cmd_context *cmd, struct volume_group *vg)
{
	return _free_vg(cmd, vg);
}

static int _free_vg_idm(struct cmd_context *cmd, struct volume_group *vg)
{
	return _free_vg(cmd, vg);
}

/* called before vg_remove on disk */

int lockd_vg_is_busy(struct cmd_context *cmd, struct volume_group *vg)
{
	daemon_reply reply;
	uint32_t lockd_flags = 0;
	int result;
	int ret;

	/*
	 * Return 1 if the vg is busy, or if an error prevents
	 * determining if it's busy.
	 */

	if (!_use_lvmlockd) {
		log_error("lvmlockd is not in use.");
		return 1;
	}
	if (!_lvmlockd_connected) {
		log_error("lvmlockd is not connected.");
		return 1;
	}

	/*
	 * Check that other hosts do not have the VG lockspace started.
	 */

	log_debug("lockd busy_vg %s", vg->name);

	reply = _lockd_send("busy_vg",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", vg->lock_type,
				"vg_lock_args = %s", vg->lock_args,
				NULL);

	if (!_lockd_result(cmd, "busy_vg", reply, &result, &lockd_flags, NULL)) {
		ret = 1;
		goto out;
	}

	if (result == -EBUSY) {
		log_error("Lockspace for \"%s\" not stopped on other hosts", vg->name);
		ret = 1;
	} else if (result < 0) {
		log_error("Lockspace busy check error %d for \"%s\"", result, vg->name);
		ret = 1;
	} else {
		ret = 0;
	}
out:
	daemon_reply_destroy(reply);
	return ret;
}

/* called before vg_remove on disk */

static int _free_vg_sanlock(struct cmd_context *cmd, struct volume_group *vg)
{
	daemon_reply reply;
	uint32_t lockd_flags = 0;
	int result;
	int ret;

	if (!_use_lvmlockd) {
		log_error("Cannot free VG sanlock, lvmlockd is not in use.");
		return 0;
	}
	if (!_lvmlockd_connected) {
		log_error("Cannot free VG sanlock, lvmlockd is not connected.");
		return 0;
	}

	/*
	 * vgremove originally held the global lock, but lost it because the
	 * vgremove command is removing multiple VGs, and removed the VG
	 * holding the global lock before attempting to remove this VG.
	 * To avoid this situation, the user should remove the VG holding
	 * the global lock in a command by itself, or as the last arg in a
	 * vgremove command that removes multiple VGs.
	 */
	if (cmd->lockd_gl_removed) {
		log_error("Global lock failed: global lock was lost by removing a previous VG.");
		return 0;
	}

	if (!vg->lock_args || !strlen(vg->lock_args)) {
		/* Shouldn't happen in general, but maybe in some error cases? */
		log_debug("_free_vg_sanlock %s no lock_args", vg->name);
		return 1;
	}

	reply = _lockd_send("free_vg",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", vg->lock_type,
				"vg_lock_args = %s", vg->lock_args,
				NULL);

	if (!_lockd_result(cmd, "free_vg", reply, &result, &lockd_flags, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	/*
	 * Other hosts could still be joined to the lockspace, which means they
	 * are using the internal sanlock LV, which means we cannot remove the
	 * VG.  Once other hosts stop using the VG it can be removed.
	 */
	if (result == -EBUSY) {
		log_error("Lockspace for \"%s\" not stopped on other hosts", vg->name);
		goto out;
	} else if (result == -ENOLS) {
		log_error("Lockspace for \"%s\" is not started.", vg->name);
		goto out;
	}

	if (!ret) {
		log_error("_free_vg_sanlock lvmlockd result %d", result);
		goto out;
	}

	/*
	 * If the global lock was been removed by removing this VG, then:
	 *
	 * Print a warning indicating that the global lock should be enabled
	 * in another remaining sanlock VG.
	 *
	 * Do not allow any more VGs to be removed by this command, e.g.
	 * if a command removes two sanlock VGs, like vgremove foo bar,
	 * and the global lock existed in foo, do not continue to remove
	 * VG bar without the global lock.  See the corresponding check above.
	 */
	if (lockd_flags & LD_RF_WARN_GL_REMOVED) {
		log_warn("VG %s held the sanlock global lock, enable global lock in another VG.", vg->name);
		cmd->lockd_gl_removed = 1;
	}

	/*
	 * The usleep delay gives sanlock time to close the lock lv,
	 * and usually avoids having an annoying error printed.
	 */
	usleep(1000000);

	_deactivate_sanlock_lv(cmd, vg);
	_remove_sanlock_lv(cmd, vg);
 out:
	daemon_reply_destroy(reply);

	return ret;
}

/* vgcreate */

int lockd_init_vg(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lock_type, int lv_lock_count)
{
	switch (get_lock_type_from_string(lock_type)) {
	case LOCK_TYPE_NONE:
		return 1;
	case LOCK_TYPE_CLVM:
		return 1;
	case LOCK_TYPE_DLM:
		return _init_vg_dlm(cmd, vg);
	case LOCK_TYPE_SANLOCK:
		return _init_vg_sanlock(cmd, vg, lv_lock_count);
	case LOCK_TYPE_IDM:
		return _init_vg_idm(cmd, vg);
	default:
		log_error("Unknown lock_type.");
		return 0;
	}
}

static int _lockd_all_lvs(struct cmd_context *cmd, struct volume_group *vg)
{
	struct lv_list *lvl;

	dm_list_iterate_items(lvl, &vg->lvs) {
		if (!lockd_lv_uses_lock(lvl->lv))
			continue;

		if (!lockd_lv(cmd, lvl->lv, "ex", 0)) {
			log_error("LV %s/%s must be inactive on all hosts.",
				  vg->name, lvl->lv->name);
			return 0;
		}

		if (!lockd_lv(cmd, lvl->lv, "un", 0)) {
			log_error("Failed to unlock LV %s/%s.", vg->name, lvl->lv->name);
			return 0;
		}
	}

	return 1;
}

/* vgremove before the vg is removed */

int lockd_free_vg_before(struct cmd_context *cmd, struct volume_group *vg,
			 int changing, int yes)
{
	int lock_type_num = get_lock_type_from_string(vg->lock_type);

	if (cmd->lockopt & LOCKOPT_FORCE) {
		if (!yes && yes_no_prompt("Force unprotected removal of shared VG? [y/n]: ") == 'n') {
			log_error("VG not removed.");
			return 0;
		}
		if (vg->sanlock_lv) {
			_deactivate_sanlock_lv(cmd, vg);
			_remove_sanlock_lv(cmd, vg);
		}
		return 1;
	}

	/*
	 * Check that no LVs are active on other hosts.
	 * When removing (not changing), each LV is locked
	 * when it is removed, they do not need checking here.
	 */
	if (lock_type_num == LOCK_TYPE_DLM || lock_type_num == LOCK_TYPE_SANLOCK ||
	    lock_type_num == LOCK_TYPE_IDM) {
		if (changing && !_lockd_all_lvs(cmd, vg)) {
			log_error("Cannot change VG %s with active LVs", vg->name);
			return 0;
		}
	}

	switch (lock_type_num) {
	case LOCK_TYPE_NONE:
		/*
		 * If a sanlock VG was forcibly changed to none,
		 * the sanlock_lv may have been left behind.
		 */
		if (vg->sanlock_lv)
			_remove_sanlock_lv(cmd, vg);
		return 1;
	case LOCK_TYPE_CLVM:
		return 1;
	case LOCK_TYPE_DLM:
	case LOCK_TYPE_IDM:
		/* returning an error will prevent vg_remove() */
		if (lockd_vg_is_busy(cmd, vg))
			return 0;
		return 1;
	case LOCK_TYPE_SANLOCK:
		/* returning an error will prevent vg_remove() */
		return _free_vg_sanlock(cmd, vg);
	default:
		log_error("Unknown lock_type.");
		return 0;
	}
}

/* vgremove after the vg is removed */

void lockd_free_vg_final(struct cmd_context *cmd, struct volume_group *vg)
{
	switch (get_lock_type_from_string(vg->lock_type)) {
	case LOCK_TYPE_NONE:
	case LOCK_TYPE_CLVM:
		break;
	case LOCK_TYPE_SANLOCK:
		persist_key_file_remove(cmd, vg);
		break;
	case LOCK_TYPE_DLM:
		_free_vg_dlm(cmd, vg);
		break;
	case LOCK_TYPE_IDM:
		_free_vg_idm(cmd, vg);
		break;
	default:
		log_error("Unknown lock_type.");
	}
}

/*
 * Starting a vg involves:
 * 1. reading the vg without a lock
 * 2. getting the lock_type/lock_args from the vg metadata
 * 3. doing start_vg in lvmlockd for the lock_type;
 *    this means joining the lockspace
 *
 * The vg read in step 1 should not be used for anything
 * other than getting the lock_type/lock_args/uuid necessary
 * for starting the lockspace.  To use the vg after starting
 * the lockspace, follow the standard method which is:
 * lock the vg, read/use/write the vg, unlock the vg.
 */

int lockd_start_vg(struct cmd_context *cmd, struct volume_group *vg, int *exists)
{
	char uuid[64] __attribute__((aligned(8)));
	const char *opts = NULL;
	char opt_buf[64] = {};
	daemon_reply reply;
	uint32_t lockd_flags = 0;
	int host_id = 0;
	int result;
	int ret;
	struct lvmlockd_pvs lock_pvs;
	const char *lock_type = vg->lock_type ?: "empty";

	memset(uuid, 0, sizeof(uuid));

	if (!vg_is_shared(vg))
		return 1;

	if (!_use_lvmlockd) {
		log_error("VG %s start failed: lvmlockd is not enabled", vg->name);
		return 0;
	}
	if (!_lvmlockd_connected) {
		log_error("VG %s start failed: lvmlockd is not running", vg->name);
		return 0;
	}

	if ((cmd->lockopt & LOCKOPT_NODELAY) ||
	    (cmd->lockopt & LOCKOPT_ADOPTLS) ||
	    (cmd->lockopt & LOCKOPT_ADOPT) ||
	    (cmd->lockopt & LOCKOPT_REPAIRVG) ||
	    (cmd->lockopt & LOCKOPT_REPAIR)) {
		if (dm_snprintf(opt_buf, sizeof(opt_buf), "%s%s%s%s",
				(cmd->lockopt & LOCKOPT_NODELAY) ? "nodelay," : "",
				(cmd->lockopt & LOCKOPT_ADOPTLS) ? "adopt_only," : "",
				(cmd->lockopt & LOCKOPT_ADOPT) ? "adopt," : "",
				(cmd->lockopt & (LOCKOPT_REPAIR|LOCKOPT_REPAIRVG)) ? "repair" : "") < 0) {
			log_error("Options string too long %x", cmd->lockopt);
			return 0;
		}
		opts = opt_buf;
	}

	log_debug("lockd start VG %s lock_type %s", vg->name, lock_type);

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	if (!strcmp(lock_type, "sanlock")) {
		if (!vg->sanlock_lv) {
			log_error("Missing internal lvmlock LV for sanlock.");
			return 0;
		}
		/*
		 * This is the big difference between starting
		 * sanlock vgs vs starting dlm vgs: the internal
		 * sanlock lv needs to be activated before lvmlockd
		 * does the start because sanlock needs to use the lv
		 * to access locks.
		 */
		if (!_activate_sanlock_lv(cmd, vg))
			return 0;

		host_id = find_config_tree_int(cmd, local_host_id_CFG, NULL);
	}

	/*
	 * Create the VG's PV list when start the VG, the PV list
	 * is passed to lvmlockd, and the the PVs path will be used
	 * to send SCSI commands for idm locking scheme.
	 */
	if (!strcmp(lock_type, "idm")) {
		_lockd_retrieve_vg_pv_list(vg, &lock_pvs);
		reply = _lockd_send_with_pvs("start_vg",
				&lock_pvs,
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", lock_type,
				"vg_lock_args = %s", vg->lock_args ?: "none",
				"vg_uuid = %s", uuid[0] ? uuid : "none",
				"version = " FMTd64, (int64_t) vg->seqno,
				"host_id = " FMTd64, (int64_t) host_id,
				"opts = %s", opts ?:  "none",
				NULL);
		_lockd_free_pv_list(&lock_pvs);
	} else {
		reply = _lockd_send_with_pvs("start_vg",
				NULL,
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", lock_type,
				"vg_lock_args = %s", vg->lock_args ?: "none",
				"vg_uuid = %s", uuid[0] ? uuid : "none",
				"version = " FMTd64, (int64_t) vg->seqno,
				"host_id = " FMTd64, (int64_t) host_id,
				"opts = %s", opts ?:  "none",
				NULL);
	}

	if (!_lockd_result(cmd, "start_vg", reply, &result, &lockd_flags, NULL)) {
		ret = 0;
		result = -ELOCKD;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	if (lockd_flags & LD_RF_WARN_GL_REMOVED)
		cmd->lockd_gl_removed = 1;

	switch (result) {
	case 0:
		log_print_unless_silent("VG %s starting %s lockspace", vg->name, lock_type);
		ret = 1;
		break;
	case -ELOCKD:
		log_error("VG %s start failed: lvmlockd not available", vg->name);
		break;
	case -EEXIST:
		log_debug("VG %s start error: already started", vg->name);
		ret = 1;
		break;
	case -ESTARTING:
		log_debug("VG %s start error: already starting", vg->name);
		if (exists)
			*exists = 1;
		ret = 1;
		break;
	case -EARGS:
		log_error("VG %s start failed: invalid parameters for %s", vg->name, lock_type);
		break;
	case -EHOSTID:
		log_error("VG %s start failed: invalid sanlock host_id, set in lvmlocal.conf", vg->name);
		break;
	case -EMANAGER:
		log_error("VG %s start failed: lock manager %s is not running", vg->name, lock_type);
		break;
	case -EPROTONOSUPPORT:
		log_error("VG %s start failed: lock manager %s is not supported by lvmlockd", vg->name, lock_type);
		break;
	case -ELOCKREPAIR: 
		log_error("VG %s start failed: sanlock lease needs repair", vg->name);
		break;
	default:
		log_error("VG %s start failed: %d", vg->name, result);
	}

	if (!ret && !strcmp(lock_type, "sanlock")) {
		log_debug("lockd_starg_vg result %d deactivate sanlock lv", result);
		if (!_deactivate_sanlock_lv(cmd, vg))
			log_error("Failed to deactivate internal lvmlock LV for sanlock.");
	}

	/*
	 * Update persistent reservation key with the correct
	 * host generation number (from sanlock) if necessary.
	 * lockstart is async, so when lvmlockd processes the
	 * start_vg, it can read the previous generation number
	 * and return that, knowing that the generation number
	 * used in the in-progress start will be +1.  We want
	 * to use the current generation number in the PR key.
	 */
	if (!result && !strcmp(lock_type, "sanlock")) {
		uint32_t prev_gen = (uint32_t)daemon_reply_int(reply, "prev_generation", 0);
		log_debug("lockd start update pr key with prev_gen %u", prev_gen);
		if (!persist_key_update(cmd, vg, prev_gen)) {
			lockd_stop_vg(cmd, vg);
			ret = 0;
		}
	}

	daemon_reply_destroy(reply);

	return ret;
}

int lockd_stop_vg(struct cmd_context *cmd, struct volume_group *vg)
{
	daemon_reply reply;
	int result;
	int ret;

	if (!vg_is_shared(vg))
		return 1;
	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	log_debug("lockd stop VG %s lock_type %s",
		  vg->name, vg->lock_type ? vg->lock_type : "empty");

	reply = _lockd_send("stop_vg",
			"pid = " FMTd64, (int64_t) getpid(),
			"vg_name = %s", vg->name,
			NULL);

	if (!_lockd_result(cmd, "stop_vg", reply, &result, NULL, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	if (result == -ENOLS) {
		ret = 1;
		goto out;
	}

	if (result == -EBUSY) {
		log_error("VG %s stop failed: LVs must first be deactivated", vg->name);
		goto out;
	}

	if (!ret) {
		log_error("VG %s stop failed: %d", vg->name, result);
		goto out;
	}

	if (!strcmp(vg->lock_type, "sanlock")) {
		log_debug("lockd_stop_vg deactivate sanlock lv");
		_deactivate_sanlock_lv(cmd, vg);
	}
out:
	daemon_reply_destroy(reply);

	return ret;
}

int lockd_start_wait(struct cmd_context *cmd)
{
	daemon_reply reply;
	int result;
	int ret;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	reply = _lockd_send("start_wait",
			"pid = " FMTd64, (int64_t) getpid(),
			NULL);

	if (!_lockd_result(cmd, "start_wait", reply, &result, NULL, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	if (!ret)
		log_error("Lock start failed");

	/*
	 * FIXME: get a list of vgs that started so we can
	 * better report what worked and what didn't?
	 */

	daemon_reply_destroy(reply);

	if (cmd->lockd_gl_removed) {
		log_error("Missing global lock: global lock was lost by removing a previous VG.");
		log_error("To enable the global lock in another VG, see lvmlockctl --gl-enable.");
	}

	return ret;
}

/*
 * lockd_gl_create() is a variation of lockd_gl() used only by vgcreate.
 * It handles the case that when using sanlock, the global lock does
 * not exist until after the first vgcreate is complete, since the global
 * lock exists on storage within an actual VG.  So, the first vgcreate
 * needs special logic to detect this bootstrap case.
 *
 * When the vgcreate is not creating the first VG, then lockd_gl_create()
 * behaves the same as lockd_gl().
 *
 * vgcreate will have a lock_type for the new VG which lockd_gl_create()
 * can provide in the lock-gl call.
 *
 * lockd_gl() and lockd_gl_create() differ in the specific cases where
 * ENOLS (no lockspace found) is overridden.  In the vgcreate case, the
 * override cases are related to sanlock bootstrap, and the lock_type of
 * the vg being created is needed.
 *
 * 1. vgcreate of the first lockd-type vg calls lockd_gl_create()
 *    to acquire the global lock.
 *
 * 2. vgcreate/lockd_gl_create passes gl lock request to lvmlockd,
 *    along with lock_type of the new vg.
 *
 * 3. lvmlockd finds no global lockspace/lock.
 *
 * 4. dlm:
 *    If the lock_type from vgcreate is dlm, lvmlockd creates the
 *    dlm global lockspace, and queues the global lock request
 *    for vgcreate.  lockd_gl_create returns success with the gl held.
 *
 *    sanlock:
 *    If the lock_type from vgcreate is sanlock, lvmlockd returns -ENOLS
 *    with the NO_GL_LS flag.  lvmlockd cannot create or acquire a sanlock
 *    global lock until the VG exists on disk (the locks live within the VG).
 *
 *    lockd_gl_create sees sanlock/ENOLS/NO_GL_LS (and optionally the
 *    "enable" lock-gl arg), determines that this is the sanlock
 *    bootstrap special case, and returns success without the global lock.
 *
 *    vgcreate creates the VG on disk, and calls lockd_init_vg() which
 *    initializes/enables a global lock on the new VG's internal sanlock lv.
 *    Future lockd_gl/lockd_gl_create calls will acquire the existing gl.
 */

int lockd_global_create(struct cmd_context *cmd, const char *def_mode, const char *vg_lock_type)
{
	struct owner owner = { 0 };
	const char *mode = NULL;
	uint32_t lockd_flags;
	int retries = 0;
	int result;

	/*
	 * There are four variations of creating a local/lockd VG
	 * with/without use_lvmlockd set.
	 *
	 * use_lvmlockd=1, lockd VG:
	 * This function should acquire or create the global lock.
	 *
	 * use_lvmlockd=0, local VG:
	 * This function is a no-op, just returns 1.
	 *
	 * use_lvmlockd=0, lockd VG
	 * An error is returned in vgcreate_params_set_from_args (before this is called).
	 *
	 * use_lvmlockd=1, local VG
	 * This function should acquire the global lock.
	 */
	if (!_use_lvmlockd) {
		if (!is_lockd_type(vg_lock_type))
			return 1;
		log_error("Cannot create VG with lock_type %s without lvmlockd.", vg_lock_type);
		return 0;
	}

	if (cmd->lockd_gl_disable) {
		log_debug("lockd global create disabled %s", def_mode ?: "");
		if (def_mode && !strcmp(def_mode, "ex"))
			log_warn("WARNING: Skipping global lock in lvmlockd.");
		goto out;
	}

	log_debug("lockd global create lock_type %s", vg_lock_type);

	if (!mode)
		mode = def_mode;
	if (!mode) {
		log_error("Unknown lock-gl mode");
		return 0;
	}

 req:
	if (!_lockd_request(cmd, "lock_gl",
			      NULL, vg_lock_type, NULL, NULL, NULL, NULL, mode, NULL,
			      NULL, &result, &lockd_flags, &owner)) {
		/* No result from lvmlockd, it is probably not running. */
		log_error("Global lock failed: check that lvmlockd is running.");
		return 0;
	}

	if (result == -EAGAIN || result == -EIOTIMEOUT) {
		if (retries < find_config_tree_int(cmd, global_lvmlockd_lock_retries_CFG, NULL)) {
			if (result == -EIOTIMEOUT)
				log_warn("Retrying global lock: io timeout");
			else
				log_warn("Retrying global lock: held by other host%s", _owner_str(&owner));
			sleep(1);
			retries++;
			goto req;
		}
	}

	/*
	 * ENOLS: no lockspace was found with a global lock.
	 * It may not exist (perhaps this command is creating the first),
	 * or it may not be visible or started on the system yet.
	 */

	if (result == -ENOLS) {
		if (!strcmp(mode, "un"))
			return 1;

		/*
		 * This is the sanlock bootstrap condition for proceeding
		 * without the global lock: a chicken/egg case for the first
		 * sanlock VG that is created.  When creating the first
		 * sanlock VG, there is no global lock to acquire because
		 * the gl will exist in the VG being created.  So, we
		 * skip acquiring the global lock when creating this initial
		 * VG, and enable the global lock in this VG.
		 *
		 * This initial bootstrap condition is identified based on
		 * two things:
		 *
		 * 1. No sanlock VGs have been started in lvmlockd, causing
		 *    lvmlockd to return NO_GL_LS/NO_LOCKSPACES.
		 *
		 * 2. No sanlock VGs are seen in lvmcache after the disk
		 *    scan performed.
		 *
		 * If both of those are true, we go ahead and create this new
		 * VG which will have the global lock enabled.  However, this
		 * has a shortcoming: another sanlock VG may exist that hasn't
		 * appeared to the system yet.  If that VG has its global lock
		 * enabled, then when it appears later, duplicate global locks
		 * will be seen, and a warning will indicate that one of them
		 * should be disabled.
		 *
		 * The two bootstrap conditions have another shortcoming to the
		 * opposite effect:  other sanlock VGs may be visible to the
		 * system, but none of them have a global lock enabled.
		 * In that case, it would make sense to create this new VG with
		 * an enabled global lock.  (FIXME: we could detect that none
		 * of the existing sanlock VGs have a gl enabled and allow this
		 * vgcreate to go ahead.)  Enabling the global lock in one of
		 * the existing sanlock VGs is currently the simplest solution.
		 */

		if ((lockd_flags & LD_RF_NO_GL_LS) &&
		    (lockd_flags & LD_RF_NO_LOCKSPACES) &&
		    !strcmp(vg_lock_type, "sanlock")) {
			if (lvmcache_contains_lock_type_sanlock(cmd)) {
				/* FIXME: we could check that all are started, and then check that none have gl enabled. */
				log_error("Global lock failed: start existing sanlock VGs to access global lock.");
				log_error("(If all sanlock VGs are started, enable global lock with lvmlockctl.)");
				return 0;
			}
			log_print_unless_silent("Enabling sanlock global lock");
			return 1;
		}

		if (!strcmp(vg_lock_type, "sanlock"))
			log_error("Global lock failed: check that VG holding global lock exists and is started.");
		else
			log_error("Global lock failed: check that global lockspace is started.");

		if (lockd_flags & LD_RF_NO_LM)
			log_error("Start a lock manager, lvmlockd did not find one running.");
		return 0;
	}

	/*
	 * Check for each specific error that can be returned so a helpful
	 * message can be printed for it.
	 */
	if (result < 0) {
		if (result == -ESTARTING)
			log_error("Global lock failed: lockspace is starting.");
		else if (result == -EIOTIMEOUT)
			log_error("Global lock failed: io timeout");
		else if (result == -ELOCKREPAIR)
			log_error("Global lock failed: sanlock lease needs repair");
		else if (result == -EAGAIN)
			log_error("Global lock failed: held by other host%s", _owner_str(&owner));
		else if (result == -EPROTONOSUPPORT)
			log_error("VG create failed: lock manager %s is not supported by lvmlockd.", vg_lock_type);
		else
			log_error("Global lock failed: error %d", result);
		return 0;
	}

out:

	/* --shared with vgcreate does not mean include_shared_vgs */
	cmd->include_shared_vgs = 0;

	/*
	 * This is done to prevent converting an explicitly acquired
	 * ex lock to sh in process_each.
	 */
	cmd->lockd_global_ex = 1;

	return 1;
}

/*
 * The global lock protects:
 *
 * - The global VG namespace.  Two VGs cannot have the same name.
 *   Used by any command that creates or removes a VG name,
 *   e.g. vgcreate, vgremove, vgrename, vgsplit, vgmerge.
 *
 * - The set of orphan PVs.
 *   Used by any command that changes a non-PV device into an orphan PV,
 *   an orphan PV into a device, a non-orphan PV (in a VG) into an orphan PV
 *   (not in a VG), or an orphan PV into a non-orphan PV,
 *   e.g. pvcreate, pvremove, vgcreate, vgremove, vgextend, vgreduce.
 *
 * - The properties of orphan PVs.  It is possible to make changes to the
 *   properties of an orphan PV, e.g. pvresize, pvchange.
 *
 * These are things that cannot be protected by a VG lock alone, since
 * orphan PVs do not belong to a real VG (an artificial VG does not
 * apply since a sanlock lock only exists on real storage.)
 *
 * If a command will change any of the things above, it must first acquire
 * the global lock in exclusive mode.
 *
 * If command is reading any of the things above, it must acquire the global
 * lock in shared mode.  A number of commands read the things above, including:
 *
 * - Reporting/display commands which show all VGs.  Any command that
 *   will iterate through the entire VG namespace must first acquire the
 *   global lock shared so that it has an accurate view of the namespace.
 *
 * - A command where a tag name is used to identify what to process.
 *   A tag requires reading all VGs to check if they match the tag.
 *
 * In these cases, the global lock must be acquired before the list of
 * all VGs is created.
 *
 * The global lock is not generally unlocked explicitly in the code.
 * When the command disconnects from lvmlockd, lvmlockd automatically
 * releases the locks held by the command.  The exception is if a command
 * will continue running for a long time while not needing the global lock,
 * e.g. commands that poll to report progress.
 *
 * There are two cases where the global lock can be taken in shared mode,
 * and then later converted to ex.  pvchange and pvresize use process_each_pv
 * which does lockd_gl("sh") to get the list of VGs.  Later, in the "_single"
 * function called within process_each_pv, the PV may be an orphan, in which
 * case the ex global lock is needed, so it's converted to ex at that point.
 *
 * Effects of misconfiguring use_lvmlockd.
 *
 * - Setting use_lvmlockd=1 tells lvm commands to use the global lock.
 * This should not be set unless a lock manager and lockd VGs will
 * be used.  Setting use_lvmlockd=1 without setting up a lock manager
 * or using lockd VGs will cause lvm commands to fail when they attempt
 * to change any global state (requiring the ex global lock), and will
 * cause warnings when the commands read global state (requiring the sh
 * global lock).  In this condition, lvm is nominally useful, and existing
 * local VGs can continue to be used mostly as usual.  But, the
 * warnings/errors should lead a user to either set up a lock manager
 * and lockd VGs, or set use_lvmlockd to 0.
 *
 * - Setting use_lvmlockd=0 tells lvm commands to not use the global lock.
 * If use_lvmlockd=0 when lockd VGs exist which require lvmlockd, the
 * lockd_gl() calls become no-ops, but the lockd_vg() calls for the lockd
 * VGs will fail.  The warnings/errors from accessing the lockd VGs
 * should lead the user to set use_lvmlockd to 1 and run the necessary
 * lock manager.  In this condition, lvm reverts to the behavior of
 * the following case, in which system ID largely protects shared
 * devices, but has limitations.
 *
 * - Setting use_lvmlockd=0 with shared devices, no lockd VGs and
 * no lock manager is a recognized mode of operation that is
 * described in the lvmsystemid man page.  Using lvm on shared
 * devices this way is made safe by using system IDs to assign
 * ownership of VGs to single hosts.  The main limitation of this
 * mode (among others outlined in the man page), is that orphan PVs
 * are unprotected.
 */

int lockd_global(struct cmd_context *cmd, const char *def_mode)
{
	struct owner owner = { 0 };
	char opt_buf[64] = {};
	const char *mode = NULL;
	const char *opts = NULL;
	uint32_t lockd_flags;
	int retries = 0;
	int result;

	if (!_use_lvmlockd)
		return 1;

	/*
	 * Verify that when --readonly is used, no ex locks should be used.
	 */
	if (cmd->metadata_read_only && def_mode && !strcmp(def_mode, "ex")) {
		log_error("Exclusive locks are not allowed with readonly option.");
		return 0;
	}

	if (def_mode && !strcmp(def_mode, "un")) {
		mode = "un";
		goto req;
	}

	if (!mode)
		mode = def_mode;
	if (!mode) {
		log_error("Unknown lvmlockd global lock mode");
		return 0;
	}

	if ((cmd->lockopt & LOCKOPT_ADOPTGL) ||
	    (cmd->lockopt & LOCKOPT_ADOPT) ||
	    (cmd->lockopt & LOCKOPT_REPAIRGL) ||
	    (cmd->lockopt & LOCKOPT_REPAIR)) {
		if (dm_snprintf(opt_buf, sizeof(opt_buf), "%s%s%s",
			    (cmd->lockopt & LOCKOPT_ADOPTGL) ? "adopt_only" : "",
			    (cmd->lockopt & LOCKOPT_ADOPT) ? "adopt" : "",
			    (cmd->lockopt & (LOCKOPT_REPAIR|LOCKOPT_REPAIRGL)) ? "repair" : "") < 0) {
			log_error("Options string too long %x", cmd->lockopt);
			return 0;
		}
		opts = opt_buf;
	}

	if (!strcmp(mode, "sh") && cmd->lockd_global_ex)
		return 1;

	if (!strcmp(mode, "un") && cmd->lockd_global_ex)
		cmd->lockd_global_ex = 0;

	if (cmd->lockd_gl_disable) {
		log_debug("lockd global disabled %s", def_mode ?: "");
		if (def_mode && !strcmp(def_mode, "ex"))
			log_warn("WARNING: Skipping global lock in lvmlockd.");
		goto allow;
	}
 req:
	log_debug("lockd global %s", mode);

	if (!_lockd_request(cmd, "lock_gl",
			    NULL, NULL, NULL, NULL, NULL, NULL, mode, opts,
			    NULL, &result, &lockd_flags, &owner)) {
		/* No result from lvmlockd, it is probably not running. */

		/* We don't care if an unlock fails. */
		if (!strcmp(mode, "un"))
			return 1;

		/* We can continue reading if a shared lock fails. */
		if (!strcmp(mode, "sh")) {
			log_warn("Reading without shared global lock.");
			goto allow;
		}

		log_error("Global lock failed: check that lvmlockd is running.");
		return 0;
	}

	if (result == -EAGAIN || result == -EIOTIMEOUT) {
		if (retries < find_config_tree_int(cmd, global_lvmlockd_lock_retries_CFG, NULL)) {
			if (result == -EIOTIMEOUT)
				log_warn("Retrying global lock: io timeout");
                        else
				log_warn("Retrying global lock: held by other host%s", _owner_str(&owner));
			sleep(1);
			retries++;
			goto req;
		}
	}

	if (result == -EALREADY) {
		/*
		 * This should generally not happen because commands should be coded
		 * to avoid reacquiring the global lock.  If there is a case that's
		 * missed which causes the command to request the gl when it's already
		 * held, it's not a problem, so let it go.
		 */
		log_debug("lockd global %s already held.", mode);
		return 1;
	}

	if (!strcmp(mode, "un"))
		return 1;

	/*
	 * ENOLS: no lockspace was found with a global lock.
	 * The VG with the global lock may not be visible or started yet,
	 * this should be a temporary condition.
	 *
	 * ESTARTING: the lockspace with the gl is starting.
	 * The VG with the global lock is starting and should finish shortly.
	 *
	 * ELOCKIO: sanlock gets i/o errors when trying to read/write leases
	 * (This can progress to EVGKILLED.)
	 *
	 * EVGKILLED: the sanlock lockspace is being killed after losing
	 * access to lease storage.
	 */

	if (result == -ENOLS && (lockd_flags & LD_RF_NO_LM))
		log_error("Start a lock manager, lvmlockd did not find one running.");

	if (result == -ENOLS ||
	    result == -ESTARTING ||
	    result == -EVGKILLED ||
	    result == -ELOCKIO ||
	    result == -EIOTIMEOUT ||
	    result == -ELOCKREPAIR ||
	    result == -ELMERR ||
	    result == -EORPHAN ||
	    result == -EADOPT_RETRY ||
	    result == -EADOPT_NONE ||
	    result == -EAGAIN) {
		/*
		 * If an ex global lock fails, then the command fails.
		 */
		if (strcmp(mode, "sh")) {
			if (result == -ESTARTING)
				log_error("Global lock failed: lockspace is starting");
			else if (result == -ENOLS)
				log_error("Global lock failed: check that global lockspace is started");
			else if (result == -ELOCKIO)
				log_error("Global lock failed: storage errors for sanlock leases");
			else if (result == -EIOTIMEOUT)
				log_error("Global lock failed: io timeout");
			else if (result == -ELOCKREPAIR)
				log_error("Global lock failed: sanlock lease needs repair");
			else if (result == -ELMERR)
				log_error("Global lock failed: lock manager error");
			else if (result == -EVGKILLED)
				log_error("Global lock failed: storage failed for sanlock leases");
			else if (result == -EORPHAN)
				log_error("Global lock failed: orphan lock needs to be adopted");
			else if (result == -EADOPT_NONE)
				log_error("Global lock failed: adopt found no orphan");
			else if (result == -EADOPT_RETRY)
				log_error("Global lock failed: adopt found other mode");
			else if (result == -EAGAIN)
				log_error("Global lock failed: held by other host%s", _owner_str(&owner));
			else
				log_error("Global lock failed: error %d", result);

			return 0;
		}

		/*
		 * If a sh global lock fails, then the command can continue
		 * reading without it, but force a global cache validation,
		 * and print a warning.
		 */

		if (result == -ESTARTING) {
			log_warn("Skipping global lock: lockspace is starting");
			goto allow;
		}

		if (result == -ELOCKIO || result == -EVGKILLED) {
			log_warn("Skipping global lock: storage %s for sanlock leases",
				  result == -ELOCKIO ? "errors" : "failed");
			goto allow;
		}

		if (result == -EIOTIMEOUT) {
			log_warn("Skipping global lock: io timeout");
			goto allow;
		}

		if (result == -ELOCKREPAIR) {
			log_warn("Skipping global lock: sanlock lease needs repair");
			goto allow;
		}

		if ((lockd_flags & LD_RF_NO_GL_LS) && (lockd_flags & LD_RF_WARN_GL_REMOVED)) {
			log_warn("Skipping global lock: VG with global lock was removed");
			goto allow;
		}

		if (result == -EORPHAN) {
			log_warn("Skipping global lock: orphan lock needs to be adopted");
			goto allow;
		}

		if (result == -EADOPT_NONE) {
			log_warn("Skipping global lock: adopt found no orphan");
			goto allow;
		}

		if (result == -EADOPT_RETRY) {
			log_warn("Skipping global lock: adopt found other mode");
			goto allow;
		}

		if (result == -ELMERR) {
			log_warn("Skipping global lock: lock manager error");
			goto allow;
		}

		if (result == -EAGAIN) {
			log_warn("Skipping global lock: held by other host%s", _owner_str(&owner));
			goto allow;
		}

		if ((lockd_flags & LD_RF_NO_GL_LS) || (lockd_flags & LD_RF_NO_LOCKSPACES)) {
			log_debug("Skipping global lock: lockspace not found or started");
			goto allow;
		}

		/*
		 * This is for completeness.  If we reach here, then
		 * a specific check for the error should be added above
		 * with a more helpful message.
		 */
		log_error("Global lock failed: error %d", result);
		return 0;
	}

	if ((lockd_flags & LD_RF_DUP_GL_LS) && strcmp(mode, "un"))
		log_warn("Duplicate sanlock global locks should be corrected");

	if (result < 0) {
		/*
		 * We don't intend to reach this.  We should check
		 * any known/possible error specifically and print
		 * a more helpful message.  This is for completeness.
		 */
		log_error("Global lock failed: error %d.", result);
		return 0;
	}

 allow:

	/*
	 * This is done to prevent converting an explicitly acquired
	 * ex lock to sh in process_each.
	 */
	if (!strcmp(mode, "ex"))
		cmd->lockd_global_ex = 1;

	return 1;
}

/*
 * VG lock
 *
 * Return 1: continue, lockd_state may still indicate an error
 * Return 0: failure, do not continue
 *
 * lvmlockd could also return the lock_type that it used for the VG,
 * and we could encode that in lockd_state, and verify later that it
 * matches vg->lock_type.
 *
 * The result of the VG lock operation needs to be saved in lockd_state
 * because the result needs to be passed into vg_read so it can be
 * assessed in combination with vg->lock_type.
 *
 * The VG lock protects the VG metadata on disk from concurrent access
 * among hosts.
 *
 * The VG lock must be acquired before the VG is read, i.e. before vg_read().
 * The result from lockd_vg() is saved in the "lockd_state" variable, and
 * this result is passed into vg_read().  After vg_read() reads the VG,
 * it checks if the VG lock_type (sanlock or dlm) requires a lock to be
 * held, and if so, it verifies that the lock was correctly acquired by
 * looking at lockd_state.
 *
 * If vg_read() sees that the VG is a local VG, i.e. lock_type is not
 * sanlock or dlm, then no lock is required, and it ignores lockd_state,
 * which would indicate no lock was found.... although a newer
 * optimization avoids calling lockd_vg() at all for local VGs
 * by checking the lock_type in lvmcache saved by label_scan.  In extremely
 * rare case where the lock_type changes between label_scan and vg_read,
 * the caller will go back and repeat lockd_vg()+vg_read().
 */

int lockd_vg(struct cmd_context *cmd, const char *vg_name, const char *def_mode,
	     uint32_t flags, uint32_t *lockd_state)
{
	struct owner owner = { 0 };
	char opt_buf[64] = {};
	const char *mode = NULL;
	const char *opts = NULL;
	uint32_t lockd_flags;
	uint32_t prev_state = *lockd_state;
	int retries = 0;
	int result;
	int ret = 1;

	/*
	 * The result of the VG lock request is saved in lockd_state to be
	 * passed into vg_read where the lock result is needed once we
	 * know if this is a local VG or lockd VG.
	 */
	*lockd_state = 0;

	if (!is_real_vg(vg_name))
		return 1;

	/*
	 * Verify that when --readonly is used, no ex locks should be used.
	 */
	if (cmd->metadata_read_only &&
	    ((def_mode && !strcmp(def_mode, "ex")) ||
	     (!def_mode && !cmd->lockd_vg_default_sh))) {
		log_error("Exclusive locks are not allowed with readonly option.");
		return 0;
	}

	/*
	 * Some special cases need to disable the vg lock.
	 */
	if (cmd->lockd_vg_disable) {
		log_debug("lockd VG disabled %s", def_mode ?: "");
		if (def_mode && !strcmp(def_mode, "ex"))
			log_warn("WARNING: Skipping VG lock in lvmlockd.");
		return 1;
	}

	/*
	 * An unlock is simply sent or skipped without any need
	 * for the mode checking for sh/ex.
	 *
	 * Look at lockd_state from the sh/ex lock, and if it failed,
	 * don't bother sending the unlock to lvmlockd.  The main
	 * purpose of this is to avoid sending an unnecessary unlock
	 * for local VGs (the lockd_state from sh/ex on the local VG
	 * will be failed.)  This implies that the lockd_state value
	 * should be preserved from the sh/ex lockd_vg() call and
	 * passed back to lockd_vg() for the corresponding unlock.
	 */
	if (def_mode && !strcmp(def_mode, "un")) {
		if (prev_state & LDST_FAIL)
			return 1;

		mode = "un";
		goto req;
	}

	/*
	 * The default mode may not have been provided in the
	 * function args.  This happens when lockd_vg is called
	 * from a process_each function that handles different
	 * commands.  Commands that only read/check/report/display
	 * the vg have LOCKD_VG_SH set in commands.h, which is
	 * copied to lockd_vg_default_sh.  Commands without this
	 * set modify the vg and need ex.
	 */
	if (!mode)
		mode = def_mode;
	if (!mode)
		mode = cmd->lockd_vg_default_sh ? "sh" : "ex";

	if (cmd->lockopt & LOCKOPT_ADOPTVG)
		opts = "adopt_only";
	else if (cmd->lockopt & LOCKOPT_ADOPT)
		opts = "adopt";

	if ((cmd->lockopt & LOCKOPT_ADOPTVG) ||
	    (cmd->lockopt & LOCKOPT_ADOPT) ||
	    (cmd->lockopt & LOCKOPT_REPAIRVG) ||
	    (cmd->lockopt & LOCKOPT_REPAIR)) {
		if (dm_snprintf(opt_buf, sizeof(opt_buf), "%s%s%s",
			    (cmd->lockopt & LOCKOPT_ADOPTVG) ? "adopt_only" : "",
			    (cmd->lockopt & LOCKOPT_ADOPT) ? "adopt" : "",
			    (cmd->lockopt & (LOCKOPT_REPAIR|LOCKOPT_REPAIRVG)) ? "repair" : "") < 0) {
			log_error("Options string too long %x.", cmd->lockopt);
			return 0;
		}
		opts = opt_buf;
	}

	if (!strcmp(mode, "ex"))
		*lockd_state |= LDST_EX;

 req:
	/*
	 * This check is not at the top of the function so that
	 * we can first set LDST_EX which will be used later to
	 * decide whether a failure can be ignored or not.
	 *
	 * We do not know if this is a local VG or lockd VG yet,
	 * so we must return success, go ahead and read the VG,
	 * then check if the lock_type required lvmlockd or not.
	 */
	if (!_use_lvmlockd) {
		*lockd_state |= LDST_FAIL_REQUEST;
		return 1;
	}

	log_debug("lockd VG %s %s", vg_name, mode);

	if (!_lockd_request(cmd, "lock_vg",
			      vg_name, NULL, NULL, NULL, NULL, NULL, mode, opts,
			      NULL, &result, &lockd_flags, &owner)) {
		/*
		 * No result from lvmlockd, it is probably not running.
		 * Decide if it is ok to continue without a lock in
		 * access_vg_lock_type() after the VG has been read and
		 * the lock_type can be checked.  We don't care about
		 * this error for local VGs, but we do care for lockd VGs.
		 */
		*lockd_state |= LDST_FAIL_REQUEST;
		return 1;
	}

	if (result == -EAGAIN || result == -EIOTIMEOUT) {
		if (retries < find_config_tree_int(cmd, global_lvmlockd_lock_retries_CFG, NULL)) {
			if (result == -EIOTIMEOUT)
				log_warn("Retrying lock on VG %s: io timeout", vg_name);
			else
				log_warn("Retrying lock on VG %s: held by other host%s", vg_name, _owner_str(&owner));
			sleep(1);
			retries++;
			goto req;
		}
	}

	switch (result) {
	case 0:
		/* success */
		break;
	case -ENOLS:
		*lockd_state |= LDST_FAIL_NOLS;
		break;
	case -ESTARTING:
		*lockd_state |= LDST_FAIL_STARTING;
		break;
	default:
		*lockd_state |= LDST_FAIL_OTHER;
	}

	/*
	 * Normal success.
	 */
	if (!result)
		goto out;

	/*
	 * The VG has been removed.  This will only happen with a dlm VG
	 * since a sanlock VG must be stopped everywhere before it's removed.
	 */
	if (result == -EREMOVED) {
		log_warn("VG %s lock failed: removed", vg_name);
		goto out;
	}

	/*
	 * The lockspace for the VG is starting (the VG must not
	 * be local), and is not yet ready to do locking.  Allow
	 * reading without a sh lock during this period.
	 */
	if (result == -ESTARTING) {
		if (!strcmp(mode, "un"))
			goto out;
		else if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: lock start in progress", vg_name);
			goto out;
		} else {
			log_error("VG %s lock failed: lock start in progress", vg_name);
			ret = 0;
			goto out;
		}
	}

	/*
	 * sanlock is getting i/o errors while reading/writing leases, or the
	 * lockspace/VG is being killed after failing to renew its lease for
	 * too long.
	 */
	if (result == -EVGKILLED || result == -ELOCKIO) {
		const char *problem = (result == -ELOCKIO ? "errors" : "failed");

		if (!strcmp(mode, "un"))
			goto out;
		else if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: storage %s for sanlock leases", vg_name, problem);
			goto out;
		} else {
			log_error("VG %s lock failed: storage %s for sanlock leases", vg_name, problem);
			ret = 0;
			goto out;
		}
	}

	if (result == -EIOTIMEOUT) {
		if (!strcmp(mode, "un"))
			goto out;
		else if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: io timeout", vg_name);
			goto out;
		} else {
			log_error("VG %s lock failed: io timeout", vg_name);
			ret = 0;
			goto out;
		}
	}

	if (result == -ELOCKREPAIR) {
		if (!strcmp(mode, "un"))
			goto out;
		else if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: sanlock lease needs repair", vg_name);
			goto out;
		} else {
			log_error("VG %s lock failed: sanlock lease needs repair", vg_name);
			ret = 0;
			goto out;
		}
	}

	/*
	 * The lock is held by another host, and retries have been unsuccessful.
	 */
	if (result == -EAGAIN) {
		if (!strcmp(mode, "un"))
			goto out;
		else if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: held by other host%s", vg_name, _owner_str(&owner));
			goto out;
		} else {
			log_error("VG %s lock failed: held by other host%s", vg_name, _owner_str(&owner));
			ret = 0;
			goto out;
		}
	}

	if (result == -EORPHAN) {
		if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: orphan lock needs to be adopted.", vg_name);
			goto out;
		} else {
			log_error("VG %s lock failed: orphan lock needs to be adopted.", vg_name);
			ret = 0;
			goto out;
		}
	}

	if (result == -EADOPT_NONE) {
		if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: adopt found no orphan.", vg_name);
			goto out;
		} else {
			log_error("VG %s lock failed: adopt found no orphan.", vg_name);
			ret = 0;
			goto out;
		}
	}

	if (result == -EADOPT_RETRY) {
		if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: adopt found other mode.", vg_name);
			goto out;
		} else {
			log_error("VG %s lock failed: adopt found other mode.", vg_name);
			ret = 0;
			goto out;
		}
	}

	if (result == -ELMERR) {
		if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: lock manager error.", vg_name);
			ret = 1;
			goto out;
		} else {
			log_error("VG %s lock failed: lock manager error.", vg_name);
			ret = 0;
			goto out;
		}
	}

	/*
	 * No lockspace for the VG was found.  It may be a local
	 * VG that lvmlockd doesn't keep track of, or it may be
	 * a lockd VG that lvmlockd doesn't yet know about (it hasn't
	 * been started yet.)  Decide what to do after the VG is
	 * read and we can see the lock_type.
	 */
	if (result == -ENOLS)
		goto out;

	/*
	 * Another error.  We don't intend to reach here, but
	 * want to check for each specific error above so that
	 * a helpful message can be printed.
	 */
	if (result) {
		if (!strcmp(mode, "un"))
			goto out;
		else if (!strcmp(mode, "sh")) {
			log_warn("VG %s lock skipped: error %d", vg_name, result);
			goto out;
		} else {
			log_error("VG %s lock failed: error %d", vg_name, result);
			ret = 0;
			goto out;
		}
	}

out:
	/*
	 * A notice from lvmlockd that duplicate gl locks have been found.
	 * It would be good for the user to disable one of them.
	 */
	if ((lockd_flags & LD_RF_DUP_GL_LS) && strcmp(mode, "un"))
		log_warn("Duplicate sanlock global lock in VG %s", vg_name);
 
	return ret;
}

/*
 * This must be called before a new version of the VG metadata is
 * written to disk.  For local VGs, this is a no-op, but for lockd
 * VGs, this notifies lvmlockd of the new VG seqno.  lvmlockd must
 * know the latest VG seqno so that it can save it within the lock's
 * LVB.  The VG seqno in the VG lock's LVB is used by other hosts to
 * detect when their cached copy of the VG metadata is stale, i.e.
 * the cached VG metadata has a lower seqno than the seqno seen in
 * the VG lock.
 */

int lockd_vg_update(struct volume_group *vg)
{
	daemon_reply reply;
	int result;
	int ret;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;
	if (!vg_is_shared(vg))
		return 1;

#if !LVMLOCKD_USE_SANLOCK_LVB
	/*
	 * lvb (for lock version) is disabled for sanlock since
	 * lock versions are not used any more, and it's more
	 * costly for sanlock to implement (extra i/o.)
	 */
	if (!strcmp(vg->lock_type, "sanlock"))
		return 1;
#endif
	log_debug("lockd_vg_update %s", vg->name);

	reply = _lockd_send("vg_update",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"version = " FMTd64, (int64_t) vg->seqno,
				NULL);

	if (!_lockd_result(vg->cmd, "vg_update", reply, &result, NULL, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	daemon_reply_destroy(reply);
	return ret;
}

int lockd_vg_is_started(struct cmd_context *cmd, struct volume_group *vg, uint32_t *cur_gen)
{
	daemon_reply reply;
	struct owner owner = { 0 };
	int result;
	int ret = 0;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;
	if (!vg_is_shared(vg))
		return 0;

	log_debug("lockd_vg_status %s", vg->name);

	reply = _lockd_send("vg_status",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				NULL);

	if (!_lockd_result(vg->cmd, "vg_status", reply, &result, NULL, &owner)) {
		log_debug("lockd_vg_status %s no result", vg->name);
		goto out;
	}

	if (result < 0) {
		log_debug("lockd_vg_status %s result error %d", vg->name, result);
		goto out;
	}

	log_debug("lockd_vg_status %s host_id %u gen %u",
		  vg->name, owner.host_id, owner.generation);

	if (cur_gen)
		*cur_gen = owner.generation;
	ret = 1;
 out:
	daemon_reply_destroy(reply);
	return ret;
}

static int _query_lv(struct cmd_context *cmd, struct volume_group *vg,
		     const char *lv_name, char *lv_uuid, const char *lock_args,
		     int *ex, int *sh)
{
	daemon_reply reply;
	const char *reply_str;
	int result;
	int ret;

	log_debug("lockd query LV %s/%s", vg->name, lv_name);

	reply = _lockd_send("query_lock_lv",
				"pid = " FMTd64, (int64_t) getpid(),
				"opts = %s", "none",
				"vg_name = %s", vg->name,
				"lv_name = %s", lv_name,
				"lv_uuid = %s", lv_uuid,
				"vg_lock_type = %s", vg->lock_type,
				"vg_lock_args = %s", vg->lock_args,
				"lv_lock_args = %s", lock_args ?: "none",
				NULL);

	if (!_lockd_result(cmd, "query_lock_lv", reply, &result, NULL, NULL)) {
		/* No result from lvmlockd, it is probably not running. */
		log_error("Lock query failed for LV %s/%s", vg->name, lv_name);
		return 0;
	} else {
		/* ENOENT => The lv was not active/locked. */
		ret = (result < 0 && (result != -ENOENT)) ? 0 : 1;
	}

	if (!ret)
		log_error("query_lock_lv lvmlockd result %d", result);

	if (!(reply_str = daemon_reply_str(reply, "mode", NULL))) {
		log_error("query_lock_lv mode not returned");
		ret = 0;
	}

	if (reply_str && !strcmp(reply_str, "ex"))
		*ex = 1;
	else if (reply_str && !strcmp(reply_str, "sh"))
		*sh = 1;

	daemon_reply_destroy(reply);

	return ret;
}

int lockd_query_lv(struct cmd_context *cmd, struct logical_volume *lv, int *ex, int *sh)
{
	struct volume_group *vg = lv->vg;
	char lv_uuid[64] __attribute__((aligned(8)));

	if (cmd->lockd_lv_disable)
		return 1;
	if (!vg_is_shared(vg))
		return 1;
	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	/* types that cannot be active concurrently will always be ex. */
	if (lv_is_external_origin(lv) ||
	    lv_is_thin_type(lv) ||
	    lv_is_mirror_type(lv) ||
	    lv_is_raid_type(lv) ||
	    lv_is_vdo_type(lv) ||
	    lv_is_cache_type(lv)) {
		*ex = 1;
		return 1;
	}

	if (!id_write_format(&lv->lvid.id[1], lv_uuid, sizeof(lv_uuid)))
		return_0;

	return _query_lv(cmd, vg, lv->name, lv_uuid, lv->lock_args, ex, sh);
}

/*
 * When this is called directly (as opposed to being called from
 * lockd_lv), the caller knows that the LV has a lock.
 */

int lockd_lv_name(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lv_name, struct id *lv_id,
		  const char *lock_args, const char *def_mode, uint32_t flags)
{
	struct owner owner = { 0 };
	char lv_uuid[64] __attribute__((aligned(8)));
	char opt_buf[64] = {};
	const char *opts = NULL;
	const char *mode = NULL;
	uint32_t lockd_flags;
	int refreshed = 0;
	int result;
	struct lvmlockd_pvs lock_pvs;

	/*
	 * Verify that when --readonly is used, no LVs should be activated or used.
	 */
	if (cmd->metadata_read_only) {
		log_error("LV locks are not allowed with readonly option.");
		return 0;
	}

	if (!id_write_format(lv_id, lv_uuid, sizeof(lv_uuid)))
		return_0;

	if (cmd->lockd_lv_disable && !strcmp(vg->lock_type, "dlm")) {
		/*
		 * If the command is updating an LV with a shared lock,
		 * and using --lockopt skiplv to skip the incompat ex
		 * lock, then check if an existing sh lock exists.
		 */
		if (!strcmp(cmd->name, "lvextend") || !strcmp(cmd->name, "lvresize") ||
		    !strcmp(cmd->name, "lvchange") || !strcmp(cmd->name, "lvconvert")) {
			int ex = 0, sh = 0;

			if (!_query_lv(cmd, vg, lv_name, lv_uuid, lock_args, &ex, &sh))
				return 1;
			if (sh) {
				log_warn("WARNING: Shared LV may require refresh on other hosts where it is active.");
				return 1;
			}
		}
		return 1;
	}

	if (cmd->lockd_lv_disable) {
		log_debug("lockd_lv disabled %s %s/%s", def_mode ?: "", vg->name, lv_name);
		if (def_mode && strcmp(def_mode, "un"))
			log_warn("WARNING: Skipping LV lock in lvmlockd.");
		return 1;
	}

	if (!_use_lvmlockd || !_lvmlockd_connected) {
		if (def_mode && !strcmp(def_mode, "un"))
			return 1;
		if (!_use_lvmlockd)
			log_error("LV %s/%s lock failed: lvmlockd is required for VG lock_type %s.",
				  vg->name, lv_name, vg->lock_type ?: "unknown");
		if (!_lvmlockd_connected)
			log_error("LV %s/%s lock failed: lvmlockd connection is required.",
				  vg->name, lv_name);
		return 0;
	}

	/*
	 * For lvchange/vgchange activation, def_mode is "sh" or "ex"
	 * according to the specific -a{e,s}y mode designation.
	 * No e,s designation gives NULL def_mode.
	 */

	if (def_mode)
		mode = def_mode;

	if (mode && !strcmp(mode, "sh") && (flags & LDLV_MODE_NO_SH)) {
		struct logical_volume *lv = find_lv(vg, lv_name);
		log_error("Shared activation not compatible with LV type %s of %s/%s",
			  lv ? lvseg_name(first_seg(lv)) : "", vg->name, lv_name);
		return 0;
	}

	if (!mode)
		mode = "ex";

	if ((flags & LDLV_PERSISTENT) ||
	    (cmd->lockopt & LOCKOPT_ADOPTLV) ||
	    (cmd->lockopt & LOCKOPT_ADOPT) ||
	    (cmd->lockopt & LOCKOPT_REPAIRLV) ||
	    (cmd->lockopt & LOCKOPT_REPAIR)) {
		if (dm_snprintf(opt_buf, sizeof(opt_buf), "%s%s%s%s",
			    (flags & LDLV_PERSISTENT) ? "persistent," : "",
			    (cmd->lockopt & LOCKOPT_ADOPTLV) ? "adopt_only" : "",
			    (cmd->lockopt & LOCKOPT_ADOPT) ? "adopt" : "",
			    (cmd->lockopt & (LOCKOPT_REPAIR|LOCKOPT_REPAIRLV)) ? "repair" : "") < 0) {
			log_error("Options string too long %x.", cmd->lockopt);
			return 0;
		}
		opts = opt_buf;
	}

 retry:
	log_debug("lockd_lv %s %s/%s %s %s", mode, vg->name, lv_name, lv_uuid, opts ?: "");

	/* Pass PV list for IDM lock type */
	if (!strcmp(vg->lock_type, "idm")) {
		_lockd_retrieve_lv_pv_list(vg, lv_name, &lock_pvs);
		if (!_lockd_request(cmd, "lock_lv",
				       vg->name, vg->lock_type, vg->lock_args,
				       lv_name, lv_uuid, lock_args, mode, opts,
				       &lock_pvs, &result, &lockd_flags, NULL)) {
			_lockd_free_pv_list(&lock_pvs);
			/* No result from lvmlockd, it is probably not running. */
			log_error("Locking failed for LV %s/%s", vg->name, lv_name);
			return 0;
		}
		_lockd_free_pv_list(&lock_pvs);
	} else {
		if (!_lockd_request(cmd, "lock_lv",
				       vg->name, vg->lock_type, vg->lock_args,
				       lv_name, lv_uuid, lock_args, mode, opts,
				       NULL, &result, &lockd_flags, &owner)) {
			/* No result from lvmlockd, it is probably not running. */
			log_error("Locking failed for LV %s/%s", vg->name, lv_name);
			return 0;
		}
	}

	/* The lv was not active/locked. */
	if (result == -ENOENT && !strcmp(mode, "un"))
		return 1;

	if (result == -EALREADY)
		return 1;

	if (result == -EAGAIN) {
		log_error("LV locked by other host: %s/%s%s", vg->name, lv_name, _owner_str(&owner));
		return 0;
	}

	if (result == -EIOTIMEOUT) {
		log_error("LV %s/%s lock failed: io timeout.", vg->name, lv_name);
		return 0;
	}

	if (result == -ELOCKREPAIR) {
		log_error("LV %s/%s lock failed: sanlock lease needs repair.", vg->name, lv_name);
		return 0;
	}

	if (result == -EORPHAN) {
		log_error("LV %s/%s lock failed: orphan lock needs to be adopted.", vg->name, lv_name);
		return 0;
	}

	if (result == -EADOPT_NONE) {
		log_error("LV %s/%s lock failed: adopt found no orphan.", vg->name, lv_name);
		return 0;
	}

	if (result == -EADOPT_RETRY) {
		log_error("LV %s/%s lock failed: adopt found other mode.", vg->name, lv_name);
		return 0;
	}

	if (result == -EEXIST) {
		/*
		 * This happens if a command like lvchange tries to modify the
		 * LV with an ex LV lock when the LV is already active with a
		 * sh LV lock.
		 */

		if (lockd_flags & LD_RF_SH_EXISTS) {
			if (flags & LDLV_SH_EXISTS_OK) {
				log_warn("WARNING: Extending LV with a shared lock, other hosts may require LV refresh.");
				cmd->lockd_lv_sh_for_ex = 1;
				return 1;
			}
		}

		log_error("LV is already locked with incompatible mode: %s/%s", vg->name, lv_name);
		return 0;
	}

	if (result == -EMSGSIZE) {
		/* Another host probably extended lvmlock. */
		if (!refreshed++) {
			log_debug("Refresh lvmlock");
		       	_refresh_sanlock_lv(cmd, vg);
			goto retry;
		}
	}

	if (result == -ENOLS) {
		log_error("LV %s/%s lock failed: lockspace is inactive", vg->name, lv_name);
		return 0;
	}

	if (result == -EVGKILLED || result == -ELOCKIO) {
		const char *problem = (result == -ELOCKIO ? "errors" : "failed");
		log_error("LV %s/%s lock failed: storage %s for sanlock leases", vg->name, lv_name, problem);
		return 0;
	}

	if (result < 0) {
		log_error("LV %s/%s lock failed: error %d", vg->name, lv_name, result);
		return 0;
	}

	return 1;
}

/*
 * In general, persistent locks are used for activating an LV,
 * and transient locks are used for any other LV access by a
 * command.  In the transient case, access to the LV from the
 * command stops when the command exits.  In the persistent
 * case, access to the active LV device continues after the
 * activation command exits.
 *
 * A complication of this is when a command temporarily
 * activates an LV for its own purposes, to access it
 * only for the duration of the command, and deactivate
 * it when done.  If the command crashes, a transient
 * lock will be released, potentially while the LV
 * remains active on the system.  (It would be ideal if
 * the temp activation would also be automatically dropped
 * if the command crashed, like the transient lock.)
 *
 * The problem with using persistent locks instead of
 * transient locks when commands temporarily activate LVs
 * for the duration of the command, is that it's difficult
 * for the command to know if should unlock the persistent
 * lock before exiting.  It needs to have a place at the
 * end of the command where it can check if the LV will
 * remain active, and not unlock the persistent lock if so.
 * We use this strategy in lvcreate for thin pools/volumes.
 *
 * case 1
 * . LV is active, with a persistent lock in lvmlockd
 * . a command wants to do something with LV, and requests
 *   a transient lock
 * . the transient lock request is granted by lvmlockd
 *   when it sees a persistent lock exists
 *   (the transient lock request is a no-op in lvmlockd
 *   when a persistent lock exists)
 * . when the command is done, its transient lock is
 *   unlocked (either explicitly or automatically by
 *   command exit)
 * . the transient unlock does not affect the persistent
 *   lock that existed for the active LV
 * . the LV remains active with its persistent lock held
 *
 * case 2
 * . LV is inactive, with no lock in lvmlockd
 * . a command wants to do something with LV, and requests
 *   a transient lock
 * . lvmlockd acquires the lock
 * . if the command activates the LV (for "private" use),
 *   it must also deactivate it before exiting
 * . when the command is done, its transient lock is
 *   unlocked (either explicitly or automatically by
 *   command exit)
 * . LV is inactive, with no lock held
 */

static int _lockd_lvcreate_lock_thin(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp,
				     int creating_thin_pool, int creating_thin_volume)
{
	if ((creating_thin_pool && cmd->lockd_created_thin_pool) ||
	    (creating_thin_volume && cmd->lockd_created_thin_volume) ||
	    (creating_thin_pool && cmd->lockd_creating_thin_pool) ||
	    (creating_thin_volume && cmd->lockd_creating_thin_volume)) {
		/* shouldn't happen */
		log_error("lockd_lvcreate_lock invalid thin transition creating p %d v %d created p %d v %d",
			  creating_thin_pool, creating_thin_volume, cmd->lockd_created_thin_pool, cmd->lockd_created_thin_volume);
		return 0;
	}

	/*
	 * cmd->lockd_creating_thin_pool and LDLV_CREATING_THIN_POOL, or
	 * cmd->lockd_creating_thin_volume and LDLV_CREATING_THIN_VOLUME
	 * enable lockd_lv().
	 */
	cmd->lockd_creating_thin_pool = creating_thin_pool;
	cmd->lockd_creating_thin_volume = creating_thin_volume;
                         
	/*
	 * If a thin pool was just created, then it's already locked.
	 * If a thin pool was not just created, then we need to lock
	 * the thin pool before creating a thin volume.
	 */
	if (creating_thin_volume && !cmd->lockd_created_thin_pool) {
		struct logical_volume *pool_lv;

		log_debug("lockd_lvcreate_lock creating_thin_volume locking thin pool %s", lp->pool_name);

		if (!(pool_lv = find_lv(vg, lp->pool_name))) {
			log_error("Couldn't find thin pool %s for creating thin volume.", lp->pool_name);
			return 0;
		}

		if (!lockd_lv(cmd, pool_lv, "ex", LDLV_PERSISTENT | LDLV_CREATING_THIN_VOLUME)) {
			log_error("Failed to lock thin pool %s for creating thin volume.", pool_lv->name);
			return 0;
		}

		/* Save pool info to use in lockd_lvcreate_done() */
		lp->lockd_name = dm_pool_strdup(cmd->mem, pool_lv->name);
	}

	return 1;
}

/*
 * Lock an existing LV in lvmlockd that is required to create
 * another new LV associated with it.
 */
int lockd_lvcreate_lock(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp,
			int creating_thin_pool, int creating_thin_volume, int creating_cow_snapshot,
			int creating_vdo_volume)
{
	if (!vg_is_shared(vg))
		return 1;

	/*
	 * Thin is more complicated than others because a single lvcreate may
	 * be creating just a thin pool, just a thin volume, or both.
	 */
	if (creating_thin_pool || creating_thin_volume) {
		log_debug("lockd_lvcreate_lock creating_thin_pool %d creating_thin_volume %d created pool %d volume %d",
			  creating_thin_pool, creating_thin_volume,
			  cmd->lockd_created_thin_pool, cmd->lockd_created_thin_volume);
                         
		return _lockd_lvcreate_lock_thin(cmd, vg, lp, creating_thin_pool, creating_thin_volume);
	}

	if (creating_cow_snapshot) {
		struct logical_volume *origin_lv;

		log_debug("lockd_lvcreate_lock creating_cow_snapshot locking origin %s", lp->origin_name);

		if (!lp->origin_name) {
			/* Sparse LV case. We require a lock from the origin LV. */
			log_error("Cannot create snapshot without origin LV in shared VG.");
			return 0;
		}

		if (!(origin_lv = find_lv(vg, lp->origin_name))) {
			log_error("Failed to find origin LV %s/%s", vg->name, lp->origin_name);
			return 0;
		}

		if (!lockd_lv(cmd, origin_lv, "ex", LDLV_CREATING_COW_SNAP_ON_THIN)) {
			log_error("Failed to lock snapshot origin LV %s/%s", vg->name, lp->origin_name);
			return 0;
		}

		return 1;
	}

	if (creating_vdo_volume) {
		struct logical_volume *vdo_pool_lv;

		log_debug("lockd_lvcreate_lock creating_vdo_volume locking vdo pool %s", lp->pool_name);

		if (!(vdo_pool_lv = find_lv(vg, lp->pool_name))) {
			log_error("Failed to find vdo pool %s/%s", vg->name, lp->pool_name);
			return 0;
		}

		if (!lockd_lv(cmd, vdo_pool_lv, "ex", LDLV_PERSISTENT)) {
			log_error("Failed to lock vdo pool %s/%s", vg->name, lp->pool_name);
			return 0;
		}

		return 1;
	}

	/* Nothing to do */
	return 1;
}

int lockd_lvcreate_prepare(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp)
{
	if (!vg_is_shared(vg))
		return 1;

	if (cmd->command_enum == lvcreate_thin_vol_with_thinpool_or_sparse_snapshot_CMD) {
		log_error("Use lvconvert to create thin pools and cache pools in a shared VG.");
		return 0;
	}

	if (!strcmp(vg->lock_type, "sanlock")) {
		if (segtype_is_thin_volume(lp->segtype) && !lp->create_pool)
			log_debug("lockd_lvcreate_prepare find_free_lock skipped for thin volume");
		else if (!segtype_is_thin_volume(lp->segtype) && lp->snapshot)
			log_debug("lockd_lvcreate_prepare find_free_lock skipped for cow snap of thin volume");
		else {
			/* Ensure there is space on disk for a new sanlock lease. */
			if (!_handle_sanlock_lv(cmd, vg)) {
				log_error("No space for sanlock lock, extend the internal lvmlock LV.");
				return 0;
			}
		}
	}

	/*
	 * The primary LV requested by the user begins with the
	 * expectation that a lock is needed for the new LV.
	 * This will be cleared for some cases that do not need
	 * a lock.  The lp struct for an internal LV that the
	 * command creates will not have this set, so those
	 * internal LVs will not by default have locks allocated.
	 */
	lp->needs_lockd_init = 1;

	return 1;
}

void lockd_lvcreate_done(struct cmd_context *cmd, struct volume_group *vg, struct lvcreate_params *lp)
{
	struct logical_volume *pool_lv;
	uint32_t flags = LDLV_PERSISTENT;

	if (!vg_is_shared(vg))
		return;

	if (!cmd->lockd_created_thin_volume && !cmd->lockd_created_thin_pool)
		return;

	if (!lp->lockd_name) {
		log_error("lockd_lvcreate_done missing name %s", lp->lockd_name ?: "-");
		return;
	}

	if (!(pool_lv = find_lv(vg, lp->lockd_name))) {
		log_error("lockd_lvcreate_done cannot find thin pool %s", lp->lockd_name);
		return;
	}

	if (thin_pool_is_active(pool_lv)) {
		log_debug("lockd_lvcreate_done hold lock for active thin pool");
		return;
	}

	if (cmd->lockd_creating_thin_pool)
		flags |= LDLV_CREATING_THIN_POOL;
	else if (cmd->lockd_creating_thin_volume)
		flags |= LDLV_CREATING_THIN_VOLUME;

	if (!lockd_lv_name(cmd, vg, pool_lv->name, &pool_lv->lvid.id[1], pool_lv->lock_args, "un", flags))
		log_error("Failed to unlock thin pool %s", lp->lockd_name);
}

int lockd_lvremove_lock(struct cmd_context *cmd, struct logical_volume *lv,
			struct logical_volume **lv_other, int *other_unlock)
{
	struct volume_group *vg = lv->vg;

	*lv_other = NULL;

	if (!vg_is_shared(vg))
		return 1;

	if (lv_is_thin_type(lv)) {
		struct logical_volume *lv_pool;

		if (lv_is_thin_volume(lv))
			lv_pool = first_seg(lv)->pool_lv;
		else if (lv_is_thin_pool(lv))
			lv_pool = lv;
		else
			return_0;
		if (!lv_pool)
			return_0;

		if (!lv_pool->lockd_thin_pool_locked) {
			log_debug("lockd_lvremove_lock thin pool %s for %s", lv_pool->name, lv->name);

			if (!lockd_lv(cmd, lv_pool, "ex", LDLV_PERSISTENT))
				return_0;

			lv_pool->lockd_thin_pool_locked = 1;
		} else
			log_debug("lockd_lvremove_lock skip repeat thin pool %s", lv_pool->name);

		*lv_other = lv_pool;
		*other_unlock = 2; /* 2: unlock persistent */

	} else if (lv_is_cow(lv)) {
		struct logical_volume *lv_origin;

		if (!(lv_origin = origin_from_cow(lv)))
			return_0;

		log_debug("lockd_lvremove_lock cow origin %s for %s", lv_origin->name, lv->name);

		if (!lockd_lv(cmd, lv_origin, "ex", 0))
			return_0;

		*lv_other = lv_origin;
		*other_unlock = 1; /* 1: unlock transient */

	} else if (lv_is_vdo(lv)) {
		struct logical_volume *lv_pool;

		if (!first_seg(lv))
			return_0;
		if (!(lv_pool = seg_lv(first_seg(lv), 0)))
			return_0;

		log_debug("lockd_lvremove_lock vdo pool %s for %s", lv_pool->name, lv->name);

		if (!lockd_lv(cmd, lv_pool, "ex", 0))
			return_0;

		*lv_other = lv_pool;
		*other_unlock = 1; /* 1: unlock transient */

	} else if (lv_is_cache_pool(lv) || lv_is_cache_vol(lv)) {
		struct logical_volume *lv_main;
		struct lv_segment *seg_main;

		/*
		 * lvremove of the hidden cachepool or cachevol is a backdoor
		 * method of lvconvert --uncache when using dm-cache.
		 */

		/* No locking is done for an unused cache pool. */
		if (dm_list_empty(&lv->segs_using_this_lv))
			return 1;

		if (!(seg_main = get_only_segment_using_this_lv(lv)))
			return_0;
		if (!(lv_main = seg_main->lv))
			return_0;

		if (!lv_is_cache(lv_main)) {
			/* lvremove to uncache doesn't apply to writecache. */
			log_error("Detach cachevol before removing.");
			return 0;
		}

		/*
		 * If lv_main is active, use a transient lock, which is a no-op
		 * here, and unlocking the transient lock in
		 * lockd_lvremove_done doesn't affect the existing persistent
		 * lock.  If lv_main is inactive, also use a transient lock,
		 * which will acquire the lock here, and it will be released in
		 * lockd_lvremove_done.
		 */

		log_debug("lockd_lvremove_lock cache main %s for %s", lv_main->name, lv->name);

		if (!lockd_lv(cmd, lv_main, "ex", 0))
			return_0;

		*lv_other = lv_main;
		*other_unlock = 1; /* 1: unlock transient */

	} else {
		/*
		 * The original simple approach to locking here is to request a
		 * persistent ex lock, and do a persistent unlock before
		 * lockd_free_lv.  That works whether or not the LV is already
		 * active with an existing persistent lock.  The problem with
		 * that approach is if the command follows an error path before
		 * unlock and free, and the LV isn't removed.  In that case, a
		 * persistent lock acquired here (i.e. the LV wasn't active
		 * before lvremove) may remain in place without the LV being
		 * active.
		 *
		 * FIXME: to fix that, request a transient ex lock here, and
		 * unlock either a transient or persistent lock before free_lv.
		 * That requires a flag telling lvmlockd to unlock either a
		 * persistent or transient lock, or tracking in the command
		 * whether a transient or persistent lock is held, so that the
		 * correct unlock can be used to release it.  If a persistent
		 * lock already existed, the transient lock requested here
		 * will be a no-op, and the persistent lock will remain if
		 * the LV is not removed.  If a transient lock is acquired
		 * here, it will be dropped if lvremove follows an error
		 * path where the LV is not removed, or the transient lock
		 * will be unlocked before free_lv.
		 */
		log_debug("lockd_lvremove_lock %s", lv->name);

		if (!lockd_lv(cmd, lv, "ex", LDLV_PERSISTENT))
			return_0;
	}

	return 1;
}

void lockd_lvremove_done(struct cmd_context *cmd, struct logical_volume *lv, struct logical_volume *lv_other,
			 int other_unlock)
{
	struct volume_group *vg = lv->vg;

	if (!vg_is_shared(vg))
		return;

	if (lv_other && lv_is_thin_pool(lv_other)) {
		if (thin_pool_is_active(lv_other))
			log_debug("lockd_lvremove_done skip unlock of active thin pool %s for %s", lv_other->name, lv->name);

		else if (lv_other->lockd_thin_pool_locked && !lv_other->lockd_thin_pool_unlocked) {
			log_debug("lockd_lvremove_done unlock thin pool %s for %s", lv_other->name, lv->name);

			if (!lockd_lv(cmd, lv_other, "un", LDLV_PERSISTENT))
				goto_bad;
			else
				lv_other->lockd_thin_pool_unlocked = 1;
		} else
			log_debug("lockd_lvremove_done skip unlocked thin pool %s for %s", lv_other->name, lv->name);

	} else if (lv_other) {
		if (!other_unlock) {
			log_debug("lockd_lvremove_done skip unlock %s for %s", lv_other->name, lv->name);
			return;
		}

		log_debug("lockd_lvremove_done unlock %s for %s%s", lv_other->name, lv->name,
			  (other_unlock == 2) ? " persistent" : "");

		if (!lockd_lv(cmd, lv_other, "un", (other_unlock == 2) ? LDLV_PERSISTENT : 0))
			goto_bad;
	} else {
		log_debug("lockd_lvremove_done unlock %s", lv->name);

		if (!lockd_lv(cmd, lv, "un", LDLV_PERSISTENT))
			goto_bad;
	}

	/*
	 * In some cases the LV being removed will not have a lock itself to
	 * free (no lock_args), e.g. when removing a thin LV.
	 */
	if (lv->lock_args)
		lockd_free_lv_queue(cmd, vg, lv->name, &lv->lvid.id[1], lv->lock_args);

	return;
bad:
	log_warn("WARNING: Failed to unlock %s.", lv_other ? display_lvname(lv_other) : display_lvname(lv));
}

/*
 * Direct the lock request to the pool LV.
 * For a thin pool and all its thin volumes, one ex lock is used.
 * It is the one specified in metadata of the pool data lv.
 */

static int _lockd_lv_thin(struct cmd_context *cmd, struct logical_volume *lv,
			  const char *def_mode, uint32_t flags)
{
	struct logical_volume *pool_lv = NULL;
	int pool_is_active = 0;
	int locking = 0;
	int unlocking = 0;
	int result;

	if (def_mode && !strcmp(def_mode, "un"))
		unlocking = 1;
	else
		locking = 1;

	if (lv_is_thin_volume(lv)) {
		struct lv_segment *pool_seg = first_seg(lv);
		pool_lv = pool_seg ? pool_seg->pool_lv : NULL;
		log_debug("lockd_lv_thin thin_volume");

	} else if (lv_is_thin_pool(lv)) {
		pool_lv = lv;
		log_debug("lockd_lv_thin thin_pool");

	} else if (lv_is_thin_pool_data(lv)) {
		/* FIXME: there should be a function to get pool lv from data lv. */
		pool_lv = lv_parent(lv);
		log_debug("lockd_lv_thin thin_data");

	} else if (lv_is_thin_pool_metadata(lv)) {
		struct lv_segment *pool_seg = get_only_segment_using_this_lv(lv);
		if (pool_seg)
			pool_lv = pool_seg->lv;
		log_debug("lockd_lv_thin thin_metadata");

	} else {
		/* This should not happen AFAIK. */
		log_error("Lock on incorrect thin lv type %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!pool_lv) {
		/* This should not happen. */
		log_error("Cannot find thin pool for %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (cmd->lockd_creating_thin_pool && (flags & LDLV_CREATING_THIN_POOL)) {
		/* do it */
		log_debug("lockd_lv_thin creating_thin_pool");
	} else if (cmd->lockd_creating_thin_volume && (flags & LDLV_CREATING_THIN_VOLUME)) {
		/* do it */
		log_debug("lockd_lv_thin creating_thin_volume");
	} else if (cmd->lockd_creating_thin_pool && !(flags & LDLV_CREATING_THIN_POOL)) {
		/* skip it, this lockd_lv is intentionally suppressed for lvcreate */
		log_debug("lockd_lv_thin creating_thin_pool skip without LDLV_CREATING_THIN_POOL");
		return 1;
	} else if (cmd->lockd_creating_thin_volume && !(flags & LDLV_CREATING_THIN_VOLUME)) {
		/* skip it, this lockd_lv is intentionally suppressed for lvcreate */
		log_debug("lockd_lv_thin creating_thin_volume skip without LDLV_CREATING_THIN_VOLUME");
		return 1;
	} else if (flags & LDLV_CREATING_THIN_POOL) {
		/* flags used in wrong context */
		log_error("lockd_lv_thin invalid use of LDLV_CREATING_THIN_POOL");
		return 0;
	} else if (flags & LDLV_CREATING_THIN_VOLUME) {
		/* flags used in wrong context */
		log_error("lockd_lv_thin invalid use of LDLV_CREATING_THIN_VOLUME");
		return 0;
	} else if (flags & LDLV_CREATING_COW_SNAP_ON_THIN) {
		/* do it */
		log_debug("lockd_lv_thin creating cow snapshot of thin volume");
	} else {
		if (cmd->command_enum == lvcreate_new_plus_old_cachepool_or_lvconvert_old_plus_new_cachepool_CMD) {
			/* This command def defies all normal usage. */
			log_debug("lockd_lv_thin for lvcreate_new_plus_old_cachepool_or_lvconvert_old_plus_new_cachepool");
		} else if (!strcmp(cmd->name, "lvcreate")) {
			/* shouldn't happen, this is here to catch any new
			   cases that needs to be handled. */
			log_error("lockd_lv_thin from lvcreate undefined case.");
			return 0;
		}
		/* Normal thin locking for things other than lvcreate, e.g. activation */
		log_debug("lockd_lv_thin for %s", cmd->name);
	}

	pool_is_active = thin_pool_is_active(pool_lv);

	/*
	 * Locking a locked lv (pool in this case) is a no-op.
	 * Unlock when the pool is no longer active.
	 */
	if (unlocking && pool_is_active) {
		log_debug("lockd_lv_thin skip unlock for active pool %s", pool_lv->name);
		return 1;
	}

	/*
	 * Optimization for "lvchange -a n|y" of all LVs in the VG, which
	 * means this function is called for a thin pool and all thin volumes
	 * in it (and the meta/data sublvs of the pool due to the component
	 * activation special case in process_each_lv_in_vg.)
	 *
	 * Remember when a thin pool has been unlocked or unlocked by the
	 * command already, to avoid sending repeated unlock|lock requests
	 * to lvmlockd for the same thin pool.
	 */
	if (unlocking && !pool_is_active && pool_lv->lockd_thin_pool_unlocked) {
		log_debug("lockd_lv_thin skip repeated unlock for inactive pool %s", pool_lv->name);
		return 1;
	}
	if (locking && pool_is_active && pool_lv->lockd_thin_pool_locked) {
		log_debug("lockd_lv_thin skip repeated lock for active pool %s", pool_lv->name);
		return 1;
	}

	flags |= LDLV_MODE_NO_SH;

	result = lockd_lv_name(cmd, pool_lv->vg, pool_lv->name, &pool_lv->lvid.id[1],
			       pool_lv->lock_args, def_mode, flags);

	if (result && unlocking)
		pool_lv->lockd_thin_pool_unlocked = 1;
	if (result && locking)
		pool_lv->lockd_thin_pool_locked = 1;

	return result;
}

static int _lockd_lv_vdo(struct cmd_context *cmd, struct logical_volume *lv,
			 const char *def_mode, uint32_t flags)
{
	struct logical_volume *pool_lv = NULL;

	if (lv_is_vdo(lv)) {
		if (first_seg(lv))
			pool_lv = seg_lv(first_seg(lv), 0);

	} else if (lv_is_vdo_pool(lv)) {
		pool_lv = lv;

	} else if (lv_is_vdo_pool_data(lv)) {
		return 1;

	} else {
		/* This should not happen AFAIK. */
		log_error("Lock on incorrect vdo lv type %s/%s",
			  lv->vg->name, lv->name);
		return 0;
	}

	if (!pool_lv) {
		/* This happens in lvremove where it's harmless. */
		log_debug("No vdo pool for %s/%s", lv->vg->name, lv->name);
		return 0;
	}

	/*
	 * Locking a locked lv (pool in this case) is a no-op.
	 * Unlock when the pool is no longer active.
	 */

	if (def_mode && !strcmp(def_mode, "un") &&
	    lv_is_vdo_pool(pool_lv) && lv_is_active(lv_lock_holder(pool_lv)))
		return 1;

	flags |= LDLV_MODE_NO_SH;

	return lockd_lv_name(cmd, pool_lv->vg, pool_lv->name, &pool_lv->lvid.id[1],
			     pool_lv->lock_args, def_mode, flags);
}

/*
 * If the VG has no lock_type, then this function can return immediately.
 * The LV itself may have no lock (NULL lv->lock_args), but the lock request
 * may be directed to another lock, e.g. the pool LV lock in _lockd_lv_thin.
 * If the lock request is not directed to another LV, and the LV has no
 * lock_type set, it means that the LV has no lock, and no locking is done
 * for it.
 *
 * An LV lock is acquired before the LV is activated, and released
 * after the LV is deactivated.  If the LV lock cannot be acquired,
 * it means that the LV is active on another host and the activation
 * fails.  Commands that modify an inactive LV also acquire the LV lock.
 *
 * In non-lockd VGs, this is a no-op.
 *
 * In lockd VGs, normal LVs each have their own lock, but other
 * LVs do not have their own lock, e.g. the lock for a thin LV is
 * acquired on the thin pool LV, and a thin LV does not have a lock
 * of its own.  A cache pool LV does not have a lock of its own.
 * When the cache pool LV is linked to an origin LV, the lock of
 * the origin LV protects the combined origin + cache pool.
 */

int lockd_lv(struct cmd_context *cmd, struct logical_volume *lv,
	     const char *def_mode, uint32_t flags)
{
	if (!vg_is_shared(lv->vg))
		return 1;

	log_debug("lockd_lv %s %s", def_mode ?: "no_mode", display_lvname(lv));

	/*
	 * This addresses the specific case of: vgchange -an vg
	 * when vg is a shared VG that is not started.  Without
	 * this check, the command will try and fail to unlock
	 * every LV, which is wasted effort if the lockspace is
	 * not started, especially with many LVs in the VG.
	 * The command still attempts to deactivate the LVs,
	 * which it should in case they are active for some reason.
	 */
	if (lv->vg->lockd_not_started && (def_mode && !strcmp(def_mode, "un"))) {
		log_debug("Skip LV unlock: no lockspace");
		return 1;
	}

	if (lv_is_thin_type(lv))
		return _lockd_lv_thin(cmd, lv, def_mode, flags);

	if (lv_is_vdo_type(lv))
		return _lockd_lv_vdo(cmd, lv, def_mode, flags);

	/*
	 * An LV with NULL lock_args does not have a lock of its own.
	 */
	if (!lv->lock_args) {
		log_debug("Skip LV lock: no lock args for %s", lv->name);
		return 1;
	}

	/*
	 * A cachevol LV is one exception, where the LV keeps lock_args (so
	 * they do not need to be reallocated on split) but the lvmlockd lock
	 * is not used.
	 */
	if (lv_is_cache_vol(lv))
		return 1;

	/*
	 * LV type cannot be active concurrently on multiple hosts,
	 * so shared mode activation is not allowed.
	 */
	if (lv_is_external_origin(lv) ||
	    lv_is_thin_type(lv) ||
	    lv_is_mirror_type(lv) ||
	    lv_is_raid_type(lv) ||
	    lv_is_vdo_type(lv) ||
	    lv_is_cache_type(lv) ||
	    lv_is_origin(lv) ||
	    lv_is_cow(lv)) {
		flags |= LDLV_MODE_NO_SH;
	}

	return lockd_lv_name(cmd, lv->vg, lv->name, &lv->lvid.id[1],
			     lv->lock_args, def_mode, flags);
}

/*
 * Check if the LV being resized is used by gfs2/ocfs2 which we
 * know allow resizing under a shared lock.
 */
static int _shared_fs_can_resize(struct logical_volume *lv)
{
	FILE *f = NULL;
	struct mntent *m;
	int ret = 0;

	if (!(f = setmntent("/etc/mtab", "r")))
		return 0;

	while ((m = getmntent(f))) {
		if (!strcmp(m->mnt_type, "gfs2") || !strcmp(m->mnt_type, "ocfs2")) {
			/* FIXME: check if this mntent is for lv */
			ret = 1;
			break;
		}
	}
	endmntent(f);
	return ret;
}

/*
 * A special lockd_lv function is used for lvresize so that details can
 * be saved for doing cluster "refresh" at the end of the command.
 */

int lockd_lv_resize(struct cmd_context *cmd, struct logical_volume *lv,
	     const char *def_mode, uint32_t flags,
	     struct lvresize_params *lp)
{
	char lv_uuid[64] __attribute__((aligned(8)));
	char path[PATH_MAX];
	int shupdate = cmd->lockopt & LOCKOPT_SHUPDATE;
	int norefresh = cmd->lockopt & LOCKOPT_NOREFRESH;
	int rv;

	if (!vg_is_shared(lv->vg))
		return 1;

	if (!_use_lvmlockd) {
		log_error("LV in VG %s with lock_type %s requires lvmlockd.",
			  lv->vg->name, lv->vg->lock_type);
		return 0;
	}

	if (!_lvmlockd_connected)
		return 0;

	if (lv_is_lockd_sanlock_lv(lv))
		return 1;

	/*
	 * A special case for gfs2 where we want to allow lvextend
	 * of an LV that has an existing shared lock, which is normally
	 * incompatible with the ex lock required by lvextend.
	 *
	 * Check if gfs2 or ocfs2 is mounted on the LV, and enable this
	 * SH_EXISTS_OK flag if so.  Other users of the LV may not want
	 * to allow this.  --lockopt shupdate allows the shared lock in
	 * place of ex even we don't detect gfs2/ocfs2.
	 */
	if (lp->resize == LV_EXTEND) {
		if (shupdate || _shared_fs_can_resize(lv))
			flags |= LDLV_SH_EXISTS_OK;
	}

	rv = lockd_lv(cmd, lv, def_mode, flags);

	if (norefresh)
		return rv;

	/*
	 * If lockd_lv found an existing sh lock in lvmlockd and
	 * used that in place of the usual ex lock (we allowed this
	 * with SH_EXISTS_OK), then it sets this flag.
	 *
	 * We use this as a signal that we should try to refresh
	 * the LV on remote nodes through dlm/corosync at the end
	 * of the command.
	 *
	 * If lockd_lv successfully acquired the LV lock ex (did not
	 * need to make use of SH_EXISTS_OK), then we know the LV
	 * is active here only (or not active anywhere) and we
	 * don't need to do any remote refresh.
	 *
	 * lvresize --lockopt norefresh disables the remote refresh.
	 */
	if (cmd->lockd_lv_sh_for_ex) {
		if (!id_write_format(&lv->lvid.id[1], lv_uuid, sizeof(lv_uuid)))
			return 0;
		if (dm_snprintf(path, sizeof(path), "%s/%s/%s",
				cmd->dev_dir, lv->vg->name, lv->name) < 0) {
			log_error("LV path too long for lvmlockd refresh.");
			return 0;
		}

		/* These will be used at the end of lvresize to do lockd_lv_refresh */
		lp->lockd_lv_refresh_path = dm_pool_strdup(cmd->mem, path);
		lp->lockd_lv_refresh_uuid = dm_pool_strdup(cmd->mem, lv_uuid);
	}

	return rv;
}

static int _init_lv_sanlock(struct cmd_context *cmd, struct volume_group *vg,
			    const char *lv_name, struct id *lv_id,
			    const char *last_args,
			    const char **lock_args_ret)
{
	char lv_uuid[64] __attribute__((aligned(8)));
	daemon_reply reply;
	const char *reply_str;
	const char *lv_lock_args = NULL;
	int result;
	int ret;

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	if (!id_write_format(lv_id, lv_uuid, sizeof(lv_uuid)))
		return_0;

	log_debug("lockd init_lv %s %s", lv_name, lv_uuid);

	reply = _lockd_send("init_lv",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"lv_name = %s", lv_name,
				"lv_uuid = %s", lv_uuid,
				"prev_lv_args = %s", last_args ? last_args : "none",
				"vg_lock_type = %s", "sanlock",
				"vg_lock_args = %s", vg->lock_args,
				NULL);

	if (!_lockd_result(cmd, "init_lv", reply, &result, NULL, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	if (result == -EEXIST) {
		log_error("Lock already exists for LV %s/%s", vg->name, lv_name);
		goto out;
	}

	if (result == -EMSGSIZE) {
		/*
		 * No space on the lvmlock lv for a new lease, this should be
		 * detected by handle_sanlock_lv() called before.
		 */
		log_error("No sanlock space for lock for LV %s/%s", vg->name, lv_name);
		goto out;
	}

	if (!ret) {
		log_error("_init_lv_sanlock lvmlockd result %d", result);
		goto out;
	}

	if (!(reply_str = daemon_reply_str(reply, "lv_lock_args", NULL))) {
		log_error("lv_lock_args not returned");
		ret = 0;
		goto out;
	}

	if (!(lv_lock_args = dm_pool_strdup(cmd->mem, reply_str))) {
		log_error("lv_lock_args allocation failed");
		ret = 0;
	}
out:
	daemon_reply_destroy(reply);

	*lock_args_ret = lv_lock_args;
	return ret;
}

static int _free_lv(struct cmd_context *cmd, struct volume_group *vg,
		    const char *lv_name, char *lv_uuid, const char *lock_args)
{
	daemon_reply reply;
	int result;
	int ret;

	if (cmd->lockd_lv_disable) {
		log_debug("lockd free LV disabled %s/%s %s lock_args %s", vg->name, lv_name, lv_uuid, lock_args ?: "none");
		return 1;
	}

	if (!_use_lvmlockd) {
		log_error("LV %s/%s free lock in shared VG: lvmlockd is required.", vg->name, lv_name);
		return 0;
	}
	if (!_lvmlockd_connected) {
		log_error("LV %s/%s free lock in shared VG: lvmlockd is not connected.", vg->name, lv_name);
		return 0;
	}

	log_debug("lockd free LV %s/%s %s lock_args %s", vg->name, lv_name, lv_uuid, lock_args ?: "none");

	reply = _lockd_send("free_lv",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"lv_name = %s", lv_name,
				"lv_uuid = %s", lv_uuid,
				"vg_lock_type = %s", vg->lock_type,
				"vg_lock_args = %s", vg->lock_args,
				"lv_lock_args = %s", lock_args ?: "none",
				NULL);

	if (!_lockd_result(cmd, "free_lv", reply, &result, NULL, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	if (!ret) {
		if (result == -ENOLS)
			log_error("LV %s/%s free lock in shared VG: lockspace not started", vg->name, lv_name);
		else
			log_error("LV %s/%s free lock in shared VG: lvmlockd error %d.", vg->name, lv_name, result);
	}

	daemon_reply_destroy(reply);

	return ret;
}

int lockd_init_lv_args(struct cmd_context *cmd, struct volume_group *vg,
		       struct logical_volume *lv,
		       const char *lock_type,
		       const char *last_args,
		       const char **lock_args)
{
	if (!lock_type)
		return 1;
	if (!strcmp(lock_type, "dlm"))
		*lock_args = "dlm";
	else if (!strcmp(lock_type, "idm"))
		*lock_args = "idm";
	else if (!strcmp(lock_type, "sanlock"))
		return _init_lv_sanlock(cmd, vg, lv->name, &lv->lvid.id[1], last_args, lock_args);
	return 1;
}

/*
 * lvcreate
 *
 * An LV created in a lockd VG inherits the lock_type of the VG.  In some
 * cases, e.g. thin LVs, this function may decide that the LV should not be
 * given a lock, in which case it sets lp lock_args to NULL, which will cause
 * the LV to not have lock_args set in its metadata.  A lockd_lv() request on
 * an LV with no lock_args will do nothing (unless the LV type causes the lock
 * request to be directed to another LV with a lock, e.g. to the thin pool LV
 * for thin LVs.)
 */

int lockd_init_lv(struct cmd_context *cmd, struct volume_group *vg, struct logical_volume *lv,
		  struct lvcreate_params *lp)
{
	int lock_type_num = get_lock_type_from_string(vg->lock_type);

	switch (lock_type_num) {
	case LOCK_TYPE_NONE:
	case LOCK_TYPE_CLVM:
		return 1;
	case LOCK_TYPE_SANLOCK:
	case LOCK_TYPE_DLM:
	case LOCK_TYPE_IDM:
		break;
	default:
		log_error("lockd_init_lv: unknown lock_type.");
		return 0;
	}

	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	if (!lp->needs_lockd_init) {
		lv->lock_args = NULL;
		return 1;

	} else if (seg_is_cache_pool(lp)) {
		/*
		 * A cache pool does not use a lockd lock because it cannot be
		 * used by itself.  When a cache pool is attached to an actual
		 * LV, the lockd lock for that LV covers the LV and the cache
		 * pool attached to it.
		 */
		lv->lock_args = NULL;
		return 1;

	} else if (!seg_is_thin_volume(lp) && lp->snapshot) {
		/*
		 * COW snapshots are associated with their origin LV,
		 * and only the origin LV needs its own lock, which
		 * represents itself and all associated cow snapshots.
		 */
		lv->lock_args = NULL;
		return 1;

	} else if (seg_is_thin(lp)) {
		if (cmd->lockd_creating_thin_volume) {
			/* do nothing */
			return 1;
		}

		if (!cmd->lockd_creating_thin_pool) {
			/* can this happen? */
			log_error("lockd_init_lv thin invalid without lockd_creating_thin_pool");
			return 0;
		}

		/* create a new lock for a new thin pool */
		log_debug("lockd_init_lv creating new lock for thin pool");

	} else if (seg_is_vdo(lp)) {

		/*
		 * A vdo lv is being created in a vdo pool.  The vdo lv does
		 * not have its own lock, the lock of the vdo pool is used, and
		 * the vdo pool needs to be locked to create a vdo lv in it.
		 */
		lv->lock_args = NULL;
		return 1;

	} else {
		/* Creating a normal lv. */
	}

	/*
	 * The LV gets its own lock, so set lock_args to non-NULL.
	 *
	 * Waiting to do this last step until vg_write() avoids the need to
	 * revert the sanlock allocation if the lvcreate function isn't
	 * completed.
	 */

	 return lockd_init_lv_args(cmd, vg, lv, vg->lock_type, NULL, &lv->lock_args);
}

/* lvremove */

struct free_lv_info {
	struct dm_list list;
	char *uuid;
	char *name;
	char *args;
};

void lockd_free_removed_lvs(struct cmd_context *cmd, struct volume_group *vg, int remove_success)
{
	struct free_lv_info *fli;

	/*
	 * If lvremove has decided to remove none of the LVs, this will be 0
	 * and we don't free any of the locks.
	 */
	if (remove_success) {
		dm_list_iterate_items(fli, &vg->lockd_free_lvs) {
			if (!_free_lv(cmd, vg, fli->name, fli->uuid, fli->args))
				log_error("Failed to free lock for LV %s/%s in lvmlockd.", vg->name, fli->name);
		}
	}
	vg->needs_lockd_free_lvs = 0;
	dm_list_init(&vg->lockd_free_lvs);
}

/*
 * The LV lock will be freed later by lockd_free_removed_lvs() if the lvremove
 * command decides to go ahead and remove the LV.  If lvremove finds that it
 * cannot remove one the LVs that has been requested for removal, then it will
 * remove none of the LVs, and lockd_free_removed_lvs() will be called with
 * remove_success == 0, and it will not free any of the LV locks.
 */
void lockd_free_lv_queue(struct cmd_context *cmd, struct volume_group *vg,
			 const char *lv_name, struct id *lv_id, const char *lock_args)
{
	struct free_lv_info *fli;
	char lv_uuid[64] __attribute__((aligned(8)));

	switch (get_lock_type_from_string(vg->lock_type)) {
	case LOCK_TYPE_NONE:
	case LOCK_TYPE_CLVM:
		return;
	case LOCK_TYPE_DLM:
	case LOCK_TYPE_SANLOCK:
	case LOCK_TYPE_IDM:
		if (!lock_args)
			return;
		break;
	default:
		log_error("lockd_free_lv_queue: unknown lock_type.");
		return;
	}

	if (!id_write_format(lv_id, lv_uuid, sizeof(lv_uuid)))
		return;

	/* save lv info to send the free_lv messages later */
	if (!(fli = dm_pool_zalloc(vg->vgmem, sizeof(*fli))))
		return;
	if (!(fli->uuid = dm_pool_strdup(vg->vgmem, lv_uuid)))
		return;
	if (!(fli->name = dm_pool_strdup(vg->vgmem, lv_name)))
		return;
	if (!(fli->args = dm_pool_strdup(vg->vgmem, lock_args)))
		return;
	dm_list_add(&vg->lockd_free_lvs, &fli->list);
	vg->needs_lockd_free_lvs = 1;
}

int lockd_free_lv(struct cmd_context *cmd, struct volume_group *vg,
		  const char *lv_name, struct id *lv_id, const char *lock_args)
{
	char lv_uuid[64] __attribute__((aligned(8)));

	if (!id_write_format(lv_id, lv_uuid, sizeof(lv_uuid)))
		return_0;

	switch (get_lock_type_from_string(vg->lock_type)) {
	case LOCK_TYPE_NONE:
	case LOCK_TYPE_CLVM:
		return 1;
	case LOCK_TYPE_DLM:
	case LOCK_TYPE_SANLOCK:
	case LOCK_TYPE_IDM:
		if (!lock_args)
			return 1;
		return _free_lv(cmd, vg, lv_name, lv_uuid, lock_args);
	default:
		log_error("lockd_free_lv: unknown lock_type.");
		return 0;
	}
}

int lockd_rename_vg_before(struct cmd_context *cmd, struct volume_group *vg)
{
	daemon_reply reply;
	int result;
	int ret;

	if (!vg_is_shared(vg))
		return 1;
	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	if (lvs_in_vg_activated(vg)) {
		log_error("LVs must be inactive before vgrename.");
		return 0;
	}

	/* Check that no LVs are active on other hosts. */
	if (!_lockd_all_lvs(cmd, vg)) {
		log_error("Cannot rename VG %s with active LVs", vg->name);
		return 0;
	}

	/*
	 * lvmlockd:
	 * checks for other hosts in lockspace
	 * leaves the lockspace
	 */

	reply = _lockd_send("rename_vg_before",
			"pid = " FMTd64, (int64_t) getpid(),
			"vg_name = %s", vg->name,
			"vg_lock_type = %s", vg->lock_type,
			"vg_lock_args = %s", vg->lock_args,
			NULL);

	if (!_lockd_result(cmd, "rename_vg_before", reply, &result, NULL, NULL)) {
		ret = 0;
	} else {
		ret = (result < 0) ? 0 : 1;
	}

	daemon_reply_destroy(reply);

	/* Other hosts have not stopped the lockspace. */
	if (result == -EBUSY) {
		log_error("Lockspace for \"%s\" not stopped on other hosts", vg->name);
		return 0;
	}
	
	if (!ret) {
		log_error("lockd_rename_vg_before lvmlockd result %d", result);
		return 0;
	}

	if (!strcmp(vg->lock_type, "sanlock")) {
		log_debug("lockd_rename_vg_before deactivate sanlock lv");
		_deactivate_sanlock_lv(cmd, vg);
	}

	return 1;
}

int lockd_rename_vg_final(struct cmd_context *cmd, struct volume_group *vg, int success)
{
	daemon_reply reply;
	int result;
	int ret;

	if (!vg_is_shared(vg))
		return 1;
	if (!_use_lvmlockd)
		return 0;
	if (!_lvmlockd_connected)
		return 0;

	if (!success) {
		/*
		 * Depending on the problem that caused the rename to
		 * fail, it may make sense to not restart the VG here.
		 */
		if (!lockd_start_vg(cmd, vg, NULL))
			log_error("Failed to restart VG %s lockspace.", vg->name);
		return 1;
	}

	if (!strcmp(vg->lock_type, "sanlock")) {
		if (!_activate_sanlock_lv(cmd, vg))
			return 0;

		/*
		 * lvmlockd needs to rewrite the leases on disk
		 * with the new VG (lockspace) name.
		 */
		reply = _lockd_send("rename_vg_final",
				"pid = " FMTd64, (int64_t) getpid(),
				"vg_name = %s", vg->name,
				"vg_lock_type = %s", vg->lock_type,
				"vg_lock_args = %s", vg->lock_args,
				NULL);

		if (!_lockd_result(cmd, "rename_vg_final", reply, &result, NULL, NULL)) {
			ret = 0;
		} else {
			ret = (result < 0) ? 0 : 1;
		}
	
		daemon_reply_destroy(reply);

		if (!ret) {
			/*
			 * The VG has been renamed on disk, but renaming the
			 * sanlock leases failed.  Cleaning this up can
			 * probably be done by converting the VG to lock_type
			 * none, then converting back to sanlock.
			 */
			log_error("lockd_rename_vg_final lvmlockd result %d", result);
			return 0;
		}
	}

	if (!lockd_start_vg(cmd, vg, NULL))
		log_error("Failed to start VG %s lockspace.", vg->name);

	return 1;
}

const char *lockd_running_lock_type(struct cmd_context *cmd, int *found_multiple)
{
	daemon_reply reply;
	const char *lock_type = NULL;
	int result;

	if (!_use_lvmlockd)
		return NULL;
	if (!_lvmlockd_connected)
		return NULL;

	reply = _lockd_send("running_lm",
			"pid = " FMTd64, (int64_t) getpid(),
			NULL);

	if (!_lockd_result(cmd, "running_lm", reply, &result, NULL, NULL)) {
		log_error("Failed to get result from lvmlockd");
		goto out;
	}

	switch (result) {
	case -EXFULL:
		*found_multiple = 1;
		break;
	case -ENOLCK:
		break;
	case LOCK_TYPE_SANLOCK:
		log_debug("lvmlockd found sanlock");
		lock_type = "sanlock";
		break;
	case LOCK_TYPE_DLM:
		log_debug("lvmlockd found dlm");
		lock_type = "dlm";
		break;
	case LOCK_TYPE_IDM:
		log_debug("lvmlockd found idm");
		lock_type = "idm";
		break;
	default:
		log_error("Failed to find a running lock manager.");
		break;
	}
out:
	daemon_reply_destroy(reply);

	return lock_type;
}

/* Some LV types have no lock. */

int lockd_lv_uses_lock(struct logical_volume *lv)
{
	if (!lv_is_visible(lv))
		return 0;

	if (lv_is_thin_volume(lv))
		return 0;

	if (lv_is_thin_pool_data(lv))
		return 0;

	if (lv_is_thin_pool_metadata(lv))
		return 0;

	if (lv_is_pool_metadata_spare(lv))
		return 0;

	if (lv_is_vdo(lv))
		return 0;

	if (lv_is_vdo_pool_data(lv))
		return 0;

	if (lv_is_cache_vol(lv))
		return 0;

	if (lv_is_cache_pool(lv))
		return 0;

	if (lv_is_cache_pool_data(lv))
		return 0;

	if (lv_is_cache_pool_metadata(lv))
		return 0;

	if (lv_is_cow(lv))
		return 0;

	if (lv_is_snapshot(lv))
		return 0;

	/* FIXME: lv_is_virtual_origin ? */

	if (lv_is_lockd_sanlock_lv(lv))
		return 0;

	if (lv_is_mirror_image(lv))
		return 0;

	if (lv_is_mirror_log(lv))
		return 0;

	if (lv_is_raid_image(lv))
		return 0;

	if (lv_is_raid_metadata(lv))
		return 0;

	return 1;
}

/*
 * send lvmlockd a request to use libdlmcontrol dlmc_run_start/dlmc_run_check
 * to run a command on all nodes running dlm_controld:
 * lvm lvchange --refresh --nolocking <path>
 */

int lockd_lv_refresh(struct cmd_context *cmd, struct lvresize_params *lp)
{
	daemon_reply reply;
	char *lv_uuid = lp->lockd_lv_refresh_uuid;
	char *path = lp->lockd_lv_refresh_path;
	int result;

	if (!lv_uuid || !path)
		return 1;

	log_warn("Refreshing LV %s on other hosts...", path);

	reply = _lockd_send("refresh_lv",
				"pid = " FMTd64, (int64_t) getpid(),
				"opts = %s", "none",
				"lv_uuid = %s", lv_uuid,
				"path = %s", path,
				NULL);

	if (!_lockd_result(cmd, "refresh_lv", reply, &result, NULL, NULL)) {
		/* No result from lvmlockd, it is probably not running. */
		log_error("LV refresh failed for LV %s", path);
		return 0;
	}
	daemon_reply_destroy(reply);

	if (result < 0) {
		log_error("Failed to refresh LV on all hosts.");
		log_error("Manual lvchange --refresh required on all hosts for %s.", path);
		return 0;
	}
	return 1;
}

#define MAX_LOCKOPT 16

void lockd_lockopt_get_flags(const char *str, uint32_t *flags)
{
	char buf[PATH_MAX];
	char *argv[MAX_LOCKOPT];
	int argc;
	int i;

	if (!str)
		return;

	dm_strncpy(buf, str, sizeof(buf));

	split_line(buf, &argc, argv, MAX_LOCKOPT, ',');

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "force"))
			*flags |= LOCKOPT_FORCE;
		else if (!strcmp(argv[i], "shupdate"))
			*flags |= LOCKOPT_SHUPDATE;
		else if (!strcmp(argv[i], "norefresh"))
			*flags |= LOCKOPT_NOREFRESH;
		else if (!strcmp(argv[i], "skipgl"))
			*flags |= LOCKOPT_SKIPGL;
		else if (!strcmp(argv[i], "skipvg"))
			*flags |= LOCKOPT_SKIPVG;
		else if (!strcmp(argv[i], "skiplv"))
			*flags |= LOCKOPT_SKIPLV;
		else if (!strcmp(argv[i], "auto"))
			*flags |= LOCKOPT_AUTO;
		else if (!strcmp(argv[i], "nowait"))
			*flags |= LOCKOPT_NOWAIT;
		else if (!strcmp(argv[i], "autonowait"))
			*flags |= LOCKOPT_AUTONOWAIT;
		else if (!strcmp(argv[i], "adoptls"))
			*flags |= LOCKOPT_ADOPTLS;
		else if (!strcmp(argv[i], "adoptgl"))
			*flags |= LOCKOPT_ADOPTGL;
		else if (!strcmp(argv[i], "adoptvg"))
			*flags |= LOCKOPT_ADOPTVG;
		else if (!strcmp(argv[i], "adoptlv"))
			*flags |= LOCKOPT_ADOPTLV;
		else if (!strcmp(argv[i], "adopt"))
			*flags |= LOCKOPT_ADOPT;
		else if (!strcmp(argv[i], "nodelay"))
			*flags |= LOCKOPT_NODELAY;
		else if (!strcmp(argv[i], "repair"))
			*flags |= LOCKOPT_REPAIR;
		else if (!strcmp(argv[i], "repairgl"))
			*flags |= LOCKOPT_REPAIRGL;
		else if (!strcmp(argv[i], "repairvg"))
			*flags |= LOCKOPT_REPAIRVG;
		else if (!strcmp(argv[i], "repairlv"))
			*flags |= LOCKOPT_REPAIRLV;
		else
			log_warn("Ignoring unknown lockopt value: %s", argv[i]);
	}
}
