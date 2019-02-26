/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2012 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

#include "lib/cache/lvmcache.h"
#include "lib/metadata/metadata.h"
#include "lib/label/hints.h"

#include <dirent.h>
#include <sys/file.h>

struct pvscan_params {
	int new_pvs_found;
	int pvs_found;
	uint64_t size_total;
	uint64_t size_new;
	unsigned pv_max_name_len;
	unsigned vg_max_name_len;
	unsigned pv_tmp_namelen;
	char *pv_tmp_name;
};

struct pvscan_aa_params {
	unsigned int activate_errors;
};


static const char *_pvs_online_dir = DEFAULT_RUN_DIR "/pvs_online";
static const char *_vgs_online_dir = DEFAULT_RUN_DIR "/vgs_online";
static const char *_online_file = DEFAULT_RUN_DIR "/pvs_online_lock";
static int _online_fd = -1;

static int _lock_online(int mode, int nonblock)
{
	int fd;
	int op = mode;
	int ret;

	if (nonblock)
		op |= LOCK_NB;

	if (_online_fd != -1) {
		log_warn("lock_online existing fd %d", _online_fd);
		return 0;
	}

	fd = open(_online_file, O_RDWR | S_IRUSR | S_IWUSR);
	if (fd < 0) {
		log_debug("lock_online open errno %d", errno);
		return 0;
	}

	ret = flock(fd, op);
	if (!ret) {
		_online_fd = fd;
		return 1;
	}

	if (close(fd))
		stack;
	return 0;
}

static void _unlock_online(void)
{
	int ret;

	if (_online_fd == -1) {
		log_warn("unlock_online no existing fd");
		return;
	}

	ret = flock(_online_fd, LOCK_UN);
	if (ret)
		log_warn("unlock_online flock errno %d", errno);

	if (close(_online_fd))
		stack;
	_online_fd = -1;
}

static int _pvscan_display_pv(struct cmd_context *cmd,
				  struct physical_volume *pv,
				  struct pvscan_params *params)
{
	/* XXXXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXXXX */
	char uuid[40] __attribute__((aligned(8)));
	const unsigned suffix_len = sizeof(uuid) + 10;
	unsigned pv_len;
	const char *pvdevname = pv_dev_name(pv);

	/* short listing? */
	if (arg_is_set(cmd, short_ARG)) {
		log_print_unless_silent("%s", pvdevname);
		return ECMD_PROCESSED;
	}

	if (!params->pv_max_name_len) {
		lvmcache_get_max_name_lengths(cmd, &params->pv_max_name_len, &params->vg_max_name_len);

		params->pv_max_name_len += 2;
		params->vg_max_name_len += 2;
		params->pv_tmp_namelen = params->pv_max_name_len + suffix_len;

		if (!(params->pv_tmp_name = dm_pool_alloc(cmd->mem, params->pv_tmp_namelen)))
			return ECMD_FAILED;
	}

	pv_len = params->pv_max_name_len;
	memset(params->pv_tmp_name, 0, params->pv_tmp_namelen);

	if (arg_is_set(cmd, uuid_ARG)) {
		if (!id_write_format(&pv->id, uuid, sizeof(uuid))) {
			stack;
			return ECMD_FAILED;
		}

		if (dm_snprintf(params->pv_tmp_name, params->pv_tmp_namelen, "%-*s with UUID %s",
				params->pv_max_name_len - 2, pvdevname, uuid) < 0) {
			log_error("Invalid PV name with uuid.");
			return ECMD_FAILED;
		}
		pvdevname = params->pv_tmp_name;
		pv_len += suffix_len;
	}

	if (is_orphan(pv))
		log_print_unless_silent("PV %-*s    %-*s %s [%s]",
					pv_len, pvdevname,
					params->vg_max_name_len, " ",
					pv->fmt ? pv->fmt->name : "    ",
					display_size(cmd, pv_size(pv)));
	else if (pv_status(pv) & EXPORTED_VG)
		log_print_unless_silent("PV %-*s  is in exported VG %s [%s / %s free]",
					pv_len, pvdevname, pv_vg_name(pv),
					display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
					display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
	else
		log_print_unless_silent("PV %-*s VG %-*s %s [%s / %s free]",
					pv_len, pvdevname,
					params->vg_max_name_len, pv_vg_name(pv),
					pv->fmt ? pv->fmt->name : "    ",
					display_size(cmd, (uint64_t) pv_pe_count(pv) * pv_pe_size(pv)),
					display_size(cmd, (uint64_t) (pv_pe_count(pv) - pv_pe_alloc_count(pv)) * pv_pe_size(pv)));
	return ECMD_PROCESSED;
}

static int _pvscan_display_single(struct cmd_context *cmd, struct volume_group *vg,
			  struct physical_volume *pv, struct processing_handle *handle)
{
	struct pvscan_params *params = (struct pvscan_params *)handle->custom_handle;

