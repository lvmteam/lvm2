/*
 * Copyright (C) 2021 Red Hat, Inc. All rights reserved.
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

#include "base/memory/zalloc.h"
#include "lib/misc/lib.h"
#include "lib/commands/toolcontext.h"
#include "lib/device/device.h"
#include "lib/device/device_id.h"
#include "lib/device/online.h"

#include <dirent.h>

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

int online_pvid_file_read(char *path, int *major, int *minor, char *vgname)
{
	char buf[MAX_PVID_FILE_SIZE] = { 0 };
	char *name;
	int fd, rv;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		log_warn("Failed to open %s", path);
		return 0;
	}

	rv = read(fd, buf, sizeof(buf) - 1);
	if (close(fd))
		log_sys_debug("close", path);
	if (!rv || rv < 0) {
		log_warn("No info in %s", path);
		return 0;
	}
	buf[rv] = 0; /* \0 terminated buffer */

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

void free_po_list(struct dm_list *list)
{
	struct pv_online *po, *po2;

	dm_list_iterate_items_safe(po, po2, list) {
		dm_list_del(&po->list);
		free(po);
	}
}

int get_pvs_online(struct dm_list *pvs_online, const char *vgname)
{
	char path[PATH_MAX];
	char file_vgname[NAME_LEN];
	DIR *dir;
	struct dirent *de;
	struct pv_online *po;
	int file_major = 0, file_minor = 0;

	if (!(dir = opendir(PVS_ONLINE_DIR)))
		return 0;

	while ((de = readdir(dir))) {
		if (de->d_name[0] == '.')
			continue;

		if (strlen(de->d_name) != ID_LEN)
			continue;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, de->d_name);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));

		if (!online_pvid_file_read(path, &file_major, &file_minor, file_vgname))
			continue;

		if (vgname && strcmp(file_vgname, vgname))
			continue;

		if (!(po = zalloc(sizeof(*po))))
			continue;

		memcpy(po->pvid, de->d_name, ID_LEN);
		if (file_major || file_minor)
			po->devno = MKDEV(file_major, file_minor);
		if (file_vgname[0])
			strncpy(po->vgname, file_vgname, NAME_LEN-1);

		dm_list_add(pvs_online, &po->list);
	}
	if (closedir(dir))
		log_sys_debug("closedir", PVS_ONLINE_DIR);
	return 1;
}

/*
 * When a PV goes offline, remove the vg online file for that VG
 * (even if other PVs for the VG are still online).  This means
 * that the vg will be activated again when it becomes complete.
 */

void online_vg_file_remove(const char *vgname)
{
	char path[PATH_MAX];

	if (dm_snprintf(path, sizeof(path), "%s/%s", VGS_ONLINE_DIR, vgname) < 0) {
		log_error("Path %s/%s is too long.", VGS_ONLINE_DIR, vgname);
		return;
	}

	log_debug("Unlink vg online: %s", path);

	if (unlink(path) && (errno != ENOENT))
		log_sys_debug("unlink", path);
}

