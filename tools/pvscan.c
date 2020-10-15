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

/*
 * Used by _pvscan_aa_quick() which is an optimization used
 * when one vg is being activated.
 */
static struct volume_group *saved_vg;

static const char *_pvs_online_dir = DEFAULT_RUN_DIR "/pvs_online";
static const char *_vgs_online_dir = DEFAULT_RUN_DIR "/vgs_online";
static const char *_pvs_lookup_dir = DEFAULT_RUN_DIR "/pvs_lookup";

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

static void _lookup_file_remove(char *vgname)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s", _pvs_lookup_dir, vgname) < 0) {
		log_error("Path %s/%s is too long.", _pvs_lookup_dir, vgname);
		return;
	}

	log_debug("Unlink pvs_lookup: %s", path);

	if (unlink(path))
		log_sys_debug("unlink", path);
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

			if (file_vgname[0]) {
				_online_vg_file_remove(file_vgname);
				_lookup_file_remove(file_vgname);
			}
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
		log_error("Cannot create online file path for %s %d:%d.", dev_name(dev), major, minor);
		return 0;
	}

	if (vgname) {
		if ((len2 = dm_snprintf(buf + len1, sizeof(buf) - len1, "vg:%s\n", vgname)) < 0) {
			log_warn("Incomplete online file for %s %d:%d vg %s.", dev_name(dev), major, minor, vgname);
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
		log_error("Failed to create online file for %s path %s error %d", dev_name(dev), path, errno);
		return 0;
	}

	while (len > 0) {
		rv = write(fd, buf, len);
		if (rv < 0) {
			/* file exists so it still works in part */
			log_warn("Cannot write online file for %s to %s error %d",
				  dev_name(dev), path, errno);
			if (close(fd))
				log_sys_debug("close", path);
			return 1;
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
		log_debug("Check pv online %s: yes", pvid);
		return 1;
	}
	log_debug("Check pv online %s: no", pvid);
	return 0;
}

static int _write_lookup_file(struct cmd_context *cmd, struct volume_group *vg)
{
	char path[PATH_MAX];
	char line[ID_LEN+2];
	struct pv_list *pvl;
	int fd;

	if (dm_snprintf(path, sizeof(path), "%s/%s", _pvs_lookup_dir, vg->name) < 0) {
		log_error("Path %s/%s is too long.", _pvs_lookup_dir, vg->name);
		return 0;
	}

	fd = open(path, O_CREAT | O_EXCL | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		/* not a problem, can happen when multiple pvscans run at once */
		log_debug("Did not create %s: %d", path, errno);
		return 0;
	}

	log_debug("write_lookup_file %s", path);

	dm_list_iterate_items(pvl, &vg->pvs) {
		memcpy(&line, &pvl->pv->id.uuid, ID_LEN);
		line[ID_LEN] = '\n';
		line[ID_LEN+1] = '\0';

		if (write(fd, &line, ID_LEN+1) < 0)
			log_sys_debug("write", path);
	}

	if (close(fd))
		log_sys_debug("close", path);

	return 1;
}

static int _lookup_file_contains_pvid(FILE *fp, char *pvid)
{
	char line[64];

	while (fgets(line, sizeof(line), fp)) {
		if (!memcmp(pvid, line, ID_LEN))
			return 1;
	}
	return 0;
}

static void _lookup_file_count_pvid_files(FILE *fp, const char *vgname, int *pvs_online, int *pvs_offline)
{
	char line[64];
	char pvid[ID_LEN+1];

	log_debug("checking all pvid files using lookup file for %s", vgname);

	rewind(fp);

	while (fgets(line, sizeof(line), fp)) {
		memset(pvid, 0, sizeof(pvid));
		memcpy(pvid, line, ID_LEN);

		if (strlen(pvid) != ID_LEN) {
			log_debug("ignore lookup file line %s", line);
			continue;
		}

		if (_online_pvid_file_exists((const char *)pvid))
			(*pvs_online)++;
		else
			(*pvs_offline)++;
	}
}

/*
 * There is no synchronization between the one doing write_lookup_file and the
 * other doing check_lookup_file.  The pvscan doing write thinks the VG is
 * incomplete, and the pvscan doing check may also conclude the VG is
 * incomplete if it happens prior to the write.  If neither pvscan thinks the
 * VG is complete then neither will activate it.  To solve this race, the
 * pvscan doing write will recheck pvid online files after the write in which
 * case it would find the pvid online file from the pvscan doing check.
 */

/*
 * The VG is not yet complete, more PVs need to arrive, and
 * some of those PVs may not have metadata on them.  Without
 * metadata, pvscan for those PVs will be unable to determine
 * if the VG is complete.  We don't know if other PVs will have
 * metadata or not.
 *
 * Save a temp file with a list of pvids in the vg, to be used
 * by a later pvscan on a PV without metadata.  The later
 * pvscan will check for vg completeness using the temp file
 * since it has no vg metadata to use.
 *
 * Only the first pvscan for the VG creates the temp file.  If
 * there are multiple pvscans for the same VG running at once,
 * they all race to create the lookup file, and only the first
 * to create the file will write it.
 *
 * After writing the temp file, we count pvid online files for
 * the VG again - the VG could now be complete since multiple
 * pvscans will run concurrently.  We repeat this to cover a
 * race where another pvscan was running _check_lookup_file
 * during our _write_lookup_file.  _write_lookup_file may not
 * have finished before _check_lookup_file, which would cause
 * the other pvscan to not find the pvid it's looking for, and
 * conclude the VG is incomplete, while we also think the VG is
 * incomplete.  If we recheck online files after write_lookup,
 * we will see the pvid online file from the other pvscan and
 * see the VG is complete.
 */

static int _count_pvid_files_from_lookup_file(struct cmd_context *cmd, struct device *dev,
					       int *pvs_online, int *pvs_offline,
					       const char **vgname_out)
{
	char path[PATH_MAX];
	FILE *fp;
	DIR *dir;
	struct dirent *de;
	const char *vgname = NULL;
	int online = 0, offline = 0;

	*pvs_online = 0;
	*pvs_offline = 0;

	if (!(dir = opendir(_pvs_lookup_dir)))
		goto_bad;

	/*
	 * Read each file in pvs_lookup to find dev->pvid, and if it's
	 * found save the vgname of the file it's found in.
	 */
	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", _pvs_lookup_dir, de->d_name);

		if (!(fp = fopen(path, "r"))) {
			log_warn("Failed to open %s", path);
			continue;
		}

		if (_lookup_file_contains_pvid(fp, dev->pvid)) {
			vgname = dm_pool_strdup(cmd->mem, de->d_name);
			break;
		}

		if (fclose(fp))
			stack;
	}
	if (closedir(dir))
		log_sys_debug("closedir", _pvs_lookup_dir);

	if (!vgname)
		goto_bad;

	/*
	 * stat pvid online file of each pvid listed in this file
	 * the list of pvids from the file is the alternative to
	 * using vg->pvs
	 */
	_lookup_file_count_pvid_files(fp, vgname, &online, &offline);

	if (fclose(fp))
		stack;

	*pvs_online = online;
	*pvs_offline = offline;
	*vgname_out = vgname;
	return 1;
bad:
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
		goto do_lookup;

	log_debug("Creating vgs_online_dir.");
	dm_prepare_selinux_context(_vgs_online_dir, S_IFDIR);
	rv = mkdir(_vgs_online_dir, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(_vgs_online_dir, &st))
		log_error("Failed to create %s %d", _vgs_online_dir, errno);

do_lookup:
	if (!stat(_pvs_lookup_dir, &st))
		return;

	log_debug("Creating pvs_lookup_dir.");
	dm_prepare_selinux_context(_pvs_lookup_dir, S_IFDIR);
	rv = mkdir(_pvs_lookup_dir, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(_pvs_lookup_dir, &st))
		log_error("Failed to create %s %d", _pvs_lookup_dir, errno);


}

