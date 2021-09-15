/*
 * Copyright (C) 2020-2021 Seagate Ltd.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#define _XOPEN_SOURCE 500  /* pthread */
#define _ISOC99_SOURCE

#include "tools/tool.h"

#include "daemon-server.h"
#include "lib/mm/xlate.h"

#include "lvmlockd-internal.h"
#include "daemons/lvmlockd/lvmlockd-client.h"

#include "ilm.h"

#include <blkid/blkid.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <regex.h>
#include <stddef.h>
#include <syslog.h>
#include <sys/sysmacros.h>
#include <time.h>

#define IDM_TIMEOUT	60000	/* unit: millisecond, 60 seconds */

/*
 * Each lockspace thread has its own In-Drive Mutex (IDM) lock manager's
 * connection.  After established socket connection, the lockspace has
 * been created in IDM lock manager and afterwards use the socket file
 * descriptor to send any requests for lock related operations.
 */

struct lm_idm {
	int sock;	/* IDM lock manager connection */
};

struct rd_idm {
	struct idm_lock_id id;
	struct idm_lock_op op;
	uint64_t vb_timestamp;
	struct val_blk *vb;
};

int lm_data_size_idm(void)
{
	return sizeof(struct rd_idm);
}

static uint64_t read_utc_us(void)
{
	struct timespec cur_time;

	clock_gettime(CLOCK_REALTIME, &cur_time);

	/*
	 * Convert to microseconds unit.  IDM reserves the MSB in 8 bytes
	 * and the low 56 bits are used for timestamp; 56 bits can support
	 * calendar year to 2284, so it has 260 years for overflow.  Thus it
	 * is quite safe for overflow issue when wrote this code.
	 */
	return cur_time.tv_sec * 1000000 + cur_time.tv_nsec / 1000;
}

static int uuid_read_format(char *uuid_str, const char *buffer)
{
	int out = 0;

	/* just strip out any dashes */
	while (*buffer) {

		if (*buffer == '-') {
			buffer++;
			continue;
		}

		if (out >= 32) {
			log_error("Too many characters to be uuid.");
			return -1;
		}

		uuid_str[out++] = *buffer;
		buffer++;
	}

	if (out != 32) {
		log_error("Couldn't read uuid: incorrect number of "
			  "characters.");
		return -1;
	}

	return 0;
}

#define SYSFS_ROOT		"/sys"
#define BUS_SCSI_DEVS		"/bus/scsi/devices"

static struct idm_lock_op glb_lock_op;

static void lm_idm_free_dir_list(struct dirent **dir_list, int dir_num)
{
	int i;

	for (i = 0; i < dir_num; ++i)
		free(dir_list[i]);
	free(dir_list);
}

static int lm_idm_scsi_directory_select(const struct dirent *s)
{
	regex_t regex;
	int ret;

	/* Only select directory with the format x:x:x:x */
	ret = regcomp(&regex, "^[0-9]+:[0-9]+:[0-9]+:[0-9]+$", REG_EXTENDED);
	if (ret)
		return 0;

	ret = regexec(&regex, s->d_name, 0, NULL, 0);
	if (!ret) {
		regfree(&regex);
		return 1;
	}

	regfree(&regex);
	return 0;
}

static int lm_idm_scsi_find_block_dirctory(const char *block_path)
{
	struct stat stats;

	if ((stat(block_path, &stats) >= 0) && S_ISDIR(stats.st_mode))
		return 0;

	return -1;
}

static int lm_idm_scsi_block_node_select(const struct dirent *s)
{
	if (DT_LNK != s->d_type && DT_DIR != s->d_type)
		return 0;

	if (DT_DIR == s->d_type) {
		/* Skip this directory: '.' and parent: '..' */
		if (!strcmp(s->d_name, ".") || !strcmp(s->d_name, ".."))
			return 0;
	}

	return 1;
}

