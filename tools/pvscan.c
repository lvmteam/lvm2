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
#include "lib/device/online.h"
#include "lib/filters/filter.h"

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

static void _lookup_file_remove(char *vgname)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_LOOKUP_DIR, vgname) < 0) {
		log_error("Path %s/%s is too long.", PVS_LOOKUP_DIR, vgname);
		return;
	}

	log_debug("Unlink pvs_lookup: %s", path);

	if (unlink(path) && (errno != ENOENT))
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

	if (!(dir = opendir(PVS_ONLINE_DIR)))
		return;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, de->d_name);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));

		online_pvid_file_read(path, &file_major, &file_minor, file_vgname, NULL);

		if ((file_major == major) && (file_minor == minor)) {
			log_debug("Unlink pv online %s", path);
			if (unlink(path) && (errno != ENOENT))
				log_sys_debug("unlink", path);

			if (file_vgname[0]) {
				online_vg_file_remove(file_vgname);
				_lookup_file_remove(file_vgname);
			}
		}
	}
	if (closedir(dir))
		log_sys_debug("closedir", PVS_ONLINE_DIR);
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
		if (unlink(path) && (errno != ENOENT))
			log_sys_debug("unlink", path);
	}
	if (closedir(dir))
		log_sys_debug("closedir", dirpath);
}

static int _write_lookup_file(struct cmd_context *cmd, struct volume_group *vg)
{
	char path[PATH_MAX];
	char line[ID_LEN+2];
	struct pv_list *pvl;
	int fd;

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_LOOKUP_DIR, vg->name) < 0) {
		log_error_pvscan(cmd, "Path %s/%s is too long.", PVS_LOOKUP_DIR, vg->name);
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
			log_error_pvscan(cmd, "Failed to write lookup entry %s %s", path, line);
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
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };

	log_debug("checking all pvid files using lookup file for %s", vgname);

	rewind(fp);

	while (fgets(line, sizeof(line), fp)) {
		memcpy(pvid, line, ID_LEN);

		if (strlen(pvid) != ID_LEN) {
			log_debug("ignore lookup file line %s", line);
			continue;
		}

		if (online_pvid_file_exists((const char *)pvid))
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
	char path[PATH_MAX] = { 0 };
	FILE *fp;
	DIR *dir;
	struct dirent *de;
	const char *vgname = NULL;

	*vgname_out = NULL;
	*pvs_online = 0;
	*pvs_offline = 0;

	if (!(dir = opendir(PVS_LOOKUP_DIR))) {
		log_sys_debug("opendir", PVS_LOOKUP_DIR);
		return 0;
	}

	/*
	 * Read each file in pvs_lookup to find dev->pvid, and if it's
	 * found save the vgname of the file it's found in.
	 */
	while (!vgname && (de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_LOOKUP_DIR, de->d_name) < 0) {
			log_warn("WARNING: Path %s/%s is too long.", PVS_LOOKUP_DIR, de->d_name);
			continue;
		}

		if (!(fp = fopen(path, "r"))) {
			log_warn("WARNING: Failed to open %s.", path);
			continue;
		}

		if (_lookup_file_contains_pvid(fp, dev->pvid)) {
			if ((vgname = dm_pool_strdup(cmd->mem, de->d_name)))
				/*
				 * stat pvid online file of each pvid listed in this file
				 * the list of pvids from the file is the alternative to
				 * using vg->pvs
				 */
				_lookup_file_count_pvid_files(fp, vgname, pvs_online, pvs_offline);
			else
				log_warn("WARNING: Failed to strdup vgname.");
		}

		if (fclose(fp))
			log_sys_debug("fclose", path);
	}
	if (closedir(dir))
		log_sys_debug("closedir", PVS_LOOKUP_DIR);

	*vgname_out = vgname;

	return (vgname) ? 1 : 0;
}