static void _count_pvid_files(struct volume_group *vg, int *pvs_online, int *pvs_offline)
{
	struct pv_list *pvl;

	*pvs_online = 0;
	*pvs_offline = 0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		if (_online_pvid_file_exists((const char *)&pvl->pv->id.uuid))
			(*pvs_online)++;
		else
			(*pvs_offline)++;
	}
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
	if (!(vg = saved_vg))
		goto_bad;

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
			goto bad;
		}

		devno = MKDEV(file_major, file_minor);

		if (!(dev = dev_cache_get_by_devt(cmd, devno, NULL, NULL))) {
			log_error("No device found for %d:%d PVID %s", file_major, file_minor, pvid);
			goto bad;
		}

		name1 = dev_name(dev);
		name2 = pvl->pv->device_hint;

		if (strcmp(name1, name2)) {
			if (!id_write_format((const struct id *)pvid, uuidstr, sizeof(uuidstr)))
				uuidstr[0] = '\0';
			log_print("PVID %s read from %s last written to %s.", uuidstr, name1, name2);
			goto bad;
		}

		if (!(devl = zalloc(sizeof(*devl))))
			goto_bad;

		devl->dev = dev;
		dm_list_add(devs, &devl->list);
		log_debug("pvscan using %s for PVID %s in VG %s", dev_name(dev), pvid, vgname);
	}

	return 1;