static int lm_idm_scsi_find_block_node(const char *blk_path, char **blk_dev)
{
        struct dirent **dir_list;
        int dir_num;

        dir_num = scandir(blk_path, &dir_list, lm_idm_scsi_block_node_select, NULL);
        if (dir_num < 0) {
		log_error("Cannot find valid directory entry in %s", blk_path);
                return -1;
	}

	/*
	 * Should have only one block name under the path, if the dir_num is
	 * not 1 (e.g. 0 or any number bigger than 1), it must be wrong and
	 * should never happen.
	 */
	if (dir_num == 1)
		*blk_dev = strdup(dir_list[0]->d_name);
	else
		*blk_dev = NULL;

	lm_idm_free_dir_list(dir_list, dir_num);

	if (!*blk_dev)
		return -1;

        return dir_num;
}

static int lm_idm_scsi_search_propeller_partition(char *dev)
{
	int i, nparts;
	blkid_probe pr;
	blkid_partlist ls;
	int found = -1;

	pr = blkid_new_probe_from_filename(dev);
	if (!pr) {
		log_error("%s: failed to create a new libblkid probe", dev);
		return -1;
	}

	/* Binary interface */
	ls = blkid_probe_get_partitions(pr);
	if (!ls) {
		log_error("%s: failed to read partitions", dev);
		return -1;
	}

	/* List partitions */
	nparts = blkid_partlist_numof_partitions(ls);
	if (!nparts)
		goto done;

	for (i = 0; i < nparts; i++) {
		const char *p;
		blkid_partition par = blkid_partlist_get_partition(ls, i);

		p = blkid_partition_get_name(par);
		if (p) {
			log_debug("partition name='%s'", p);

			if (!strcmp(p, "propeller"))
				found = blkid_partition_get_partno(par);
		}

		if (found >= 0)
			break;
	}

done:
	blkid_free_probe(pr);
	return found;
}

static char *lm_idm_scsi_get_block_device_node(const char *scsi_path)
{
	char *blk_path = NULL;
	char *blk_dev = NULL;
	char *dev_node = NULL;
	int ret;

	/*
	 * Locate the "block" directory, such like:
	 * /sys/bus/scsi/devices/1:0:0:0/block
	 */
	ret = asprintf(&blk_path, "%s/%s", scsi_path, "block");
	if (ret < 0) {
		log_error("Fail to allocate block path for %s", scsi_path);
		goto fail;
	}

	ret = lm_idm_scsi_find_block_dirctory(blk_path);
	if (ret < 0) {
		log_error("Fail to find block path %s", blk_path);
		goto fail;
	}

	/*
	 * Locate the block device name, such like:
	 * /sys/bus/scsi/devices/1:0:0:0/block/sdb
	 *
	 * After return from this function and if it makes success,
	 * the global variable "blk_dev" points to the block device
	 * name, in this example it points to string "sdb".
	 */
	ret = lm_idm_scsi_find_block_node(blk_path, &blk_dev);
	if (ret < 0) {
		log_error("Fail to find block node");
		goto fail;
	}

	ret = asprintf(&dev_node, "/dev/%s", blk_dev);
	if (ret < 0) {
		log_error("Fail to allocate memory for blk node path");
		goto fail;
	}

	ret = lm_idm_scsi_search_propeller_partition(dev_node);
	if (ret < 0)
		goto fail;

	free(blk_path);
	free(blk_dev);
	return dev_node;

fail:
	free(blk_path);
	free(blk_dev);
	free(dev_node);
	return NULL;
}

