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

static int _online_pvid_file_read(char *path, int *major, int *minor, char *vgname)
{
	char buf[MAX_PVID_FILE_SIZE];
	char *name;
	int fd, rv;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		log_warn("Failed to open %s", path);
		return 0;
	}

	memset(buf, 0, sizeof(buf));

	rv = read(fd, buf, sizeof(buf));
	if (close(fd))
		log_sys_debug("close", path);
	if (!rv || rv < 0) {
		log_warn("No info in %s", path);
		return 0;
	}

	if (sscanf(buf, "%d:%d", major, minor) != 2) {
		log_warn("No device numbers in %s", path);
		return 0;
	}

	/* vgname points to an offset in buf */
	if ((name = _vgname_in_pvid_file_buf(buf)))
		strncpy(vgname, name, NAME_LEN);
	else
		log_debug("No vgname in %s", path);

	return 1;
}


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
	char file_vgname[NAME_LEN];
	DIR *dir;
	struct dirent *de;
	int file_major = 0, file_minor = 0;

	log_debug("Remove pv online devno %d:%d", major, minor);

	if (!(dir = opendir(_pvs_online_dir)))
		return;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, de->d_name);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));

		_online_pvid_file_read(path, &file_major, &file_minor, file_vgname);

		if ((file_major == major) && (file_minor == minor)) {
			log_debug("Unlink pv online %s", path);
			if (unlink(path))
				log_sys_debug("unlink", path);

			if (file_vgname[0])
				_online_vg_file_remove(file_vgname);
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
	char file_vgname[NAME_LEN];
	int file_major = 0, file_minor = 0;
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

	fd = open(path, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		if (errno == EEXIST)
			goto check_duplicate;
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

check_duplicate:

	/*
	 * If a PVID online file already exists for this PVID, check if the
	 * file contains a different device number, and if so we may have a
	 * duplicate PV.
	 *
	 * FIXME: disable autoactivation of the VG somehow?
	 * The VG may or may not already be activated when a dupicate appears.
	 * Perhaps write a new field in the pv online or vg online file?
	 */

	memset(file_vgname, 0, sizeof(file_vgname));

	_online_pvid_file_read(path, &file_major, &file_minor, file_vgname);

	if ((file_major == major) && (file_minor == minor)) {
		log_debug("Existing online file for %d:%d", major, minor);
		return 1;
	}

	/* Don't know how vgname might not match, but it's not good so fail. */

	if ((file_major != major) || (file_minor != minor))
		log_error("pvscan[%d] PV %s is duplicate for PVID %s on %d:%d and %d:%d.",
			  getpid(), dev_name(dev), dev->pvid, major, minor, file_major, file_minor);

	if (file_vgname[0] && vgname && strcmp(file_vgname, vgname))
		log_error("pvscan[%d] PV %s has unexpected VG %s vs %s.",
			  getpid(), dev_name(dev), vgname, file_vgname);

	return 0;
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

/*
 * If no mdas on this PV have a usable copy of the metadata,
 * then the PV behaves like a PV without metadata, which
 * causes the pvscan to scan all devs to find a copy of the
 * metadata on another dev to check if the VG is now complete
 * and can be activated.
 */

static int _online_pvscan_single(struct metadata_area *mda, void *baton)
{
	struct _pvscan_baton *b = baton;
	struct volume_group *vg;
	struct device *mda_dev = mda_get_device(mda);

	if (mda_is_ignored(mda))
		return 1;
	vg = mda->ops->vg_read(b->fid, "", mda, NULL, NULL);
	if (!vg) {
		/*
		 * Many or most cases of bad metadata would be found in
		 * the scan, and the mda removed from the list, so we
		 * would not get here to attempt this.
		 */
		log_print("pvscan[%d] metadata error in mda %d on %s.",
			  getpid(), mda->mda_num, dev_name(mda_dev));
		return 1;
	}

	log_debug("pvscan vg_read %s seqno %u in mda %d on %s",
		  vg->name, vg->seqno, mda->mda_num, dev_name(mda_dev));

	if (!b->vg || vg->seqno > b->vg->seqno)
		b->vg = vg;
	else if (b->vg)
		release_vg(vg);

	return 1;
}

static struct volume_group *_find_saved_vg(struct dm_list *saved_vgs, const char *vgname)
{
	struct vg_list *vgl;

	dm_list_iterate_items(vgl, saved_vgs) {
		if (!strcmp(vgname, vgl->vg->name))
			return vgl->vg;
	}
	return NULL;
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
			      struct dm_list *saved_vgs,
			      int disable_remove,
			      const char **pvid_without_metadata)
{
	struct lvmcache_info *info;
	struct vg_list *vgl;
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

	memset(&baton, 0, sizeof(baton));
	baton.cmd = cmd;
	baton.vg = NULL;
	baton.fid = fmt->ops->create_instance(fmt, &fic);

	if (!baton.fid) {
		ret = 0;
		goto_out;
	}

	lvmcache_foreach_mda(info, _online_pvscan_single, &baton);

	if (!baton.vg) {
		log_print("pvscan[%d] PV %s has no VG metadata.", getpid(), dev_name(dev));
		if (pvid_without_metadata)
			*pvid_without_metadata = dm_pool_strdup(cmd->mem, dev->pvid);
		fmt->ops->destroy_instance(baton.fid);
	} else {
		set_pv_devices(baton.fid, baton.vg, NULL);
	}

	/* This check repeated because set_pv_devices can do new md check. */
	if (dev->flags & DEV_IS_MD_COMPONENT) {
		log_print("pvscan[%d] PV %s ignore MD component, ignore metadata.", getpid(), dev_name(dev));
		if (baton.vg)
			release_vg(baton.vg);
		else
			fmt->ops->destroy_instance(baton.fid);
		return 1;
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

	/*
	 * Save vg's in case they need to be used at the end for checking PVs
	 * without metadata (in _check_vg_with_pvid_complete), or activating.
	 */
	if (saved_vgs && baton.vg) {
		if (!_find_saved_vg(saved_vgs, baton.vg->name)) {
	       		if ((vgl = malloc(sizeof(struct vg_list)))) {
				vgl->vg = baton.vg;
				baton.vg = NULL;
				dm_list_add(saved_vgs, &vgl->list);
			}
		}
	}

	if (baton.vg)
		release_vg(baton.vg);
out:
	return ret;
}

/*
 * This is to handle the case where pvscan --cache -aay (with no dev args)
 * gets to the final PV, completing the VG, but that final PV does not
 * have VG metadata.  In this case, we need to use VG metadata from a
 * previously scanned PV in the same VG, which we saved in the saved_vgs
 * list.  Using this saved metadata, we can find which VG this PVID
 * belongs to, and then check if that VG is now complete, and if so
 * add the VG name to the list of complete VGs to be autoactivated.
 *
 * The "pvid" arg here is the PVID of the PV that has just been scanned
 * and didn't have metadata.  We look through previously scanned VG
 * metadata to find the VG this PVID belongs to, and then check that VG
 * metadata to see if all the PVs are now online.
 */
static void _check_vg_with_pvid_complete(struct cmd_context *cmd,
				         struct dm_list *found_vgnames,
					 struct dm_list *saved_vgs,
					 const char *pvid)
{
	struct vg_list *vgl;
	struct pv_list *pvl;
	struct volume_group *vg;
	int pvids_not_online = 0;
	int found;

	dm_list_iterate_items(vgl, saved_vgs) {
		vg = vgl->vg;
		found = 0;

		dm_list_iterate_items(pvl, &vg->pvs) {
			if (strcmp((const char *)&pvl->pv->id.uuid, pvid))
				continue;
			found = 1;
			break;
		}
		if (!found)
			continue;

		dm_list_iterate_items(pvl, &vg->pvs) {
			if (!_online_pvid_file_exists((const char *)&pvl->pv->id.uuid)) {
				pvids_not_online++;
				break;
			}
		}

		if (!pvids_not_online) {
			log_debug("pvid %s makes complete VG %s", pvid, vg->name);
			if (!str_list_add(cmd->mem, found_vgnames, dm_pool_strdup(cmd->mem, vg->name)))
				stack;
		} else
			log_debug("pvid %s incomplete VG %s", pvid, vg->name);
		break;
	}
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
 *
 * found_vgnames is null for 'pvscan --cache' (without -aay)
 * since the command does not need to keep track of complete
 * vgs since it does not need to activate them.
 */

static void _online_pvscan_all_devs(struct cmd_context *cmd,
				    struct dm_list *found_vgnames,
				    struct dm_list *saved_vgs,
				    struct dm_list *dev_args)
{
	struct dev_iter *iter;
	struct device *dev;
	const char *pvid_without_metadata;

	lvmcache_label_scan(cmd);

	if (!(iter = dev_iter_create(cmd->filter, 1))) {
		log_error("dev_iter creation failed");
		return;
	}

	while ((dev = dev_iter_get(cmd, iter))) {
		if (sigint_caught()) {
			stack;
			break;
		}

		pvid_without_metadata = NULL;

		if (!_online_pvscan_one(cmd, dev, dev_args, found_vgnames, saved_vgs, 1, &pvid_without_metadata)) {
			stack;
			break;
		}

		/* This PV without metadata may complete a VG. */
		if (pvid_without_metadata && found_vgnames)
			_check_vg_with_pvid_complete(cmd, found_vgnames, saved_vgs, pvid_without_metadata);
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

/*
 * This is a very unconventional way of doing things because
 * we want to figure out which devices to read the VG from
 * without first scanning devices.  It's usually the reverse;
 * we have to scan all devs, which tells us which devs we
 * need to read to get the VG.
 *
 * We can do it this way only by cheating and using the pvid
 * online files for devs that have been scanned by prior pvscan
 * instances.
 *
 * This is similar to the hints file, but the hints file is
 * always a full picture of PV state, and is only ever created
 * by scanning all devs, whereas the online files are only
 * created incrementally by scanning one device at a time.
 * The online files are only used for determining complete VGs
 * for the purpose of autoactivation, and no attempt is made
 * to keep them in sync with lvm state once autoactivation
 * is complete, but much effort is made to always ensure hints
 * will accurately reflect PV state.
 *
 * The saved VG metadata tells us which PVIDs are needed to
 * complete the VG.  The pv online files tell us which of those
 * PVIDs are online, and the content of those pv online files
 * tells us which major:minor number holds that PVID.  The
 * dev_cache tell us which struct device has the major:minor.
 * We end up with a list of struct devices that we need to
 * scan/read in order to process/activate the VG.
 */

static int _get_devs_from_saved_vg(struct cmd_context *cmd, const char *vgname,
				   struct dm_list *saved_vgs,
				   struct dm_list *devs)
{
	char path[PATH_MAX];
	char file_vgname[NAME_LEN];
	char uuidstr[64] __attribute__((aligned(8)));
	struct pv_list *pvl;
	struct device_list *devl;
	struct device *dev;
	struct volume_group *vg;
	const char *pvid;
	const char *name1, *name2;
	dev_t devno;
	int file_major = 0, file_minor = 0;

	/*
	 * We previously saved the metadata (as a struct vg) from the device
	 * arg that was scanned.  Now use that metadata to put together a list
	 * of devices for this VG.  (This could alternatively be worked out by
	 * reading all the pvid online files, see which have a matching vg
	 * name, and getting the device numbers from those files.)
	 */
	if (!(vg = _find_saved_vg(saved_vgs, vgname)))
		return_0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		pvid = (const char *)&pvl->pv->id.uuid;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", _pvs_online_dir, pvid);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));

		_online_pvid_file_read(path, &file_major, &file_minor, file_vgname);

		if (file_vgname[0] && strcmp(vgname, file_vgname)) {
			log_error("Wrong VG found for %d:%d PVID %s: %s vs %s",
				  file_major, file_minor, pvid, vgname, file_vgname);
			return 0;
		}

		devno = MKDEV(file_major, file_minor);

		if (!(dev = dev_cache_get_by_devt(cmd, devno, NULL, NULL))) {
			log_error("No device found for %d:%d PVID %s", file_major, file_minor, pvid);
			return 0;
		}

		name1 = dev_name(dev);
		name2 = pvl->pv->device_hint;

		if (strcmp(name1, name2)) {
			if (!id_write_format((const struct id *)pvid, uuidstr, sizeof(uuidstr)))
				uuidstr[0] = '\0';
			log_print("PVID %s read from %s last written to %s.", uuidstr, name1, name2);
			return 0;
		}

		if (!(devl = zalloc(sizeof(*devl))))
			return_0;

		devl->dev = dev;
		dm_list_add(devs, &devl->list);
		log_debug("pvscan using %s for PVID %s in VG %s", dev_name(dev), pvid, vgname);
	}

	return 1;
}

/*
 * When there's a single VG to activate (the common case),
 * optimize things by cutting out the process_each_vg().
 *
 * The main point of this optimization is to avoid extra
 * device scanning in the common case where we're
 * activating a completed VG after scanning a single PV.
 * The scanning overhead of hundreds of concurrent
 * activations from hundreds of PVs appearing together
 * can be overwhelming, and scanning needs to be reduced
 * as much as possible.
 *
 * The common process_each_vg will generally do:
 * label scan all devs
 * vg_read
 *   lock vg
 *   label rescan of only vg devs (often skipped)
 *   read metadata
 *   set pv devices (find devs for each PVID)
 * do command (vgchange_activate)
 * unlock vg
 *
 * In this optimized version with process_each we do:
 * lock vg
 * label scan of only vg devs
 * vg_read
 *   read metadata
 *   set pv devices (find devs for each PVID)
 * do command (vgchange_activate)
 * unlock vg
 *
 * The optimized version avoids scanning all devs, which
 * is important when there are many devs.
 */

static int _pvscan_aa_quick(struct cmd_context *cmd, struct pvscan_aa_params *pp, const char *vgname,
			    struct dm_list *saved_vgs, int *no_quick)
{
	struct dm_list devs; /* device_list */
	struct volume_group *vg;
	struct pv_list *pvl;
	const char *vgid;
	uint32_t lockd_state = 0;
	uint32_t error_flags = 0;
	int ret = ECMD_PROCESSED;

	dm_list_init(&devs);

	/*
	 * Get list of devices for this VG so we can label scan them.
	 * The saved VG struct gives the list of PVIDs in the VG.
	 * The pvs_online/PVID files gives us the devnums for PVIDs.
	 * The dev_cache gives us struct devices from the devnums.
	 */
	if (!_get_devs_from_saved_vg(cmd, vgname, saved_vgs, &devs)) {
		log_print("pvscan[%d] VG %s not using quick activation.", getpid(), vgname);
		*no_quick = 1;
		return ECMD_FAILED;
	}

	/*
	 * Lock the VG before scanning so we don't need to
	 * rescan in _vg_read.  (The lock_vol and the
	 * label rescan are then disabled in vg_read.)
	 */
	if (!lock_vol(cmd, vgname, LCK_VG_WRITE, NULL)) {
		log_error("pvscan activation for VG %s failed to lock VG.", vgname);
		return ECMD_FAILED;
	}

	/*
	 * Drop lvmcache info about the PV/VG that was saved
	 * when originally identifying the PV.
	 */
	lvmcache_destroy(cmd, 1, 1);

	label_scan_devs(cmd, NULL, &devs);

	if (!(vgid = lvmcache_vgid_from_vgname(cmd, vgname))) {
		log_error("pvscan activation for VG %s failed to find vgid.", vgname);
		return ECMD_FAILED;
	}

	/*
	 * can_use_one_scan and READ_WITHOUT_LOCK are both important key
	 * changes made to vg_read that are possible because the VG is locked
	 * above (lock_vol).
	 */

	cmd->can_use_one_scan = 1;

	vg = vg_read(cmd, vgname, vgid, READ_WITHOUT_LOCK | READ_FOR_ACTIVATE, lockd_state, &error_flags, NULL);

	if (!vg) {
		/*
		 * The common cases would already have been caught during the
		 * original device arg scan.  There will be very few and unusual
		 * cases that would be caught here.
		 */
		log_error("pvscan activation for VG %s cannot read (%x).", vgname, error_flags);
		return ECMD_FAILED;
	}

	/*
	 * These cases would already have been caught during the original
	 * device arg scan.
	 */
	if (vg_is_clustered(vg))
		goto_out;
	if (vg_is_exported(vg))
		goto_out;
	if (vg_is_shared(vg))
		goto_out;

	/*
	 * Verify that the devices we scanned above for the VG are in fact the
	 * devices used by the VG we read.
	 */
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (dev_in_device_list(pvl->pv->dev, &devs))
			continue;
		log_error("pvscan activation for VG %s found different devices.", vgname);
		ret = ECMD_FAILED;
		goto out;
	}

	log_debug("pvscan autoactivating VG %s.", vgname);

	if (!vgchange_activate(cmd, vg, CHANGE_AAY)) {
		log_error("%s: autoactivation failed.", vg->name);
		pp->activate_errors++;
	}

out:
	unlock_vg(cmd, vg, vgname);
	release_vg(vg);
	return ret;
}

static int _pvscan_aa(struct cmd_context *cmd, struct pvscan_aa_params *pp,
		      struct dm_list *vgnames, struct dm_list *saved_vgs)
{
	struct processing_handle *handle = NULL;
	struct dm_str_list *sl, *sl2;
	int no_quick = 0;
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

	if (dm_list_size(vgnames) == 1) {
		dm_list_iterate_items(sl, vgnames)
			ret = _pvscan_aa_quick(cmd, pp, sl->str, saved_vgs, &no_quick);
	}

	if ((dm_list_size(vgnames) > 1) || no_quick)
		ret = process_each_vg(cmd, 0, NULL, NULL, vgnames, READ_FOR_ACTIVATE, 0, handle, _pvscan_aa_single);

	destroy_processing_handle(cmd, handle);

	return ret;
}

int pvscan_cache_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_aa_params pp = { 0 };
	struct dm_list add_devs;
	struct dm_list rem_devs;
	struct dm_list vgnames;
	struct dm_list vglist;
	struct dm_list *complete_vgnames = NULL;
	struct dm_list *saved_vgs = NULL;
	struct device *dev;
	struct device_list *devl;
	struct vg_list *vgl;
	const char *pv_name;
	const char *pvid_without_metadata = NULL;
	int32_t major = -1;
	int32_t minor = -1;
	int devno_args = 0;
	int all_devs;
	struct arg_value_group_list *current_group;
	dev_t devno;
	int filtered;
	int do_activate = arg_is_set(cmd, activate_ARG);
	int add_errors = 0;
	int add_single_count = 0;
	int ret = ECMD_PROCESSED;

	dm_list_init(&add_devs);
	dm_list_init(&rem_devs);
	dm_list_init(&vgnames);
	dm_list_init(&vglist);

	/*
	 * When systemd/udev run pvscan --cache commands, those commands
	 * should not wait on udev info since the udev info may not be
	 * complete until the pvscan --cache command is done.
	 */
	init_udev_sleeping(0);

	if (do_activate) {
		complete_vgnames = &vgnames;
		saved_vgs = &vglist;
	}

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

	_online_dir_setup();

	/* Creates a list of dev names from /dev, sysfs, etc; does not read any. */
	dev_cache_scan();

	if (cmd->md_component_detection && !cmd->use_full_md_check &&
	    !strcmp(cmd->md_component_checks, "auto") &&
	    dev_cache_has_md_with_end_superblock(cmd->dev_types)) {
		log_debug("Enable full md component check.");
		cmd->use_full_md_check = 1;
	}

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
					log_print("pvscan[%d] device %s excluded by filter.", getpid(), dev_name(dev));
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

			if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->filter, &filtered))) {
				if (filtered) {
					if ((dev = dev_cache_get_by_devt(cmd, devno, NULL, NULL)))
						log_print("pvscan[%d] device %d:%d %s excluded by filter.", getpid(), major, minor, dev_name(dev));
					else
						log_print("pvscan[%d] device %d:%d excluded by filter.", getpid(), major, minor);
				} else
					log_print("pvscan[%d] device %d:%d not found.", getpid(), major, minor);

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

		if (!(dev = dev_cache_get_by_devt(cmd, devno, cmd->filter, &filtered))) {
			if (filtered) {
				if ((dev = dev_cache_get_by_devt(cmd, devno, NULL, NULL)))
					log_print("pvscan[%d] device %d:%d %s excluded by filter.", getpid(), major, minor, dev_name(dev));
				else
					log_print("pvscan[%d] device %d:%d excluded by filter.", getpid(), major, minor);
			} else
				log_print("pvscan[%d] device %d:%d not found.", getpid(), major, minor);

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

		log_verbose("pvscan all devices for requested refresh.");
		_online_files_remove(_pvs_online_dir);
		_online_files_remove(_vgs_online_dir);
		_online_pvscan_all_devs(cmd, complete_vgnames, saved_vgs, NULL);

		cmd->pvscan_recreate_hints = 0;
		cmd->use_hints = 0;
		goto activate;
	}

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

			if (dev->flags & DEV_FILTER_OUT_SCAN) {
				log_print("pvscan[%d] device %s excluded by filter.", getpid(), dev_name(dev));
				continue;
			}

			add_single_count++;

			if (!_online_pvscan_one(cmd, dev, NULL, complete_vgnames, saved_vgs, 0, &pvid_without_metadata))
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
		log_print("pvscan[%d] scan all devices for PV without metadata: %s.", getpid(), pvid_without_metadata);
		_online_pvscan_all_devs(cmd, complete_vgnames, saved_vgs, &add_devs);
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
		ret = _pvscan_aa(cmd, &pp, complete_vgnames, saved_vgs);

	if (add_errors || pp.activate_errors)
		ret = ECMD_FAILED;

	dm_list_iterate_items(vgl, &vglist)
		release_vg(vgl->vg);

	if (!sync_local_dev_names(cmd))
		stack;
	return ret;
}

int pvscan(struct cmd_context *cmd, int argc, char **argv)
{
	log_error(INTERNAL_ERROR "Missing function for command definition %d:%s.",
		  cmd->command->command_index, cmd->command->command_id);
	return ECMD_FAILED;
}