	if ((arg_is_set(cmd, exported_ARG) && !(pv_status(pv) & EXPORTED_VG)) ||
	    (arg_is_set(cmd, novolumegroup_ARG) && (!is_orphan(pv)))) {
		return ECMD_PROCESSED;

	}

	params->pvs_found++;

	if (is_orphan(pv)) {
		params->new_pvs_found++;
		params->size_new += pv_size(pv);
		params->size_total += pv_size(pv);
	} else {
		params->size_total += (uint64_t) pv_pe_count(pv) * pv_pe_size(pv);
	}

	_pvscan_display_pv(cmd, pv, params);
	return ECMD_PROCESSED;
}

int pvscan_display_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_params params = { 0 };
	struct processing_handle *handle = NULL;
	int ret;

	if (arg_is_set(cmd, novolumegroup_ARG) && arg_is_set(cmd, exported_ARG)) {
		log_error("Options -e and -n are incompatible");
		return EINVALID_CMD_LINE;
	}

	if (arg_is_set(cmd, exported_ARG) || arg_is_set(cmd, novolumegroup_ARG))
		log_warn("WARNING: only considering physical volumes %s",
			  arg_is_set(cmd, exported_ARG) ?
			  "of exported volume group(s)" : "in no volume group");

	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_WRITE, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
	}

	handle->custom_handle = &params;

	ret = process_each_pv(cmd, argc, argv, NULL, 0, 0, handle, _pvscan_display_single);

	if (!params.pvs_found)
		log_print_unless_silent("No matching physical volumes found");
	else
		log_print_unless_silent("Total: %d [%s] / in use: %d [%s] / in no VG: %d [%s]",
					params.pvs_found,
					display_size(cmd, params.size_total),
					params.pvs_found - params.new_pvs_found,
					display_size(cmd, (params.size_total - params.size_new)),
					params.new_pvs_found, display_size(cmd, params.size_new));

out:
	unlock_vg(cmd, NULL, VG_GLOBAL);
	destroy_processing_handle(cmd, handle);

	return ret;
}

static char *_vgname_in_pvid_file_buf(char *buf)
{
	char *p, *n;

	/*
	 * file contains:
	 * <major>:<minor>\n
	 * vg:<vgname>\n\0
	 */

	if (!(p = strchr(buf, '\n')))
		return NULL;

	p++; /* skip \n */

	if (*p && !strncmp(p, "vg:", 3)) {
		if ((n = strchr(p, '\n')))
			*n = '\0';
		return p + 3;
	}
	return NULL;
}

#define MAX_PVID_FILE_SIZE 512

/*
 * When a PV goes offline, remove the vg online file for that VG
 * (even if other PVs for the VG are still online).  This means
 * that the vg will be activated again when it becomes complete.
 */

static void _online_vg_file_remove(const char *vgname)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s", _vgs_online_dir, vgname) < 0) {
		log_error("Path %s/%s is too long.", _vgs_online_dir, vgname);
		return;
	}

	log_debug("Unlink vg online: %s", path);

	if (unlink(path))
		log_sys_debug("unlink", path);
}

/*
 * When a device goes offline we only know its major:minor, not its PVID.
 * Since the dev isn't around, we can't read it to get its PVID, so we have to
 * read the PVID files to find the one containing this major:minor and remove
 * that one. This means that the PVID files need to contain the devno's they
 * were created from.
 */