static int lm_idm_get_gl_lock_pv_list(void)
{
	struct dirent **dir_list;
	char scsi_bus_path[PATH_MAX];
	char *drive_path;
	int i, dir_num, ret;

	if (glb_lock_op.drive_num)
		return 0;

	snprintf(scsi_bus_path, sizeof(scsi_bus_path), "%s%s",
		 SYSFS_ROOT, BUS_SCSI_DEVS);

	dir_num = scandir(scsi_bus_path, &dir_list,
			  lm_idm_scsi_directory_select, NULL);
	if (dir_num < 0) {  /* scsi mid level may not be loaded */
		log_error("Attached devices: none");
		return -1;
	}

	for (i = 0; i < dir_num; i++) {
		char *scsi_path;

		ret = asprintf(&scsi_path, "%s/%s", scsi_bus_path,
			       dir_list[i]->d_name);
		if (ret < 0) {
			log_error("Fail to allocate memory for scsi directory");
			goto failed;
		}

		if (glb_lock_op.drive_num >= ILM_DRIVE_MAX_NUM) {
			log_error("Global lock: drive number %d exceeds limitation (%d) ?!",
				  glb_lock_op.drive_num, ILM_DRIVE_MAX_NUM);
			free(scsi_path);
			goto failed;
		}

		drive_path = lm_idm_scsi_get_block_device_node(scsi_path);
		if (!drive_path) {
			free(scsi_path);
			continue;
		}

		glb_lock_op.drives[glb_lock_op.drive_num] = drive_path;
		glb_lock_op.drive_num++;

		free(scsi_path);
	}

	lm_idm_free_dir_list(dir_list, dir_num);
	return 0;

failed:
	lm_idm_free_dir_list(dir_list, dir_num);

	for (i = 0; i < glb_lock_op.drive_num; i++) {
		if (glb_lock_op.drives[i]) {
			free(glb_lock_op.drives[i]);
			glb_lock_op.drives[i] = NULL;
		}
	}

	return -1;
}

static void lm_idm_update_vb_timestamp(uint64_t *vb_timestamp)
{
	uint64_t utc_us = read_utc_us();

	/*
	 * It's possible that the multiple nodes have no clock
	 * synchronization with microsecond prcision and the time
	 * is going backward.  For this case, simply increment the
	 * existing timestamp and write out to drive.
	 */
	if (*vb_timestamp >= utc_us)
		(*vb_timestamp)++;
	else
		*vb_timestamp = utc_us;
}

int lm_prepare_lockspace_idm(struct lockspace *ls)
{
	struct lm_idm *lm = NULL;

	lm = malloc(sizeof(struct lm_idm));
	if (!lm) {
		log_error("S %s prepare_lockspace_idm fail to allocate lm_idm for %s",
			  ls->name, ls->vg_name);
		return -ENOMEM;
	}
	memset(lm, 0x0, sizeof(struct lm_idm));

	ls->lm_data = lm;
	log_debug("S %s prepare_lockspace_idm done", ls->name);
	return 0;
}

int lm_add_lockspace_idm(struct lockspace *ls, int adopt)
{
	char killpath[IDM_FAILURE_PATH_LEN];
	char killargs[IDM_FAILURE_ARGS_LEN];
	struct lm_idm *lmi = (struct lm_idm *)ls->lm_data;
	int rv;

	if (daemon_test)
		return 0;

	if (!strcmp(ls->name, S_NAME_GL_IDM)) {
		/*
		 * Prepare the pv list for global lock, if the drive contains
		 * "propeller" partition, then this drive will be considered
		 * as a member of pv list.
		 */
		rv = lm_idm_get_gl_lock_pv_list();
		if (rv < 0) {
			log_error("S %s add_lockspace_idm fail to get pv list for glb lock",
				  ls->name);
			return -EIO;
		} else {
			log_error("S %s add_lockspace_idm get pv list for glb lock",
				  ls->name);
		}
	}

	/*
	 * Construct the execution path for command "lvmlockctl" by using the
	 * path to the lvm binary and appending "lockctl".
	 */
	memset(killpath, 0, sizeof(killpath));
	snprintf(killpath, IDM_FAILURE_PATH_LEN, "%slockctl", LVM_PATH);

	/* Pass the argument "--kill vg_name" for killpath */
	memset(killargs, 0, sizeof(killargs));
	snprintf(killargs, IDM_FAILURE_ARGS_LEN, "--kill %s", ls->vg_name);

	/* Connect with IDM lock manager per every lockspace. */
	rv = ilm_connect(&lmi->sock);
	if (rv < 0) {
		log_error("S %s add_lockspace_idm fail to connect the lock manager %d",
			  ls->name, lmi->sock);
		lmi->sock = 0;
		rv = -EMANAGER;
		goto fail;
	}

	rv = ilm_set_killpath(lmi->sock, killpath, killargs);
	if (rv < 0) {
		log_error("S %s add_lockspace_idm fail to set kill path %d",
			  ls->name, rv);
		rv = -EMANAGER;
		goto fail;
	}

	log_debug("S %s add_lockspace_idm kill path is: \"%s %s\"",
		  ls->name, killpath, killargs);

	log_debug("S %s add_lockspace_idm done", ls->name);
	return 0;

fail:
	if (lmi && lmi->sock)
		close(lmi->sock);

	free(lmi);

	return rv;
}