bad:
	if (saved_vg) {
		release_vg(saved_vg);
		saved_vg = NULL;
	}
	return 0;
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
			    int *no_quick)
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
	if (!_get_devs_from_saved_vg(cmd, vgname, &devs)) {
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
		      int do_all, struct dm_list *vgnames)
{
	struct processing_handle *handle = NULL;
	struct dm_str_list *sl, *sl2;
	int no_quick = 0;
	int ret;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error("Failed to initialize processing handle.");
		ret = ECMD_FAILED;
		goto out;
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
		ret = ECMD_PROCESSED;
		goto out;
	}

	/*
	 * When saved_vg is set there should only be one vgname.
	 * If the optimized "quick" function finds something amiss
	 * it will set no_quick and return so that the slow version
	 * can be used.
	 */
	if (!do_all && saved_vg && (dm_list_size(vgnames) == 1)) {
		log_debug("autoactivate quick");
		ret = _pvscan_aa_quick(cmd, pp, saved_vg->name, &no_quick);
	}

	/*
	 * do_all indicates 'pvscan --cache' in which case
	 * pvscan_cache_all() has already done lvmcache_label_scan
	 * which does not need to be repeated by process_each_vg.
	 */
	if (!saved_vg || (dm_list_size(vgnames) > 1) || no_quick) {
		uint32_t read_flags = READ_FOR_ACTIVATE;
		if (do_all)
			read_flags |= PROCESS_SKIP_SCAN;
		log_debug("autoactivate slow");
		ret = process_each_vg(cmd, 0, NULL, NULL, vgnames, read_flags, 0, handle, _pvscan_aa_single);
	}

	destroy_processing_handle(cmd, handle);
out:
	if (saved_vg) {
		release_vg(saved_vg);
		saved_vg = NULL;
	}
	return ret;
}

struct pvscan_arg {
	struct dm_list list;
	const char *devname;
	dev_t devno;
	struct device *dev;
};

static int _get_args(struct cmd_context *cmd, int argc, char **argv,
		     struct dm_list *pvscan_args)
{
	struct arg_value_group_list *current_group;
	struct pvscan_arg *arg;
	const char *arg_name;
	int major, minor;

	/* Process position args, which can be /dev/name or major:minor */

	while (argc) {
		argc--;
		arg_name = *argv++;

		if (arg_name[0] == '/') {
			if (!(arg = dm_pool_zalloc(cmd->mem, sizeof(*arg))))
				return_0;
			arg->devname = arg_name;
			dm_list_add(pvscan_args, &arg->list);
			continue;
		}

		if (sscanf(arg_name, "%d:%d", &major, &minor) != 2) {
			log_warn("WARNING: Failed to parse major:minor from %s, skipping.", arg_name);
			continue;
		}

		if (!(arg = dm_pool_zalloc(cmd->mem, sizeof(*arg))))
			return_0;
		arg->devno = MKDEV(major, minor);
		dm_list_add(pvscan_args, &arg->list);
	}