static void _online_pvid_file_remove_devno(int major, int minor)
{
	char path[PATH_MAX];
	char buf[MAX_PVID_FILE_SIZE];
	char buf_in[MAX_PVID_FILE_SIZE];
	char *vgname = NULL;
	DIR *dir;
	struct dirent *de;
	int fd, rv;

	memset(buf, 0, sizeof(buf));
	snprintf(buf, sizeof(buf), "%d:%d\n", major, minor);

	log_debug("Remove pv online devno %d:%d", major, minor);

	if (!(dir = opendir(_pvs_online_dir)))
		return;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, de->d_name);

		fd = open(path, O_RDONLY);
		if (fd < 0) {
			log_debug("Failed to open %s", path);
			continue;
		}

		memset(buf_in, 0, sizeof(buf_in));

		rv = read(fd, buf_in, sizeof(buf_in));
		if (close(fd))
			log_sys_debug("close", path);
		if (!rv || rv < 0) {
			log_debug("Failed to read %s", path);
			continue;
		}

		if (!strncmp(buf, buf_in, strlen(buf))) {
			log_debug("Unlink pv online %s", path);
			if (unlink(path))
				log_sys_debug("unlink", path);

			/* vgname points to an offset in buf_in */
			if ((vgname = _vgname_in_pvid_file_buf(buf_in)))
				_online_vg_file_remove(vgname);
			else
				log_debug("No vgname in pvid file");

			break;
		}
	}
	if (closedir(dir))
		log_sys_debug("closedir", _pvs_online_dir);
}

static void _online_files_remove(const char *dirpath)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *de;

	if (!(dir = opendir(dirpath)))
		return;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name);
		if (unlink(path))
			log_sys_debug("unlink", path);
	}
	if (closedir(dir))
		log_sys_debug("closedir", dirpath);
}

static int _online_pvid_file_create(struct device *dev, const char *vgname)
{
	char path[PATH_MAX];
	char buf[MAX_PVID_FILE_SIZE];
	int major, minor;
	int fd;
	int rv;
	int len;
	int len1 = 0;
	int len2 = 0;

	memset(buf, 0, sizeof(buf));

	major = (int)MAJOR(dev->dev);
	minor = (int)MINOR(dev->dev);

	if (dm_snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, dev->pvid) < 0) {
		log_error("Path %s/%s is too long.", _pvs_online_dir, dev->pvid);
		return 0;
	}

	if ((len1 = dm_snprintf(buf, sizeof(buf), "%d:%d\n", major, minor)) < 0) {
		log_error("Cannot create online pv file for %d:%d.", major, minor);
		return 0;
	}

	if (vgname) {
		if ((len2 = dm_snprintf(buf + len1, sizeof(buf) - len1, "vg:%s\n", vgname)) < 0) {
			log_warn("Incomplete online pv file for %d:%d vg %s.", major, minor, vgname);
			/* can still continue without vgname */
			len2 = 0;
		}
	}

	len = len1 + len2;

	log_debug("Create pv online: %s %d:%d %s", path, major, minor, dev_name(dev));

	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		log_error("Failed to open create %s: %d", path, errno);
		return 0;
	}

	while (len > 0) {
		rv = write(fd, buf, len);
		if (rv < 0) {
			log_error("Failed to write fd %d buf %s dev %s to %s: %d",
			          fd, buf, dev_name(dev), path, errno);
			if (close(fd))
				log_sys_debug("close", path);
			return 0;
		}
		len -= rv;
	}

	/* We don't care about syncing, these files are not even persistent. */

	if (close(fd))
		log_sys_debug("close", path);

	return 1;
}

static int _online_pvid_file_exists(const char *pvid)
{
	char path[PATH_MAX];
	struct stat buf;
	int rv;

	memset(path, 0, sizeof(path));

	snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, pvid);

	log_debug("Check pv online: %s", path);

	rv = stat(path, &buf);
	if (!rv) {
		log_debug("Check pv online: yes");
		return 1;
	}
	log_debug("Check pv online: no");
	return 0;
}