int lm_rem_lockspace_idm(struct lockspace *ls, int free_vg)
{
	struct lm_idm *lmi = (struct lm_idm *)ls->lm_data;
	int i, rv = 0;

	if (daemon_test)
		goto out;

	rv = ilm_disconnect(lmi->sock);
	if (rv < 0)
		log_error("S %s rem_lockspace_idm error %d", ls->name, rv);

	/* Release pv list for global lock */
	if (!strcmp(ls->name, "lvm_global")) {
		for (i = 0; i < glb_lock_op.drive_num; i++) {
			if (glb_lock_op.drives[i]) {
				free(glb_lock_op.drives[i]);
				glb_lock_op.drives[i] = NULL;
			}
		}
	}

out:
	free(lmi);
	ls->lm_data = NULL;
	return rv;
}

static int lm_add_resource_idm(struct lockspace *ls, struct resource *r)
{
	struct rd_idm *rdi = (struct rd_idm *)r->lm_data;

	if (r->type == LD_RT_GL || r->type == LD_RT_VG) {
		rdi->vb = zalloc(sizeof(struct val_blk));
		if (!rdi->vb)
			return -ENOMEM;
	}

	return 0;
}

int lm_rem_resource_idm(struct lockspace *ls, struct resource *r)
{
	struct rd_idm *rdi = (struct rd_idm *)r->lm_data;

	free(rdi->vb);

	memset(rdi, 0, sizeof(struct rd_idm));
	r->lm_init = 0;
	return 0;
}

static int to_idm_mode(int ld_mode)
{
	switch (ld_mode) {
	case LD_LK_EX:
		return IDM_MODE_EXCLUSIVE;
	case LD_LK_SH:
		return IDM_MODE_SHAREABLE;
	default:
		break;
	};

	return -1;
}