	/* Process any grouped --major --minor args */

	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		major = grouped_arg_int_value(current_group->arg_values, major_ARG, major);
		minor = grouped_arg_int_value(current_group->arg_values, minor_ARG, minor);

		if (major < 0 || minor < 0)
			continue;

		if (!(arg = dm_pool_zalloc(cmd->mem, sizeof(*arg))))
			return_0;
		arg->devno = MKDEV(major, minor);
		dm_list_add(pvscan_args, &arg->list);
	}

	return 1;
}

static int _get_args_devs(struct cmd_context *cmd, struct dm_list *pvscan_args,
			  struct dm_list *pvscan_devs)
{
	struct pvscan_arg *arg;
	struct device_list *devl;

	/* pass NULL filter when getting devs from dev-cache, filtering is done separately */

	/* in common usage, no dev will be found for a devno */

	dm_list_iterate_items(arg, pvscan_args) {
		if (arg->devname)
			arg->dev = dev_cache_get(cmd, arg->devname, NULL);
		else if (arg->devno)
			arg->dev = dev_cache_get_by_devt(cmd, arg->devno, NULL, NULL);
		else
			return_0;
	}

	dm_list_iterate_items(arg, pvscan_args) {
		if (!arg->dev)
			continue;

		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			return_0;
		devl->dev = arg->dev;
		dm_list_add(pvscan_devs, &devl->list);
	}

	return 1;
}