static void _count_pvid_files(struct volume_group *vg, int *pvs_online, int *pvs_offline)
{
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	struct pv_list *pvl;

	*pvs_online = 0;
	*pvs_offline = 0;

	dm_list_iterate_items(pvl, &vg->pvs) {
		memcpy(pvid, &pvl->pv->id.uuid, ID_LEN);
		if (online_pvid_file_exists(pvid))
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

	if (!vgchange_activate(cmd, vg, CHANGE_AAY, 1)) {
		log_error_pvscan(cmd, "%s: autoactivation failed.", vg->name);
		pp->activate_errors++;
	}

	return ECMD_PROCESSED;
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
	char file_devname[NAME_LEN];
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	char uuidstr[64] __attribute__((aligned(8)));
	struct pv_list *pvl;
	struct device_list *devl;
	struct device *dev;
	struct volume_group *vg;
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
		memcpy(pvid, &pvl->pv->id.uuid, ID_LEN);

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, pvid);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));
		memset(file_devname, 0, sizeof(file_devname));

		online_pvid_file_read(path, &file_major, &file_minor, file_vgname, file_devname);

		if (file_vgname[0] && strcmp(vgname, file_vgname)) {
			log_error_pvscan(cmd, "Wrong VG found for %d:%d PVID %s: %s vs %s",
				         file_major, file_minor, pvid, vgname, file_vgname);
			goto bad;
		}

		devno = MKDEV(file_major, file_minor);

		if (!(dev = setup_dev_in_dev_cache(cmd, devno, file_devname[0] ? file_devname : NULL))) {
			log_error_pvscan(cmd, "No device set up for online PV %d:%d %s PVID %s", file_major, file_minor, file_devname, pvid);
			goto bad;
		}

		/*
		 * Do not need to match device_id here, see comment after
		 * get_devs_from_saved_vg about relying on pvid online file.
		 */

		name1 = dev_name(dev);
		name2 = pvl->pv->device_hint;

		/* Probably pointless since dev is from online file which was already checked. */
		if (!strncmp(name2, "/dev/md", 7) && strncmp(name1, "/dev/md", 7)) {
			if (!id_write_format((const struct id *)pvid, uuidstr, sizeof(uuidstr)))
				uuidstr[0] = '\0';
			log_print_pvscan(cmd, "PVID %s read from %s last written to %s.", uuidstr, name1, name2);
			goto bad;
		}

		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
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
		log_print_pvscan(cmd, "VG %s not using quick activation.", vgname);
		*no_quick = 1;
		return ECMD_FAILED;
	}

	/*
	 * The list of devs do not need to be filtered or checked
	 * against the devices file because a dev is only returned
	 * that has a pv online file, and a dev will only have a
	 * pv online file if it's been processed by a previous
	 * pvscan, which did the filtering and devices file check.
	 */

	/*
	 * Lock the VG before scanning so we don't need to
	 * rescan in _vg_read.  (The lock_vol and the
	 * label rescan are then disabled in vg_read.)
	 */
	if (!lock_vol(cmd, vgname, LCK_VG_WRITE, NULL)) {
		log_error_pvscan(cmd, "activation for VG %s failed to lock VG.", vgname);
		return ECMD_FAILED;
	}

	/*
	 * Drop lvmcache info about the PV/VG that was saved
	 * when originally identifying the PV.
	 */
	lvmcache_destroy(cmd, 1, 1);

	label_scan_devs(cmd, NULL, &devs);

	if (!(vgid = lvmcache_vgid_from_vgname(cmd, vgname))) {
		log_error_pvscan(cmd, "activation for VG %s failed to find vgid.", vgname);
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
		log_error_pvscan(cmd, "activation for VG %s cannot read (%x).", vgname, error_flags);
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
		log_error_pvscan(cmd, "activation for VG %s found different devices.", vgname);
		ret = ECMD_FAILED;
		goto out;
	}

	log_debug("pvscan autoactivating VG %s.", vgname);

	if (!vgchange_activate(cmd, vg, CHANGE_AAY, 1)) {
		log_error_pvscan(cmd, "%s: autoactivation failed.", vg->name);
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
	int ret = ECMD_FAILED;

	if (!(handle = init_processing_handle(cmd, NULL))) {
		log_error_pvscan(cmd, "Failed to initialize processing handle.");
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
		if (!online_vg_file_create(cmd, sl->str)) {
			log_print_pvscan(cmd, "VG %s skip autoactivation.", sl->str);
			str_list_del(vgnames, sl->str);
			continue;
		}
		log_print_pvscan(cmd, "VG %s run autoactivation.", sl->str);
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

		log_debug("autoactivate slow");

		/*
		 * PROCESS_SKIP_SCAN: we have already done lvmcache_label_scan
		 * so tell process_each to skip it.
		 */

		if (!do_all)
			lvmcache_label_scan(cmd);

		read_flags |= PROCESS_SKIP_SCAN;

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

/*
 * The optimization in which only the pvscan arg devname is added to dev-cache
 * does not work if there's an lvm.conf filter containing symlinks to the dev
 * like /dev/disk/by-id/lvm-pv-uuid-xyz entries.  A full dev_cache_scan will
 * associate the symlinks with the system dev name passed to pvscan, which lets
 * filter-regex match the devname with the symlink name in the filter.
 */
static int _filter_uses_symlinks(struct cmd_context *cmd, int filter_cfg)
{
	const struct dm_config_node *cn;
	const struct dm_config_value *cv;
	const char *fname;

	if ((cn = find_config_tree_array(cmd, filter_cfg, NULL))) {
        	for (cv = cn->v; cv; cv = cv->next) {
			if (cv->type != DM_CFG_STRING)
				continue;
			if (!cv->v.str)
				continue;

			fname = cv->v.str;

			if (fname[0] != 'a')
				continue;

			if (strstr(fname, "/dev/disk/"))
				return 1;
			if (strstr(fname, "/dev/mapper/"))
				return 1;

			/* In case /dev/disk/by was omitted */
			if (strstr(fname, "lvm-pv-uuid"))
				return 1;
			if (strstr(fname, "dm-uuid"))
				return 1;
			if (strstr(fname, "wwn-"))
				return 1;
		}
	}

	return 0;
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
	int major = -1, minor = -1;

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

	/*
	 * If no devices file is used, and lvm.conf filter is set to
	 * accept /dev/disk/by-id/lvm-pv-uuid-xyz or another symlink,
	 * but pvscan --cache is passed devname or major:minor, so
	 * pvscan needs to match its arg device to the filter symlink.
	 * setup_dev_in_dev_cache() adds /dev/sda2 to dev-cache which
	 * does not match a symlink to /dev/sda2, so we need a full
	 * dev_cache_scan that will associate all symlinks to sda2,
	 * which allows filter-regex to work.  This case could be
	 * optimized if needed by adding dev-cache entries for each
	 * filter "a" entry (filter symlink patterns would still need
	 * a full dev_cache_scan.)
	 * (When no devices file is used and 69-dm-lvm.rules is
	 * used which calls pvscan directly, symlinks may not
	 * have been created by other rules when pvscan runs, so
	 * the full dev_cache_scan may still not find them.)
	 */
	if (!cmd->enable_devices_file && !cmd->enable_devices_list &&
	    (_filter_uses_symlinks(cmd, devices_filter_CFG) ||
	     _filter_uses_symlinks(cmd, devices_global_filter_CFG))) {
		log_print_pvscan(cmd, "finding all devices for filter symlinks.");
		dev_cache_scan(cmd);
	}

	/* pass NULL filter when getting devs from dev-cache, filtering is done separately */

	/* in common usage, no dev will be found for a devno */

	dm_list_iterate_items(arg, pvscan_args) {
		if (!arg->devname && !arg->devno)
			return_0;
		if (!(arg->dev = setup_dev_in_dev_cache(cmd, arg->devno, arg->devname))) {
			log_error_pvscan(cmd, "No device set up for arg %s %d:%d",
					 arg->devname ?: "", (int)MAJOR(arg->devno), (int)MINOR(arg->devno));
		}
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

static void _set_pv_devices_online(struct cmd_context *cmd, struct volume_group *vg)
{
	char path[PATH_MAX];
	char file_vgname[NAME_LEN];
	char file_devname[NAME_LEN];
	char pvid[ID_LEN+1] = { 0 };
	struct pv_list *pvl;
	struct device *dev;
	int major, minor;
	dev_t devno;

	dm_list_iterate_items(pvl, &vg->pvs) {
		memcpy(&pvid, &pvl->pv->id.uuid, ID_LEN);

		if (pvl->pv->status & MISSING_PV) {
			log_debug("set_pv_devices_online vg %s pv %s missing flag already set",
				  vg->name, pvid);
			continue;
		}

		if (!online_pvid_file_exists(pvid)) {
			log_debug("set_pv_devices_online vg %s pv %s no online file",
				  vg->name, pvid);
			pvl->pv->status |= MISSING_PV;
			continue;
		}

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, pvid);

		major = 0;
		minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));
		memset(file_devname, 0, sizeof(file_devname));

		online_pvid_file_read(path, &major, &minor, file_vgname, file_devname);

		if (file_vgname[0] && strcmp(vg->name, file_vgname)) {
			log_warn("WARNING: VG %s PV %s wrong vgname in online file %s",
				  vg->name, pvid, file_vgname);
			pvl->pv->status |= MISSING_PV;
			continue;
		}

		devno = MKDEV(major, minor);

		if (!(dev = setup_dev_in_dev_cache(cmd, devno, file_devname[0] ? file_devname : NULL))) {
			log_print_pvscan(cmd, "VG %s PV %s no device found for online PV %d:%d %s",
					 vg->name, pvid, major, minor, file_devname);
			pvl->pv->status |= MISSING_PV;
			continue;
		}

		log_debug("set_pv_devices_online vg %s pv %s is online %s",
			  vg->name, pvid, dev_name(dev));

		pvl->pv->dev = dev;
	}
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
	struct physical_volume *pv;
	const char *vgname = NULL;
	uint64_t devsize;
	uint32_t ext_version, ext_flags;
	int do_cache = arg_is_set(cmd, cache_long_ARG);
	int do_activate = arg_is_set(cmd, activate_ARG);
	int do_list_lvs = arg_is_set(cmd, listlvs_ARG);
	int do_list_vg = arg_is_set(cmd, listvg_ARG);
	int do_check_complete = arg_is_set(cmd, checkcomplete_ARG);
	int do_vgonline = arg_is_set(cmd, vgonline_ARG);
	int pvs_online;
	int pvs_offline;
	int pvs_unknown;
	int vg_complete = 0;
	int do_full_check;
	int ret = 1;

	dm_list_iterate_items_safe(devl, devl2, pvscan_devs) {
		dev = devl->dev;

		log_debug("online_devs %s %s", dev_name(dev), dev->pvid);

		if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
			if (!do_all)
				log_print_pvscan(cmd, "ignore %s with no lvm info.", dev_name(dev));
			continue;
		}

		ext_version = lvmcache_ext_version(info);
		ext_flags = lvmcache_ext_flags(info);
		if ((ext_version >= 2) && !(ext_flags & PV_EXT_USED)) {
			log_print_pvscan(cmd, "PV %s not used.", dev_name(dev));
			(*pv_count)++;
			continue;
		}

		fmt = lvmcache_fmt(info);
		if (!(fid = fmt->ops->create_instance(fmt, &fic))) {
			log_error("pvscan[%d] failed to create format instance.", getpid());
			ret = 0;
			continue;
		}

		vg = NULL;

		mda1 = lvmcache_get_dev_mda(dev, 1);
		mda2 = lvmcache_get_dev_mda(dev, 2);

		if (mda1 && !mda_is_ignored(mda1))
			vg = mda1->ops->vg_read(cmd, fid, "", mda1, NULL, NULL);

		if (!vg && mda2 && !mda_is_ignored(mda2))
			vg = mda2->ops->vg_read(cmd, fid, "", mda2, NULL, NULL);

		if (!vg) {
			log_print_pvscan(cmd, "PV %s has no VG metadata.", dev_name(dev));
			if (fid)
				fmt->ops->destroy_instance(fid);
			goto online;
		}

		set_pv_devices(fid, vg);

		if (!(pv = find_pv(vg, dev))) {
			log_print_pvscan(cmd, "PV %s not found in VG %s.", dev_name(dev), vg->name);
			release_vg(vg);
			continue;
		}

		devsize = dev->size;
		if (!devsize &&
		    !dev_get_size(dev, &devsize)) {
			log_print_pvscan(cmd, "PV %s missing device size.", dev_name(dev));
			release_vg(vg);
			continue;
		}
		do_full_check = 0;

		/* If use_full_md_check is set then this has already been done by filter. */
		if (!cmd->use_full_md_check && (cmd->dev_types->md_major != MAJOR(dev->dev))) {
			if (devsize && (pv->size != devsize))
				do_full_check = 1;
			if (pv->device_hint && !strncmp(pv->device_hint, "/dev/md", 7))
				do_full_check = 1;
		}

		if (do_full_check && dev_is_md_component(cmd, dev, NULL, 1)) {
			log_print_pvscan(cmd, "ignore md component %s.", dev_name(dev));
			release_vg(vg);
			continue;
		}

		if (vg_is_shared(vg)) {
			log_print_pvscan(cmd, "PV %s ignore shared VG.", dev_name(dev));
			release_vg(vg);
			continue;
		}

		if (vg->system_id && vg->system_id[0] &&
		    cmd->system_id && cmd->system_id[0] &&
		    vg_is_foreign(vg)) {
			log_verbose("Ignore PV %s with VG system id %s with our system id %s",
				    dev_name(dev), vg->system_id, cmd->system_id);
			log_print_pvscan(cmd, "PV %s ignore foreign VG.", dev_name(dev));
			release_vg(vg);
			continue;
		}

		if (vg_is_exported(vg)) {
			log_print_pvscan(cmd, "PV %s ignore exported VG.", dev_name(dev));
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
		 * The command creates/checks online files only when --cache is used.
		 */
		if (do_cache && !online_pvid_file_create(cmd, dev, vg ? vg->name : NULL)) {
			log_error_pvscan(cmd, "PV %s failed to create online file.", dev_name(dev));
			release_vg(vg);
			ret = 0;
			continue;
		}

		/*
		 * A plain pvscan --cache <dev> just creates the online file.
		 */
		if (!do_activate && !do_list_lvs && !do_list_vg) {
			log_print_pvscan(cmd, "PV %s online.", dev_name(dev));
			release_vg(vg);
			continue;
		}

		/*
		 * Check if all the PVs for this VG are online.  If the arrival
		 * of this dev completes the VG, then save the vgname in
		 * complete_vgnames (activation phase will want to know which
		 * VGs to activate.)
		 */
		if (do_activate || do_check_complete) {
			pvs_online = 0;
			pvs_offline = 0;
			pvs_unknown = 0;
			vg_complete = 0;

			if (vg) {
				/*
				 * Check if the VG is complete by checking that
				 * pvs_online/<pvid> files exist for all vg->pvs.
				 */
				log_debug("checking all pvid files from vg %s", vg->name);
				_count_pvid_files(vg, &pvs_online, &pvs_offline);

				/*
				 * When there is more than one PV in the VG, write
				 * /run/lvm/pvs_lookup/<vgname> with a list of PVIDs in
				 * the VG.  This is used in case a later PV comes
				 * online that has no metadata, in which case pvscan
				 * for that PV needs to use the lookup file to check if
				 * the VG is complete.  The lookup file is also used by
				 * vgchange -aay --autoactivation event <vgname>
				 * to check if all pvs_online files for the VG exist.
				 *
				 * For multiple concurrent pvscan's, they will race to
				 * create the lookup file and the first will succeed.
				 *
				 * After writing the lookup file, recheck pvid files to
				 * resolve a possible race with another pvscan reading
				 * the lookup file that missed it.
				 */
				if (dm_list_size(&vg->pvs) > 1) {
					if (_write_lookup_file(cmd, vg)) {
						if (pvs_offline) {
							log_debug("rechecking all pvid files from vg %s", vg->name);
							_count_pvid_files(vg, &pvs_online, &pvs_offline);
							if (!pvs_offline)
								log_print_pvscan(cmd, "VG %s complete after recheck.", vg->name);
						}
					}
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
				log_print_pvscan(cmd, "PV %s online, VG unknown.", dev_name(dev));
				vg_complete = 0;
	
			} else if (pvs_offline) {
				log_print_pvscan(cmd, "PV %s online, VG %s incomplete (need %d).",
						 dev_name(dev), vgname, pvs_offline);
				vg_complete = 0;
	
			} else {
				log_print_pvscan(cmd, "PV %s online, VG %s is complete.", dev_name(dev), vgname);
				if (!str_list_add(cmd->mem, complete_vgnames, dm_pool_strdup(cmd->mem, vgname)))
					stack;
				vg_complete = 1;
			}
		}

		if (!vgname && vg)
			vgname = vg->name;

		if (do_list_vg || do_list_lvs) {
			if (!vgname) {
				log_print("VG unknown");
			} else if (!do_check_complete) {
				log_print("VG %s", vgname);
			} else if (vg_complete) {
				if (do_vgonline && !online_vg_file_create(cmd, vgname)) {
					log_print("VG %s finished", vgname);
				} else {
					/*
					 * A udev rule imports KEY=val from a program's stdout.
					 * Other output causes udev to ignore everything.
					 * Run pvscan from udev rule using --udevoutput to
					 * enable this printf, and suppress all log output
					 */
					if (arg_is_set(cmd, udevoutput_ARG))
						printf("LVM_VG_NAME_COMPLETE='%s'\n", vgname);
					else
						log_print("VG %s complete", vgname);
				}
			} else {
				if (arg_is_set(cmd, udevoutput_ARG))
					printf("LVM_VG_NAME_INCOMPLETE='%s'\n", vgname);
				else
					log_print("VG %s incomplete", vgname);
			}

			/*
			 * When the VG is complete|finished, we could print
			 * a list of devices in the VG, by reading the pvid files
			 * that were counted, which provides major:minor of each
			 * device and using that to get the struct dev and dev_name.
			 * The user could pass this list of devices to --devices
			 * to optimize a subsequent command (activation) on the VG.
			 * Just call set_pv_devices_online (if not done othewise)
			 * since that finds the devs.
			 */
		}

		if (do_list_lvs && !vg) {
			/* require all PVs used for booting have metadata */
			log_print_pvscan(cmd, "Cannot list LVs from device without metadata.");
		}

		if (do_list_lvs && vg) {
			struct dm_list lvs_list;
			struct lv_list *lvl;

			dm_list_init(&lvs_list);

			/*
			 * For each vg->pvs entry, get the dev based on the online file
			 * for the pvid and set pv->dev or pv->status MISSING_PV.
			 */
			_set_pv_devices_online(cmd, vg);

			/*
			 * lvs_list are LVs that use dev.
			 */
			if (!get_visible_lvs_using_pv(cmd, vg, dev, &lvs_list))
				log_print_pvscan(cmd, "Failed to find LVs using %s.", dev_name(dev));

			if (!do_check_complete) {
				dm_list_iterate_items(lvl, &lvs_list)
					log_print("LV %s", display_lvname(lvl->lv));
			} else if (vg_complete) {
				/*
				 * A shortcut; the vg complete implies all lvs are complete.
				 */
				dm_list_iterate_items(lvl, &lvs_list)
					log_print("LV %s complete", display_lvname(lvl->lv));
			} else {
				/*
				 * For each LV in VG, check if all devs are present.
				 * Sets the PARTIAL flag on LVs that are not complete.
				 */
				if (!vg_mark_partial_lvs(vg, 1))
					log_print_pvscan(cmd, "Failed to check partial lvs.");

				dm_list_iterate_items(lvl, &lvs_list) {
					if (!lv_is_partial(lvl->lv))
						log_print("LV %s complete", display_lvname(lvl->lv));
					else
						log_print("LV %s incomplete", display_lvname(lvl->lv));
				}
			}
		}

		/*
		 * When "pvscan --cache -aay <dev>" completes the vg, save the
		 * struct vg to use for quick activation function.
		 */
		if (do_activate && !saved_vg && vg && vg_complete && !do_all && (dm_list_size(pvscan_devs) == 1))
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

	_online_files_remove(PVS_ONLINE_DIR);
	_online_files_remove(VGS_ONLINE_DIR);
	_online_files_remove(PVS_LOOKUP_DIR);

	unlink_searched_devnames(cmd);

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

/*
 * If /dev/sda* of /dev/vda* is excluded by the devices file
 * it's usually a misconfiguration that prevents proper booting,
 * so make it a special case to give extra info to help debugging.
 */
static void _warn_excluded_root(struct cmd_context *cmd, struct device *dev)
{
	struct dev_use *du;
	const char *cur_idname;

	if (!(du = get_du_for_devname(cmd, dev_name(dev)))) {
		log_warn("WARNING: no autoactivation for %s: not found in system.devices.", dev_name(dev));
		return;
	}

	cur_idname = device_id_system_read(cmd, dev, du->idtype);

	log_warn("WARNING: no autoactivation for %s: system.devices %s current %s.",
		 dev_name(dev), du->idname, cur_idname ?: "missing device id");
}

static int _pvscan_cache_args(struct cmd_context *cmd, int argc, char **argv,
			      struct dm_list *complete_vgnames)
{
	struct dm_list pvscan_args; /* struct pvscan_arg */
	struct dm_list pvscan_devs; /* struct device_list */
	struct pvscan_arg *arg;
	struct device_list *devl, *devl2;
	int relax_deviceid_filter = 0;
	int pv_count = 0;
	int ret;

	dm_list_init(&pvscan_args);
	dm_list_init(&pvscan_devs);

	cmd->expect_missing_vg_device = 1;

	/*
	 * Special pvscan-specific setup steps to avoid looking
	 * at any devices except for device args.
	 * Read devices file and determine if devices file will be used.
	 * Does not do dev_cache_scan (adds nothing to dev-cache), and
	 * does not do any device id matching.
	 */
	if (!setup_devices_for_online_autoactivation(cmd)) {
		log_error_pvscan(cmd, "Failed to set up devices.");
		return 0;
	}

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
	 *
	 * We want pvscan autoactivation to work when using a devices file
	 * containing idtype=devname, in cases when the devname changes
	 * after reboot.  To make this work, we have to relax the devices
	 * file restrictions somewhat here in cases where the devices file
	 * contains entries with idtype=devname: disable filter-deviceid
	 * when applying the nodata filters here, and read the label header.
	 * Once the label header is read, check if the label header pvid
	 * is in the devices file, and ignore the device if it's not.
	 * The downside of this is that pvscans from the system will read
	 * devs belonging to other devices files.
	 * Enable/disable this behavior with a config setting?
	 */
	 
	log_debug("pvscan_cache_args: filter devs nodata");

	/*
	 * Match dev args with the devices file because special/optimized
	 * device setup was used above which does not check the devices file.
	 * If a match fails here do not exclude it, that will be done below by
	 * passes_filter() which runs filter-deviceid. The
	 * relax_deviceid_filter case needs to be able to work around
	 * unmatching devs.
	 */

	if (cmd->enable_devices_file) {
		dm_list_iterate_items(devl, &pvscan_devs)
			device_ids_match_dev(cmd, devl->dev);

	}
	if (cmd->enable_devices_list)
		device_ids_match_device_list(cmd);

	if (cmd->enable_devices_file && device_ids_use_devname(cmd)) {
		relax_deviceid_filter = 1;
		cmd->filter_deviceid_skip = 1;
	}

	cmd->filter_nodata_only = 1;

	dm_list_iterate_items_safe(devl, devl2, &pvscan_devs) {
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL)) {
			log_print_pvscan(cmd, "%s excluded: %s.",
					 dev_name(devl->dev), dev_filtered_reason(devl->dev));
			dm_list_del(&devl->list);

			/* Special case warning when probable root dev is missing from system.devices */
			if ((devl->dev->filtered_flags & DEV_FILTERED_DEVICES_FILE) &&
			    (!strncmp(dev_name(devl->dev), "/dev/sda", 8) ||
			     !strncmp(dev_name(devl->dev), "/dev/vda", 8)))
				_warn_excluded_root(cmd, devl->dev);
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
		int has_pvid;

		if (!label_read_pvid(devl->dev, &has_pvid)) {
			log_print_pvscan(cmd, "%s cannot read label.", dev_name(devl->dev));
			dm_list_del(&devl->list);
			continue;
		}

		if (!has_pvid) {
			/* Not an lvm device */
			log_print_pvscan(cmd, "%s not an lvm device.", dev_name(devl->dev));
			dm_list_del(&devl->list);
			continue;
		}

		/*
		 * filter-deviceid is not being used because of unstable devnames,
		 * so in place of that check if the pvid is in the devices file.
		 */
		if (relax_deviceid_filter) {
			if (!get_du_for_pvid(cmd, devl->dev->pvid)) {
				log_print_pvscan(cmd, "%s excluded by devices file (checking PVID).",
					         dev_name(devl->dev));
				dm_list_del(&devl->list);
				continue;
			}
		}

		/* Applies all filters, including those that need data from dev. */
		if (!cmd->filter->passes_filter(cmd, cmd->filter, devl->dev, NULL)) {
			log_print_pvscan(cmd, "%s excluded: %s.",
					 dev_name(devl->dev), dev_filtered_reason(devl->dev));
			dm_list_del(&devl->list);
		}
	}

	if (relax_deviceid_filter)
		cmd->filter_deviceid_skip = 0;

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
	if (pv_count) {
		invalidate_hints(cmd);
		unlink_searched_devnames(cmd);
	}

	return ret;
}

static int _get_autoactivation(struct cmd_context *cmd, int event_activation, int *skip_command)
{
	const char *aa_str;

	if (!(aa_str = arg_str_value(cmd, autoactivation_ARG, NULL)))
		return 1;

	if (strcmp(aa_str, "event")) {
		log_print_pvscan(cmd, "Skip pvscan for unknown autoactivation value.");
		*skip_command = 1;
		return 1;
	}
	
	if (!event_activation) {
		log_print_pvscan(cmd, "Skip pvscan for event with event_activation=0.");
		*skip_command = 1;
		return 1;
	}

	return 1;
}

int pvscan_cache_cmd(struct cmd_context *cmd, int argc, char **argv)
{
	struct pvscan_aa_params pp = { 0 };
	struct dm_list complete_vgnames;
	int do_activate = arg_is_set(cmd, activate_ARG);
	int event_activation;
	int skip_command = 0;
	int devno_args = 0;
	int do_all;
	int ret;

	dm_list_init(&complete_vgnames);

	cmd->check_devs_used = 0;

	cmd->print_device_id_not_found = 0;

	cmd->ignore_device_name_mismatch = 1;

	event_activation = find_config_tree_bool(cmd, global_event_activation_CFG, NULL);

	if (do_activate && !event_activation) {
		log_verbose("Ignoring pvscan --cache -aay because event_activation is disabled.");
		return ECMD_PROCESSED;
	}

	/*
	 * lvm udev rules call:
	 *   pvscan --cache --listvg|--listlvs --checkcomplete PV
	 * when PVs appear, even if event_activation=0 in lvm.conf.
	 *
	 * The udev rules will do autoactivation if they see complete
	 * VGs/LVs reported from the pvscan.
	 *
	 * When event_activation=0 we do not want to do autoactivation
	 * from udev events, so we need the pvscan to not report any
	 * complete VGs/LVs when event_activation=0 so that the udev
	 * rules do not attempt to autoactivate.
	 */

	if (arg_is_set(cmd, checkcomplete_ARG) && !event_activation) {
		if (arg_is_set(cmd, udevoutput_ARG))
			printf("LVM_EVENT_ACTIVATION=0\n");
		else
			log_print_pvscan(cmd, "Ignoring pvscan with --checkcomplete because event_activation is disabled.");
		return ECMD_PROCESSED;
	}

	/*
	 * Do not use udev for device listing or device info because pvscan
	 * is used to populate udev info.
	 */
	init_obtain_device_list_from_udev(0);
	init_external_device_info_source(DEV_EXT_NONE);

	if (arg_is_set(cmd, major_ARG) + arg_is_set(cmd, minor_ARG))
		devno_args = 1;

	if (devno_args && (!arg_is_set(cmd, major_ARG) || !arg_is_set(cmd, minor_ARG))) {
		log_error("Both --major and --minor required to identify devices.");
		return EINVALID_CMD_LINE;
	}

	do_all = !argc && !devno_args;

	online_dir_setup(cmd);

	if (do_all) {
		if (!_pvscan_cache_all(cmd, argc, argv, &complete_vgnames))
			return ECMD_FAILED;
	} else {
		if (!arg_is_set(cmd, checkcomplete_ARG) && !event_activation) {
			/* Avoid doing anything for device removal: pvscan --cache <devno> */
			log_verbose("Ignoring pvscan --cache because event_activation is disabled.");
			return ECMD_PROCESSED;
		}

		if (!_get_autoactivation(cmd, event_activation, &skip_command))
			return_ECMD_FAILED;

		if (skip_command)
			return ECMD_PROCESSED;

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