int lm_lock_idm(struct lockspace *ls, struct resource *r, int ld_mode,
		struct val_blk *vb_out, char *lv_uuid, struct pvs *pvs,
		int adopt)
{
	struct lm_idm *lmi = (struct lm_idm *)ls->lm_data;
	struct rd_idm *rdi = (struct rd_idm *)r->lm_data;
	char **drive_path = NULL;
	uint64_t timestamp;
	int reset_vb = 0;
	int rv, i;

	if (!r->lm_init) {
		rv = lm_add_resource_idm(ls, r);
		if (rv < 0)
			return rv;
		r->lm_init = 1;
	}

	rdi->op.mode = to_idm_mode(ld_mode);
	if (rv < 0) {
		log_error("lock_idm invalid mode %d", ld_mode);
		return -EINVAL;
	}

	log_debug("S %s R %s lock_idm", ls->name, r->name);

	if (daemon_test) {
		if (rdi->vb) {
			vb_out->version = le16_to_cpu(rdi->vb->version);
			vb_out->flags = le16_to_cpu(rdi->vb->flags);
			vb_out->r_version = le32_to_cpu(rdi->vb->r_version);
		}
		return 0;
	}

	rdi->op.timeout = IDM_TIMEOUT;

	/*
	 * Generate the UUID string, for RT_VG, it only needs to generate
	 * UUID string for VG level, for RT_LV, it needs to generate
	 * UUID strings for both VG and LV levels.  At the end, these IDs
	 * are used as identifier for IDM in drive firmware.
	 */
	if (r->type == LD_RT_VG || r->type == LD_RT_LV)
		log_debug("S %s R %s VG uuid %s", ls->name, r->name, ls->vg_uuid);
	if (r->type == LD_RT_LV)
		log_debug("S %s R %s LV uuid %s", ls->name, r->name, lv_uuid);

	memset(&rdi->id, 0x0, sizeof(struct idm_lock_id));
	if (r->type == LD_RT_VG) {
		uuid_read_format(rdi->id.vg_uuid, ls->vg_uuid);
	} else if (r->type == LD_RT_LV) {
		uuid_read_format(rdi->id.vg_uuid, ls->vg_uuid);
		uuid_read_format(rdi->id.lv_uuid, lv_uuid);
	}

	/*
	 * Establish the drive path list for lock, since different lock type
	 * has different drive list; the GL lock uses the global pv list,
	 * the VG lock uses the pv list spanned for the whole volume group,
	 * the LV lock uses the pv list for the logical volume.
	 */
	switch (r->type) {
	case LD_RT_GL:
		drive_path = glb_lock_op.drives;
		rdi->op.drive_num = glb_lock_op.drive_num;
		break;
	case LD_RT_VG:
		drive_path = (char **)ls->pvs.path;
		rdi->op.drive_num = ls->pvs.num;
		break;
	case LD_RT_LV:
		drive_path = (char **)pvs->path;
		rdi->op.drive_num = pvs->num;
		break;
	default:
		break;
	}

	if (!drive_path) {
		log_error("S %s R %s cannot find the valid drive path array",
			  ls->name, r->name);
		return -EINVAL;
	}

	if (rdi->op.drive_num >= ILM_DRIVE_MAX_NUM) {
		log_error("S %s R %s exceeds limitation for drive path array",
			  ls->name, r->name);
		return -EINVAL;
	}

	for (i = 0; i < rdi->op.drive_num; i++)
		rdi->op.drives[i] = drive_path[i];

	log_debug("S %s R %s mode %d drive_num %d timeout %d",
		  ls->name, r->name, rdi->op.mode,
		  rdi->op.drive_num, rdi->op.timeout);

	for (i = 0; i < rdi->op.drive_num; i++)
		log_debug("S %s R %s drive path[%d] %s",
			  ls->name, r->name, i, rdi->op.drives[i]);

	rv = ilm_lock(lmi->sock, &rdi->id, &rdi->op);
	if (rv < 0) {
		log_debug("S %s R %s lock_idm acquire mode %d rv %d",
			  ls->name, r->name, ld_mode, rv);
		return -ELOCKIO;
	}

	if (rdi->vb) {
		rv = ilm_read_lvb(lmi->sock, &rdi->id, (char *)&timestamp,
				  sizeof(uint64_t));

		/*
		 * If fail to read value block, which might be caused by drive
		 * failure, notify up layer to invalidate metadata.
		 */
		if (rv < 0) {
			log_error("S %s R %s lock_idm get_lvb error %d",
				  ls->name, r->name, rv);
			reset_vb = 1;

			/* Reset timestamp */
			rdi->vb_timestamp = 0;

		/*
		 * If the cached timestamp mismatches with the stored value
		 * in the IDM, this means another host has updated timestamp
		 * for the new VB.  Let's reset VB and notify up layer to
		 * invalidate metadata.
		 */
		} else if (rdi->vb_timestamp != timestamp) {
			log_debug("S %s R %s lock_idm get lvb timestamp %lu:%lu",
				  ls->name, r->name, rdi->vb_timestamp,
				  timestamp);

			rdi->vb_timestamp = timestamp;
			reset_vb = 1;
		}

		if (reset_vb == 1) {
			memset(rdi->vb, 0, sizeof(struct val_blk));
			memset(vb_out, 0, sizeof(struct val_blk));

			/*
			 * The lock is still acquired, but the vb values has
			 * been invalidated.
			 */
			rv = 0;
			goto out;
		}

		/* Otherwise, copy the cached VB to up layer */
		memcpy(vb_out, rdi->vb, sizeof(struct val_blk));
	}

out:
	return rv;
}