int online_vg_file_create(struct cmd_context *cmd, const char *vgname)
{
	char path[PATH_MAX];
	int fd;

	if (dm_snprintf(path, sizeof(path), "%s/%s", VGS_ONLINE_DIR, vgname) < 0) {
		log_error_pvscan(cmd, "Path %s/%s is too long.", VGS_ONLINE_DIR, vgname);
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

int online_pvid_file_create(struct cmd_context *cmd, struct device *dev, const char *vgname)
{
	char path[PATH_MAX];
	char buf[MAX_PVID_FILE_SIZE] = { 0 };
	char file_vgname[NAME_LEN];
	int file_major = 0, file_minor = 0;
	int major, minor;
	int fd;
	int rv;
	int len;
	int len1 = 0;
	int len2 = 0;

	major = (int)MAJOR(dev->dev);
	minor = (int)MINOR(dev->dev);

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, dev->pvid) < 0) {
		log_error_pvscan(cmd, "Path %s/%s is too long.", PVS_ONLINE_DIR, dev->pvid);
		return 0;
	}

	if ((len1 = dm_snprintf(buf, sizeof(buf), "%d:%d\n", major, minor)) < 0) {
		log_error_pvscan(cmd, "Cannot create online file path for %s %d:%d.", dev_name(dev), major, minor);
		return 0;
	}

	if (vgname) {
		if ((len2 = dm_snprintf(buf + len1, sizeof(buf) - len1, "vg:%s\n", vgname)) < 0) {
			log_print_pvscan(cmd, "Incomplete online file for %s %d:%d vg %s.", dev_name(dev), major, minor, vgname);
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
		log_error_pvscan(cmd, "Failed to create online file for %s path %s error %d", dev_name(dev), path, errno);
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

	online_pvid_file_read(path, &file_major, &file_minor, file_vgname);

	if ((file_major == major) && (file_minor == minor)) {
		log_debug("Existing online file for %d:%d", major, minor);
		return 1;
	}

	/* Don't know how vgname might not match, but it's not good so fail. */

	if ((file_major != major) || (file_minor != minor))
		log_error_pvscan(cmd, "PV %s is duplicate for PVID %s on %d:%d and %d:%d.",
			         dev_name(dev), dev->pvid, major, minor, file_major, file_minor);

	if (file_vgname[0] && vgname && strcmp(file_vgname, vgname))
		log_error_pvscan(cmd, "PV %s has unexpected VG %s vs %s.",
			         dev_name(dev), vgname, file_vgname);

	return 0;
}

int online_pvid_file_exists(const char *pvid)
{
	char path[PATH_MAX] = { 0 };
	struct stat buf;
	int rv;

	if (dm_snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, pvid) < 0) {
		log_debug(INTERNAL_ERROR "Path %s/%s is too long.", PVS_ONLINE_DIR, pvid);
		return 0;
	}

	log_debug("Check pv online: %s", path);

	rv = stat(path, &buf);
	if (!rv) {
		log_debug("Check pv online %s: yes", pvid);
		return 1;
	}
	log_debug("Check pv online %s: no", pvid);
	return 0;
}

int get_pvs_lookup(struct dm_list *pvs_online, const char *vgname)
{
	char lookup_path[PATH_MAX] = { 0 };
	char path[PATH_MAX] = { 0 };
	char line[64];
	char pvid[ID_LEN + 1] __attribute__((aligned(8))) = { 0 };
	char file_vgname[NAME_LEN];
	struct pv_online *po;
	int file_major = 0, file_minor = 0;
	FILE *fp;

	if (dm_snprintf(lookup_path, sizeof(path), "%s/%s", PVS_LOOKUP_DIR, vgname) < 0)
		return_0;

	if (!(fp = fopen(lookup_path, "r")))
		return_0;

	while (fgets(line, sizeof(line), fp)) {
		memcpy(pvid, line, ID_LEN);
		if (strlen(pvid) != ID_LEN)
			goto_bad;

		memset(path, 0, sizeof(path));
		snprintf(path, sizeof(path), "%s/%s", PVS_ONLINE_DIR, pvid);

		file_major = 0;
		file_minor = 0;
		memset(file_vgname, 0, sizeof(file_vgname));

		if (!online_pvid_file_read(path, &file_major, &file_minor, file_vgname))
			goto_bad;

		if (vgname && strcmp(file_vgname, vgname))
			goto_bad;

		if (!(po = zalloc(sizeof(*po))))
			goto_bad;

		memcpy(po->pvid, pvid, ID_LEN);
		if (file_major || file_minor)
			po->devno = MKDEV(file_major, file_minor);
		if (file_vgname[0])
			strncpy(po->vgname, file_vgname, NAME_LEN-1);

		dm_list_add(pvs_online, &po->list);
	}

	fclose(fp);
	return 1;

bad:
	free_po_list(pvs_online);
	fclose(fp);
	return 0;
}

void online_dir_setup(struct cmd_context *cmd)
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
		log_error_pvscan(cmd, "Failed to create %s %d", DEFAULT_RUN_DIR, errno);

do_pvs:
	if (!stat(PVS_ONLINE_DIR, &st))
		goto do_vgs;

	log_debug("Creating pvs_online_dir.");
	dm_prepare_selinux_context(PVS_ONLINE_DIR, S_IFDIR);
	rv = mkdir(PVS_ONLINE_DIR, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(PVS_ONLINE_DIR, &st))
		log_error_pvscan(cmd, "Failed to create %s %d", PVS_ONLINE_DIR, errno);

do_vgs:
	if (!stat(VGS_ONLINE_DIR, &st))
		goto do_lookup;

	log_debug("Creating vgs_online_dir.");
	dm_prepare_selinux_context(VGS_ONLINE_DIR, S_IFDIR);
	rv = mkdir(VGS_ONLINE_DIR, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(VGS_ONLINE_DIR, &st))
		log_error_pvscan(cmd, "Failed to create %s %d", VGS_ONLINE_DIR, errno);

do_lookup:
	if (!stat(PVS_LOOKUP_DIR, &st))
		return;

	log_debug("Creating pvs_lookup_dir.");
	dm_prepare_selinux_context(PVS_LOOKUP_DIR, S_IFDIR);
	rv = mkdir(PVS_LOOKUP_DIR, 0755);
	dm_prepare_selinux_context(NULL, 0);

	if ((rv < 0) && stat(PVS_LOOKUP_DIR, &st))
		log_error_pvscan(cmd, "Failed to create %s %d", PVS_LOOKUP_DIR, errno);
}