static int _online_devs(struct cmd_context *cmd, int do_all, struct dm_list *pvscan_devs,
			int *pv_count, struct dm_list *complete_vgnames)
{
	struct device_list *devl, *devl2;
	struct device *dev;
	struct lvmcache_info *info;
	const struct format_type *fmt;
	struct format_instance_ctx fic = { .type = 0 };
	struct format_instance *fid;
	struct metadata_area *mda1, *mda2;
	struct volume_group *vg;
	const char *vgname;
	uint32_t ext_version, ext_flags;
	int do_activate = arg_is_set(cmd, activate_ARG);
	int pvs_online;
	int pvs_offline;
	int pvs_unknown;
	int vg_complete;
	int ret = 1;

	dm_list_iterate_items_safe(devl, devl2, pvscan_devs) {
		dev = devl->dev;

		log_debug("online_devs %s %s", dev_name(dev), dev->pvid);

		/*
		 * This should already have been done by the filter, but make
		 * another check directly with udev in case the filter was not
		 * using udev and the native version didn't catch it.
		 */
		if (udev_dev_is_mpath_component(dev)) {
			log_print("pvscan[%d] ignore multipath component %s.", getpid(), dev_name(dev));
			continue;
		}

		if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
			if (!do_all)
				log_print("pvscan[%d] ignore %s with no lvm info.", getpid(), dev_name(dev));
			continue;
		}

		ext_version = lvmcache_ext_version(info);
		ext_flags = lvmcache_ext_flags(info);
		if ((ext_version >= 2) && !(ext_flags & PV_EXT_USED)) {
			log_print("pvscan[%d] PV %s not used.", getpid(), dev_name(dev));
			(*pv_count)++;
			continue;
		}

		fmt = lvmcache_fmt(info);
		fid = fmt->ops->create_instance(fmt, &fic);
		vg = NULL;

		mda1 = lvmcache_get_dev_mda(dev, 1);
		mda2 = lvmcache_get_dev_mda(dev, 2);

		if (mda1 && !mda_is_ignored(mda1))
			vg = mda1->ops->vg_read(cmd, fid, "", mda1, NULL, NULL);

		if (!vg && mda2 && !mda_is_ignored(mda2))
			vg = mda2->ops->vg_read(cmd, fid, "", mda2, NULL, NULL);

		if (!vg) {
			log_print("pvscan[%d] PV %s has no VG metadata.", getpid(), dev_name(dev));
			fmt->ops->destroy_instance(fid);
			goto online;
		}

		set_pv_devices(fid, vg, NULL);

		/*
		 * Skip devs that are md components (set_pv_devices can do new
		 * md check), are shared, or foreign.
		 */

        	if (dev->flags & DEV_IS_MD_COMPONENT) {
			log_print("pvscan[%d] PV %s ignore MD component, ignore metadata.", getpid(), dev_name(dev));
			release_vg(vg);
			continue;
		}

		if (vg_is_shared(vg)) {
			log_print("pvscan[%d] PV %s ignore shared VG.", getpid(), dev_name(dev));
			release_vg(vg);
			continue;
		}

		if (vg->system_id && vg->system_id[0] &&
		    cmd->system_id && cmd->system_id[0] &&
		    vg_is_foreign(vg)) {
			log_verbose("Ignore PV %s with VG system id %s with our system id %s",
				    dev_name(dev), vg->system_id, cmd->system_id);
			log_print("pvscan[%d] PV %s ignore foreign VG.", getpid(), dev_name(dev));
			release_vg(vg);
			continue;
		}

		/*
		 * online file phase
		 * create pvs_online/<pvid>
		 * check if vg is complete: stat pvs_online/<pvid> for each vg->pvs
		 * if vg is complete, save vg name in list for activation phase
		 * if vg not complete, create pvs_lookup/<vgname> listing all pvids from vg->pvs
		 * (only if pvs_lookup/<vgname> does not yet exist)
		 * if no vg metadata, read pvs_lookup files for pvid, use that list to check if complete
		 */
 online:
		(*pv_count)++;

		/*
		 * Create file named for pvid to record this PV is online.
		 */
		if (!_online_pvid_file_create(dev, vg ? vg->name : NULL)) {
			log_error("pvscan[%d] PV %s failed to create online file.", getpid(), dev_name(dev));
			release_vg(vg);
			ret = 0;
			continue;
		}

		/*
		 * When not activating we don't need to know about vg completeness.
		 */
		if (!do_activate) {
			log_print("pvscan[%d] PV %s online.", getpid(), dev_name(dev));
			release_vg(vg);
			continue;
		}

		/*
		 * Check if all the PVs for this VG are online.  If the arrival
		 * of this dev completes the VG, then save the vgname in
		 * complete_vgnames so it will be activated.
		 */
		pvs_online = 0;
		pvs_offline = 0;
		pvs_unknown = 0;
		vg_complete = 0;

		if (vg) {
			/*
			 * Use the VG metadata from this PV for a list of all
			 * PVIDs.  Write a lookup file of PVIDs in case another
			 * pvscan needs it.  After writing lookup file, recheck
			 * pvid files to resolve a possible race with another
			 * pvscan reading the lookup file that missed it.
			 */
			log_debug("checking all pvid files from vg %s", vg->name);
			_count_pvid_files(vg, &pvs_online, &pvs_offline);

			if (pvs_offline && _write_lookup_file(cmd, vg)) {
				log_debug("rechecking all pvid files from vg %s", vg->name);
				_count_pvid_files(vg, &pvs_online, &pvs_offline);
				if (!pvs_offline)
					log_print("pvscan[%d] VG %s complete after recheck.", getpid(), vg->name);
			}

			vgname = vg->name;
		} else {
			/*
			 * No VG metadata on this PV, so try to use a lookup
			 * file written by a prior pvscan for a list of all
			 * PVIDs.  A lookup file may not exist for this PV if
			 * it's the first to appear from the VG.
			 */
			log_debug("checking all pvid files from lookup file");
			if (!_count_pvid_files_from_lookup_file(cmd, dev, &pvs_online, &pvs_offline, &vgname))
				pvs_unknown = 1;
		}

		if (pvs_unknown) {
			log_print("pvscan[%d] PV %s online, VG unknown.", getpid(), dev_name(dev));
			vg_complete = 0;

		} else if (pvs_offline) {
			log_print("pvscan[%d] PV %s online, VG %s incomplete (need %d).",
				  getpid(), dev_name(dev), vgname, pvs_offline);
			vg_complete = 0;

		} else {
			log_print("pvscan[%d] PV %s online, VG %s is complete.", getpid(), dev_name(dev), vgname);
			if (!str_list_add(cmd->mem, complete_vgnames, dm_pool_strdup(cmd->mem, vgname)))
				stack;
			vg_complete = 1;
		}

		if (!saved_vg && vg && vg_complete && !do_all && (dm_list_size(pvscan_devs) == 1))
			saved_vg = vg;
		else
			release_vg(vg);
	}

	return ret;
}