int lm_convert_idm(struct lockspace *ls, struct resource *r,
		   int ld_mode, uint32_t r_version)
{
	struct lm_idm *lmi = (struct lm_idm *)ls->lm_data;
	struct rd_idm *rdi = (struct rd_idm *)r->lm_data;
	int mode, rv;

	if (rdi->vb && r_version && (r->mode == LD_LK_EX)) {
		if (!rdi->vb->version) {
			/* first time vb has been written */
			rdi->vb->version = VAL_BLK_VERSION;
		}
		rdi->vb->r_version = r_version;

		log_debug("S %s R %s convert_idm set r_version %u",
			  ls->name, r->name, r_version);

		lm_idm_update_vb_timestamp(&rdi->vb_timestamp);
		log_debug("S %s R %s convert_idm vb %x %x %u timestamp %lu",
			  ls->name, r->name, rdi->vb->version, rdi->vb->flags,
			  rdi->vb->r_version, rdi->vb_timestamp);
	}

	mode = to_idm_mode(ld_mode);
	if (mode < 0) {
		log_error("S %s R %s convert_idm invalid mode %d",
			  ls->name, r->name, ld_mode);
		return -EINVAL;
	}

	log_debug("S %s R %s convert_idm", ls->name, r->name);

	if (daemon_test)
		return 0;

	if (rdi->vb && r_version && (r->mode == LD_LK_EX)) {
		rv = ilm_write_lvb(lmi->sock, &rdi->id,
				   (char *)rdi->vb_timestamp, sizeof(uint64_t));
		if (rv < 0) {
			log_error("S %s R %s convert_idm write lvb error %d",
				  ls->name, r->name, rv);
			return -ELMERR;
		}
	}

	rv = ilm_convert(lmi->sock, &rdi->id, mode);
	if (rv < 0)
		log_error("S %s R %s convert_idm convert error %d",
			  ls->name, r->name, rv);

	return rv;
}

int lm_unlock_idm(struct lockspace *ls, struct resource *r,
		  uint32_t r_version, uint32_t lmu_flags)
{
	struct lm_idm *lmi = (struct lm_idm *)ls->lm_data;
	struct rd_idm *rdi = (struct rd_idm *)r->lm_data;
	int rv;

	if (rdi->vb && r_version && (r->mode == LD_LK_EX)) {
		if (!rdi->vb->version) {
			/* first time vb has been written */
			rdi->vb->version = VAL_BLK_VERSION;
		}
		if (r_version)
			rdi->vb->r_version = r_version;

		lm_idm_update_vb_timestamp(&rdi->vb_timestamp);
		log_debug("S %s R %s unlock_idm vb %x %x %u timestamp %lu",
			  ls->name, r->name, rdi->vb->version, rdi->vb->flags,
			  rdi->vb->r_version, rdi->vb_timestamp);
	}

	log_debug("S %s R %s unlock_idm", ls->name, r->name);

	if (daemon_test)
		return 0;

	if (rdi->vb && r_version && (r->mode == LD_LK_EX)) {
		rv = ilm_write_lvb(lmi->sock, &rdi->id,
				   (char *)&rdi->vb_timestamp, sizeof(uint64_t));
		if (rv < 0) {
			log_error("S %s R %s unlock_idm set_lvb error %d",
				  ls->name, r->name, rv);
			return -ELMERR;
		}
	}

	rv = ilm_unlock(lmi->sock, &rdi->id);
	if (rv < 0)
		log_error("S %s R %s unlock_idm error %d", ls->name, r->name, rv);

	return rv;
}

int lm_hosts_idm(struct lockspace *ls, int notify)
{
	struct resource *r;
	struct lm_idm *lmi = (struct lm_idm *)ls->lm_data;
	struct rd_idm *rdi;
	int count, self, found_others = 0;
	int rv;

	list_for_each_entry(r, &ls->resources, list) {
		if (!r->lm_init)
			continue;

		rdi = (struct rd_idm *)r->lm_data;

		rv = ilm_get_host_count(lmi->sock, &rdi->id, &rdi->op,
					&count, &self);
		if (rv < 0) {
			log_error("S %s lm_hosts_idm error %d", ls->name, rv);
			return rv;
		}

		/* Fixup: need to reduce self count */
		if (count > found_others)
			found_others = count;
	}

	return found_others;
}

int lm_get_lockspaces_idm(struct list_head *ls_rejoin)
{
	/* TODO: Need to add support for adoption. */
	return -1;
}

int lm_is_running_idm(void)
{
	int sock, rv;

	if (daemon_test)
		return gl_use_idm;

	rv = ilm_connect(&sock);
	if (rv < 0) {
		log_error("Fail to connect seagate IDM lock manager %d", rv);
		return 0;
	}

	ilm_disconnect(sock);
	return 1;
}