static void _online_dir_setup(void)
{
	struct stat st;
	int rv;

	if (!stat(DEFAULT_RUN_DIR, &st))
		goto do_pvs;

	log_debug("Creating run_dir.");
	dm_prepare_selinux_context(DEFAULT_RUN_DIR, S_IFDIR);
	rv = mkdir(DEFAULT_RUN_DIR, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(DEFAULT_RUN_DIR, &st))
		log_error("Failed to create %s %d", DEFAULT_RUN_DIR, errno);

do_pvs:
	if (!stat(_pvs_online_dir, &st))
		goto do_vgs;

	log_debug("Creating pvs_online_dir.");
	dm_prepare_selinux_context(_pvs_online_dir, S_IFDIR);
	rv = mkdir(_pvs_online_dir, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(_pvs_online_dir, &st))
		log_error("Failed to create %s %d", _pvs_online_dir, errno);

do_vgs:
	if (!stat(_vgs_online_dir, &st))
		return;

	log_debug("Creating vgs_online_dir.");
	dm_prepare_selinux_context(_vgs_online_dir, S_IFDIR);
	rv = mkdir(_vgs_online_dir, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(_vgs_online_dir, &st))
		log_error("Failed to create %s %d", _vgs_online_dir, errno);
}

static void _online_file_setup(void)
{
	FILE *fp;
	struct stat st;

	if (!stat(_online_file, &st))
		return;

	if (!(fp = fopen(_online_file, "w"))) {
		log_error("Failed to create %s %d", _online_file, errno);
		return;
	}
	if (fclose(fp))
		stack;
}

static int _online_pvid_files_missing(void)
{
	DIR *dir;
	struct dirent *de;

	if (!(dir = opendir(_pvs_online_dir))) {
		log_debug("Failed to open %s", _pvs_online_dir);
		return 1;
	}

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;
		if (closedir(dir))
			log_sys_debug("closedir", _pvs_online_dir);
		return 0;
	}
	if (closedir(dir))
		log_sys_debug("closedir", _pvs_online_dir);
	return 1;
}

static int _online_pv_found(struct cmd_context *cmd,
			    struct device *dev, struct dm_list *dev_args,
			    struct volume_group *vg,
			    struct dm_list *found_vgnames)
{
	struct pv_list *pvl;
	int pvids_not_online = 0;
	int dev_args_in_vg = 0;

	/*
	 * Create file named for pvid to record this PV is online.
	 */

	if (!_online_pvid_file_create(dev, vg ? vg->name : NULL))
		return_0;

	if (!vg || !found_vgnames) {
		log_print("pvscan[%d] PV %s online.", getpid(), dev_name(dev));
		return 1;
	}

	/*
	 * Check if all the PVs for this VG are online.  This is only
	 * needed when autoactivating the VG which should be run only
	 * when the VG is complete.  If the arrival of this dev completes
	 * the VG, then we want to activate the VG.
	 */

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!_online_pvid_file_exists((const char *)&pvl->pv->id.uuid))
			pvids_not_online++;

		/* Check if one of the devs on the command line is in this VG. */
		if (dev_args && dev_in_device_list(pvl->pv->dev, dev_args))
			dev_args_in_vg = 1;
	}

	/*
	 * Return if we did not find an online file for one of the PVIDs
	 * in the VG, which means the VG is not yet complete.
	 */

	if (pvids_not_online) {
		log_print("pvscan[%d] PV %s online, VG %s incomplete (need %d).",
			  getpid(), dev_name(dev), vg->name, pvids_not_online);
		return 1;
	}

	log_print("pvscan[%d] PV %s online, VG %s is complete.", getpid(), dev_name(dev), vg->name);

	/*
	 * When all PVIDs from the VG are online, then add vgname to
	 * found_vgnames.
	 */

	log_debug("online dev %s completes VG %s.", dev_name(dev), vg->name);

	/*
	 * We either want to return all complete VGs that are found on any devs
	 * we are scanning, or we want to return complete VGs only when they
	 * contain PVs that were specified on the command line.
	 */

	if (!dev_args || dev_args_in_vg) {
		log_debug("online dev %s can autoactivate VG %s", dev_name(dev), vg->name);
		if (!str_list_add(cmd->mem, found_vgnames, dm_pool_strdup(cmd->mem, vg->name)))
			stack;
	}

	return 1;
}

struct _pvscan_baton {
	struct cmd_context *cmd;
	struct volume_group *vg;
	struct format_instance *fid;
};

static int _online_pvscan_single(struct metadata_area *mda, void *baton)
{
	struct _pvscan_baton *b = baton;
	struct volume_group *vg;

	if (mda_is_ignored(mda) ||
	    !(vg = mda->ops->vg_read(b->fid, "", mda, NULL, NULL)))
		return 1;

	/* FIXME Also ensure contents match etc. */
	if (!b->vg || vg->seqno > b->vg->seqno)
		b->vg = vg;
	else if (b->vg)
		release_vg(vg);

	return 1;
}