static int _pvscan_cache_all(struct cmd_context *cmd, int argc, char **argv,
			     struct dm_list *complete_vgnames)
{
	struct dm_list pvscan_devs;
	struct dev_iter *iter;
	struct device_list *devl;
	struct device *dev;
	int pv_count = 0;

	dm_list_init(&pvscan_devs);

	_online_files_remove(_pvs_online_dir);
	_online_files_remove(_vgs_online_dir);
	_online_files_remove(_pvs_lookup_dir);

	/*
	 * pvscan --cache removes existing hints and recreates new ones.
	 * We begin by clearing hints at the start of the command.
	 * The pvscan_recreate_hints flag is used to enable the
	 * special case hint recreation in label_scan.
	 */
	cmd->pvscan_recreate_hints = 1;
	pvscan_recreate_hints_begin(cmd);

	log_debug("pvscan_cache_all: label scan all");

	/*
	 * use lvmcache_label_scan() instead of just label_scan_devs()
	 * because label_scan() has the ability to update hints,
	 * which we want 'pvscan --cache' to do, and that uses
	 * info from lvmcache, e.g. duplicate pv info.
	 */
	lvmcache_label_scan(cmd);

	cmd->pvscan_recreate_hints = 0;
	cmd->use_hints = 0;

	log_debug("pvscan_cache_all: create list of devices");

	/*
	 * The use of filter here will just reuse the existing
	 * (persistent) filter info label_scan has already set up.
	 */
	if (!(iter = dev_iter_create(cmd->filter, 1)))
		return_0;

	while ((dev = dev_iter_get(cmd, iter))) {
		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl)))) {
			dev_iter_destroy(iter);
			return_0;
		}
		devl->dev = dev;
		dm_list_add(&pvscan_devs, &devl->list);
	}
	dev_iter_destroy(iter);

	_online_devs(cmd, 1, &pvscan_devs, &pv_count, complete_vgnames);

	return 1;
}

static int _pvscan_cache_args(struct cmd_context *cmd, int argc, char **argv,
			      struct dm_list *complete_vgnames)
{
	struct dm_list pvscan_args; /* struct pvscan_arg */
	struct dm_list pvscan_devs; /* struct device_list */
	struct pvscan_arg *arg;
	struct device_list *devl, *devl2;
	int pv_count = 0;
	int ret;

	dm_list_init(&pvscan_args);
	dm_list_init(&pvscan_devs);

	cmd->pvscan_cache_single = 1;

	dev_cache_scan();

	/*
	 * Get list of args.  Do not use filters.
	 */
	if (!_get_args(cmd, argc, argv, &pvscan_args))
		return_0;

	/*
	 * Get list of devs for args.  Do not use filters.
	 */
	if (!_get_args_devs(cmd, &pvscan_args, &pvscan_devs))
		return_0;

	/*
	 * Remove pvid online files for major/minor args for which the dev has
	 * been removed.
	 */
	dm_list_iterate_items(arg, &pvscan_args) {
		if (arg->dev || !arg->devno)
			continue;
		_online_pvid_file_remove_devno((int)MAJOR(arg->devno), (int)MINOR(arg->devno));
	}

	/*
	 * A common pvscan removal of a single dev is done here.
	 */
	if (dm_list_empty(&pvscan_devs))
		return 1;

	if (cmd->md_component_detection && !cmd->use_full_md_check &&
	    !strcmp(cmd->md_component_checks, "auto") &&
	    dev_cache_has_md_with_end_superblock(cmd->dev_types)) {
		log_debug("Enable full md component check.");
		cmd->use_full_md_check = 1;
	}

	/*
	 * Apply nodata filters.
	 * bcache is not yet set up so no filters will do io.
	 */

	log_debug("pvscan_cache_args: filter devs nodata");