/*
 * disable_remove is 1 when resetting the online state, which begins with
 * removing all pvid files, and then creating new pvid files for PVs that
 * are found, so we don't need to try to remove pvid files here when a PV
 * is not found on a device.
 */

static int _online_pvscan_one(struct cmd_context *cmd, struct device *dev,
			      struct dm_list *dev_args,
			      struct dm_list *found_vgnames,
			      int disable_remove,
			      const char **pvid_without_metadata)
{
	struct lvmcache_info *info;
	struct _pvscan_baton baton;
	const struct format_type *fmt;
	/* Create a dummy instance. */
	struct format_instance_ctx fic = { .type = 0 };
	uint32_t ext_version;
	uint32_t ext_flags;
	int ret = 0;

	log_debug("pvscan metadata from dev %s", dev_name(dev));

	if (udev_dev_is_mpath_component(dev)) {
		log_print("pvscan[%d] ignore multipath component %s.", getpid(), dev_name(dev));
		return 1;
	}

	if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
		log_debug("No PV info found on %s for PVID %s.", dev_name(dev), dev->pvid);
		if (!disable_remove)
			_online_pvid_file_remove_devno((int)MAJOR(dev->dev), (int)MINOR(dev->dev));
		return 1;
	}

	if (!lvmcache_get_label(info)) {
		log_debug("No PV label found for %s.", dev_name(dev));
		if (!disable_remove)
			_online_pvid_file_remove_devno((int)MAJOR(dev->dev), (int)MINOR(dev->dev));
		return 1;
	}

	ext_version = lvmcache_ext_version(info);
	ext_flags = lvmcache_ext_flags(info);

	if ((ext_version >= 2) && !(ext_flags & PV_EXT_USED)) {
		log_print("pvscan[%d] PV %s not used.", getpid(), dev_name(dev));
		return 1;
	}

	fmt = lvmcache_fmt(info);

	baton.cmd = cmd;
	baton.vg = NULL;
	baton.fid = fmt->ops->create_instance(fmt, &fic);

	if (!baton.fid) {
		ret = 0;
		goto_out;
	}

	lvmcache_foreach_mda(info, _online_pvscan_single, &baton);

	if (!baton.vg) {
		if (pvid_without_metadata)
			*pvid_without_metadata = dm_pool_strdup(cmd->mem, dev->pvid);
		fmt->ops->destroy_instance(baton.fid);
	}

	if (baton.vg && vg_is_shared(baton.vg)) {
		log_print("pvscan[%d] PV %s ignore shared VG.", getpid(), dev_name(dev));
		release_vg(baton.vg);
		return 1;
	}

	if (baton.vg &&
	    baton.vg->system_id && baton.vg->system_id[0] &&
	    cmd->system_id && cmd->system_id[0] &&
	    vg_is_foreign(baton.vg)) {
		log_verbose("Ignore PV %s with VG system id %s with our system id %s",
			    dev_name(dev), baton.vg->system_id, cmd->system_id);
		log_print("pvscan[%d] PV %s ignore foreign VG.", getpid(), dev_name(dev));
		release_vg(baton.vg);
		return 1;
	}

	ret = _online_pv_found(cmd, dev, dev_args, baton.vg, found_vgnames);

	release_vg(baton.vg);
out:
	return ret;
}

/*
 * dev_args is the list of devices that were specified on the
 * pvscan command line.
 *
 * . When dev_args is NULL, any complete VGs that are found will
 *   be returned in found_vgnames.
 *
 * . When dev_args is set, then complete VGs that that contain
 *   devs in dev_args will be returned in found_vgnames.
 */

static void _online_pvscan_all_devs(struct cmd_context *cmd,
				    struct dm_list *found_vgnames,
				    struct dm_list *dev_args)
{
	struct dev_iter *iter;
	struct device *dev;

	label_scan(cmd);

	if (!(iter = dev_iter_create(cmd->filter, 1))) {
		log_error("dev_iter creation failed");
		return;
	}

	while ((dev = dev_iter_get(cmd, iter))) {
		if (sigint_caught()) {
			stack;
			break;
		}

		if (!_online_pvscan_one(cmd, dev, dev_args, found_vgnames, 1, NULL)) {
			stack;
			break;
		}
	}

	dev_iter_destroy(iter);
}

static int _pvscan_aa_single(struct cmd_context *cmd, const char *vg_name,
			     struct volume_group *vg, struct processing_handle *handle)
{
	struct pvscan_aa_params *pp = (struct pvscan_aa_params *)handle->custom_handle;

	if (vg_is_clustered(vg))
		return ECMD_PROCESSED;

	if (vg_is_exported(vg))
		return ECMD_PROCESSED;

	if (vg_is_shared(vg))
		return ECMD_PROCESSED;

	log_debug("pvscan autoactivating VG %s.", vg_name);

	if (!vgchange_activate(cmd, vg, CHANGE_AAY)) {
		log_error("%s: autoactivation failed.", vg->name);
		pp->activate_errors++;
	}

	return ECMD_PROCESSED;
}

static int _online_vg_file_create(struct cmd_context *cmd, const char *vgname)
{
	char path[PATH_MAX];
	int fd;

	if (dm_snprintf(path, sizeof(path), "%s/%s", _vgs_online_dir, vgname) < 0) {
		log_error("Path %s/%s is too long.", _vgs_online_dir, vgname);
		return 0;
	}

	log_debug("Create vg online: %s", path);

	fd = open(path, O_CREAT | O_EXCL | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		log_debug("Failed to create %s: %d", path, errno);
		return 0;
	}

	/* We don't care about syncing, these files are not even persistent. */

	if (close(fd))
		log_sys_debug("close", path);

	return 1;
}

static int _pvscan_aa(struct cmd_context *cmd, struct pvscan_aa_params *pp,
		      struct dm_list *vgnames)
{
	struct processing_handle *handle = NULL;
	struct dm_str_list *sl, *sl2;
	int ret;

	if (dm_list_empty(vgnames)) {
		log_debug("No VGs to autoactivate.");
		return ECMD_PROCESSED;
	}

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		return ECMD_FAILED;
	}

	handle->custom_handle = pp;

	/*
	 * For each complete vg that can be autoactivated, see if this
	 * particular pvscan command should activate the vg.  There can be
	 * multiple concurrent pvscans for the same completed vg (when all the
	 * PVs for the VG appear at once), and we want only one of the pvscans
	 * to run the activation.  The first to create the file will do it.
	 */
	dm_list_iterate_items_safe(sl, sl2, vgnames) {
		if (!_online_vg_file_create(cmd, sl->str)) {
			log_print("pvscan[%d] VG %s skip autoactivation.", getpid(), sl->str);
			str_list_del(vgnames, sl->str);
			continue;
		}
		log_print("pvscan[%d] VG %s run autoactivation.", getpid(), sl->str);
	}

	if (dm_list_empty(vgnames)) {
		destroy_processing_handle(cmd, handle);
		return ECMD_PROCESSED;
	}

	ret = process_each_vg(cmd, 0, NULL, NULL, vgnames, READ_FOR_UPDATE, 0, handle, _pvscan_aa_single);

	destroy_processing_handle(cmd, handle);

	return ret;
}