	cmd->filter_nodata_only = 1;

	dm_list_iterate_items_safe(devl, devl2, &pvscan_devs) {
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL)) {
			log_print("pvscan[%d] %s excluded by filters: %s.", getpid(),
				  dev_name(devl->dev), dev_filtered_reason(devl->dev));
			dm_list_del(&devl->list);
		}
	}

	cmd->filter_nodata_only = 0;

	/*
	 * Clear the results of nodata filters that were saved by the
	 * persistent filter so that the complete set of filters will
	 * be checked by passes_filter below.
	 */
	dm_list_iterate_items(devl, &pvscan_devs)
		cmd->filter->wipe(cmd, cmd->filter, devl->dev, NULL);

	/*
	 * Read header from each dev.
	 * Eliminate non-lvm devs.
	 * Apply all filters.
	 */

	log_debug("pvscan_cache_args: read and filter devs");

	label_scan_setup_bcache();

	dm_list_iterate_items_safe(devl, devl2, &pvscan_devs) {
		if (!label_read_pvid(devl->dev)) {
			/* Not an lvm device */
			log_print("pvscan[%d] %s not an lvm device.", getpid(), dev_name(devl->dev));
			dm_list_del(&devl->list);
			continue;
		}

		/* Applies all filters, including those that need data from dev. */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL)) {
			log_print("pvscan[%d] %s excluded by filters: %s.", getpid(),
				  dev_name(devl->dev), dev_filtered_reason(devl->dev));
			dm_list_del(&devl->list);
		}
	}

	if (dm_list_empty(&pvscan_devs))
		return 1;

	log_debug("pvscan_cache_args: label scan devs");

	/*
	 * Scan devs to populate lvmcache info, which includes the mda info that's
	 * needed to read vg metadata in the next step.  The _cached variant of
	 * label_scan is used so the exsting bcache data from label_read_pvid above
	 * can be reused (although more data may need to be read depending on how
	 * much of the metadata was covered by reading the pvid.)
	 */
	label_scan_devs_cached(cmd, NULL, &pvscan_devs);

	ret = _online_devs(cmd, 0, &pvscan_devs, &pv_count, complete_vgnames);

	/*
	 * When a new PV appears, the system runs pvscan --cache dev.
	 * This also means that existing hints are invalid, and
	 * we can force hints to be refreshed here.  There may be
	 * cases where this detects a change that the other methods
	 * of detecting invalid hints doesn't catch.
	 */
	if (pv_count)
		invalidate_hints(cmd);

	return ret;
}

int pvscan_cache_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_aa_params pp = { 0 };
	struct dm_list complete_vgnames;
	int do_activate = arg_is_set(cmd, activate_ARG);
	int devno_args = 0;
	int do_all;
	int ret;

	dm_list_init(&complete_vgnames);

	if (arg_is_set(cmd, major_ARG) + arg_is_set(cmd, minor_ARG))
		devno_args = 1;

	if (devno_args && (!arg_is_set(cmd, major_ARG) || !arg_is_set(cmd, minor_ARG))) {
		log_error("Both --major and --minor required to identify devices.");
		return EINVALID_CMD_LINE;
	}

	do_all = !argc && !devno_args;

	_online_dir_setup();

	if (do_all) {
		if (!_pvscan_cache_all(cmd, argc, argv, &complete_vgnames))
			return ECMD_FAILED;
	} else {
		if (!_pvscan_cache_args(cmd, argc, argv, &complete_vgnames))
			return ECMD_FAILED;
	}

	if (!do_activate)
		return ECMD_PROCESSED;

	if (dm_list_empty(&complete_vgnames)) {
		log_debug("No VGs to autoactivate.");
		return ECMD_PROCESSED;
	}

	/*
	 * When the PV was recorded online, we check if all the PVs for the VG
	 * are online.  If so, the vgname was added to the list, and we can
	 * attempt to autoactivate LVs in the VG.
	 */
	ret = _pvscan_aa(cmd, &pp, do_all, &complete_vgnames);

	if (pp.activate_errors)
		ret = ECMD_FAILED;

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