int pvscan_cache_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_aa_params pp = { 0 };
	struct dm_list add_devs;
	struct dm_list rem_devs;
	struct dm_list vgnames;
	struct dm_list *complete_vgnames = NULL;
	struct device *dev;
	struct device_list *devl;
	const char *pv_name;
	const char *pvid_without_metadata = NULL;
	int32_t major = -1;
	int32_t minor = -1;
	int devno_args = 0;
	int all_devs;
	struct arg_value_group_list *current_group;
	dev_t devno;
	int do_activate = arg_is_set(cmd, activate_ARG);
	int add_errors = 0;
	int add_single_count = 0;
	int ret = ECMD_PROCESSED;

	dm_list_init(&add_devs);
	dm_list_init(&rem_devs);
	dm_list_init(&vgnames);

	if (do_activate)
		complete_vgnames = &vgnames;

	if (arg_is_set(cmd, major_ARG) + arg_is_set(cmd, minor_ARG))
		devno_args = 1;

	if (devno_args && (!arg_is_set(cmd, major_ARG) || !arg_is_set(cmd, minor_ARG))) {
		log_error("Both --major and --minor required to identify devices.");
		return EINVALID_CMD_LINE;
	}

	if (argc || devno_args) {
		log_verbose("pvscan devices on command line.");
		cmd->pvscan_cache_single = 1;
		all_devs = 0;
	} else {
		all_devs = 1;
	}

	if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_READ, NULL)) {
		log_error("Unable to obtain global lock.");
		return ECMD_FAILED;
	}

	_online_dir_setup();
	_online_file_setup();

	/* Creates a list of dev names from /dev, sysfs, etc; does not read any. */
	dev_cache_scan();

	/*
	 * For each device command arg (from either position or --major/--minor),
	 * decide if that device is being added to the system (a dev node exists
	 * for it in /dev), or being removed from the system (no dev node exists
	 * for it in /dev).  Create entries in add_devs/rem_devs for each arg
	 * accordingly.
	 */

	while (argc) {
		argc--;

		pv_name = *argv++;
		if (pv_name[0] == '/') {
			if (!(dev = dev_cache_get(cmd, pv_name, cmd->filter))) {
				log_debug("pvscan arg %s not found.", pv_name);
				if ((dev = dev_cache_get(cmd, pv_name, NULL))) {
					/* nothing to do for this dev name */
				} else {
					log_error("Physical Volume %s not found.", pv_name);
					ret = ECMD_FAILED;
				}
			} else {
				/*
				 * Scan device.  This dev could still be removed
				 * below if it doesn't pass other filters.
				 */
				log_debug("pvscan arg %s found.", pv_name);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&add_devs, &devl->list);
			}
		} else {
			if (sscanf(pv_name, "%d:%d", &major, &minor) != 2) {
				log_warn("WARNING: Failed to parse major:minor from %s, skipping.", pv_name);
				continue;
			}
			devno = MKDEV(major, minor);

			if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->filter))) {
				log_debug("pvscan arg %d:%d not found.", major, minor);
				if (!(dev = dm_pool_zalloc(cmd->mem, sizeof(struct device))))
					return_0;
				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				dev->dev = devno;
				devl->dev = dev;
				dm_list_add(&rem_devs, &devl->list);
			} else {
				/*
				 * Scan device.  This dev could still be removed
				 * below if it doesn't pass other filters.
				 */
				log_debug("pvscan arg %d:%d found.", major, minor);

				if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
					return_0;
				devl->dev = dev;
				dm_list_add(&add_devs, &devl->list);
			}
		}
	}

	/* Process any grouped --major --minor args */
	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		major = grouped_arg_int_value(current_group->arg_values, major_ARG, major);
		minor = grouped_arg_int_value(current_group->arg_values, minor_ARG, minor);

		if (major < 0 || minor < 0)
			continue;

		devno = MKDEV(major, minor);

		if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->filter))) {
			log_debug("pvscan arg %d:%d not found.", major, minor);
			if (!(dev = dm_pool_zalloc(cmd->mem, sizeof(struct device))))
				return_0;
			if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
				return_0;
			dev->dev = devno;
			devl->dev = dev;
			dm_list_add(&rem_devs, &devl->list);
		} else {
			log_debug("pvscan arg %d:%d found.", major, minor);

			if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
				return_0;
			devl->dev = dev;
			dm_list_add(&add_devs, &devl->list);
		}
	}

	/*
	 * No device args means rescan/regenerate/[reactivate] everything.
	 * Scan all devices when no args are given; clear all pvid
	 * files on recreate pvid files for existing devices.
	 * When -aay is set, any complete vg is activated
	 * (even if it's already active.)
	 */

	if (all_devs) {
		/*
		 * pvscan --cache removes existing hints and recreates new ones.
		 * We begin by clearing hints at the start of the command.
		 * The pvscan_recreate_hints flag is used to enable the
		 * special case hint recreation in label_scan.
		 */
		cmd->pvscan_recreate_hints = 1;
		pvscan_recreate_hints_begin(cmd);

		_lock_online(LOCK_EX, 0);
		log_verbose("pvscan all devices for requested refresh.");
		_online_files_remove(_pvs_online_dir);
		_online_files_remove(_vgs_online_dir);
		_online_pvscan_all_devs(cmd, complete_vgnames, NULL);
		_unlock_online();

		cmd->pvscan_recreate_hints = 0;
		cmd->use_hints = 0;
		goto activate;
	}

	/*
	 * Initialization case:
	 * lock_online ex
	 * if empty
	 * pvscan all
	 * create pvid files
	 * identify complete vgs
	 * unlock_online
	 * activate complete vgs
	 *
	 * Non-initialization case:
	 * lock_online ex
	 * if not empty
	 * unlock_unlock
	 * pvscan devs
	 * create pvid files
	 * identify complete vgs
	 * activate complete vgs
	 *
	 * In the non-init case, a VG with two PVs, where both PVs appear at once,
	 * two parallel pvscans for each PV create the pvid files for each PV in
	 * parallel, then both pvscans see the vg has completed, and both pvscans
	 * activate the VG in parallel.  The first pvscan to create the vgname
	 * file in vgs_online will do the activation, any others will skip it.
	 */

	_lock_online(LOCK_EX, 0);

	if (_online_pvid_files_missing()) {
		log_verbose("pvscan all devices to initialize available PVs.");
		_online_pvscan_all_devs(cmd, complete_vgnames, &add_devs);
		_unlock_online();
		goto activate;
	}

	_unlock_online();
	log_verbose("pvscan only specific devices add %d rem %d.",
		    dm_list_size(&add_devs), dm_list_size(&rem_devs));

	/*
	 * Unlink online files for devices that no longer have a device node.
	 * When unlinking a pvid file for dev, we don't need to scan the dev
	 * (we can't since it's gone), but we know which pvid file it is
	 * because the major:minor are saved in the pvid files which we can
	 * read to find the correct one.
	 */
	dm_list_iterate_items(devl, &rem_devs)
		_online_pvid_file_remove_devno((int)MAJOR(devl->dev->dev), (int)MINOR(devl->dev->dev));

	/*
	 * Create online files for devices that exist and pass the filter.
	 * When creating a pvid file for a dev, we have to scan it first
	 * to know that it's ours and what its pvid is (and which vg it
	 * belongs to if we want to do autoactivation.)
	 */
	if (!dm_list_empty(&add_devs)) {
		label_scan_devs(cmd, cmd->filter, &add_devs);

		dm_list_iterate_items(devl, &add_devs) {
			dev = devl->dev;

			if (dev->flags & DEV_FILTER_OUT_SCAN)
				continue;

			add_single_count++;

			if (!_online_pvscan_one(cmd, dev, NULL, complete_vgnames, 0, &pvid_without_metadata))
				add_errors++;
		}
	}

	/*
	 * After scanning only specific devs to add a device, there is a
	 * special case that requires us to then scan all devs.  That is when
	 * the dev scanned has no VG metadata, and it's the final device to
	 * complete the VG.  In this case we want to autoactivate the VG, but
	 * the scanned device does not know what VG it's in or whether that VG
	 * is now complete.  In this case we need to scan all devs and pick out
	 * the complete VG holding this device so we can then autoactivate that
	 * VG.
	 */
	if (!dm_list_empty(&add_devs) && complete_vgnames && dm_list_empty(complete_vgnames) &&
	    pvid_without_metadata && do_activate) {
		log_verbose("pvscan all devices for PV without metadata: %s.", pvid_without_metadata);
		_online_pvscan_all_devs(cmd, complete_vgnames, &add_devs);
	}

	/*
	 * When a new PV appears, the system runs pvscan --cache dev.
	 * This also means that existing hints are invalid, and
	 * we can force hints to be refreshed here.  There may be
	 * cases where this detects a change that the other methods
	 * of detecting invalid hints doesn't catch.
	 */
	if (add_single_count)
		invalidate_hints(cmd);

activate:

	/*
	 * Step 2: when the PV was recorded online, we check if all the
	 * PVs for the VG are online.  If so, the vgname was added to the
	 * list, and we can attempt to autoactivate LVs in the VG.
	 */
	if (do_activate)
		ret = _pvscan_aa(cmd, &pp, complete_vgnames);

	if (add_errors || pp.activate_errors)
		ret = ECMD_FAILED;

	if (!sync_local_dev_names(cmd))
		stack;
	unlock_vg(cmd, NULL, VG_GLOBAL);
	return ret;
}

int pvscan(struct cmd_context *cmd, int argc, char **argv)
{
	log_error(INTERNAL_ERROR "Missing function for command definition %d:%s.",
		  cmd->command->command_index, cmd->command->command_id);
	return ECMD_FAILED;
}

