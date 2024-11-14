/*
 * Copyright (C) 2024 Red Hat, Inc. All rights reserved.
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
#include "lib/device/persist.h"
#include "lib/mm/xlate.h"

#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <assert.h>

#define NVME_PR_BUF_SIZE 8192 /* space for 127 keys */

#ifdef NVME_SUPPORT
#include <libnvme.h>

static int iszero(unsigned char *d, size_t n)
{
	size_t i;

	for (i = 0; i < n; ++i) {
		if (d[i])
			return 0;
	}
	return 1;
}

static void _save_uuid(struct device *dev, unsigned char *uuid)
{
	char idname[DEV_WWID_SIZE] = {0};
	int max, pos, num, i;

	max = sizeof(idname);
	pos = 0;

	num = snprintf(idname + pos, max - pos, "uuid.");
	if (num >= max - pos)
		goto bad;
	pos += num;

	for (i = 0; i < NVME_UUID_LEN; ++i) {
		num = snprintf(idname + pos, max - pos, "%02x", uuid[i]);
		if (num >= max - pos)
			goto bad;
		pos += num;

		if (i == 3 || i == 5 || i == 7 || i == 9) {
			num = snprintf(idname + pos, max - pos, "-");
			if (num >= max - pos)
				goto bad;
			pos += num;
		}
	}

	idname[DEV_WWID_SIZE-1] = '\0';

	dev_add_nvme_wwid(idname, 3, &dev->wwids);

	return;
bad:
	log_debug("dev_read_nvme_wwids ignore invalid uuid %s for %s", uuid, dev_name(dev));
}

static void _save_nguid(struct device *dev, unsigned char *nguid)
{
	char idname[DEV_WWID_SIZE] = {0};
	int max, pos, num, i;

	max = sizeof(idname);
	pos = 0;

	num = snprintf(idname + pos, max - pos, "eui.");
	if (num >= max - pos)
		goto bad;
	pos += num;

	for (i = 0; i < 16; ++i) {
		num = snprintf(idname + pos, max - pos, "%02x", nguid[i]);
		if (num >= max - pos)
			goto bad;
		pos += num;
	}

	idname[DEV_WWID_SIZE-1] = '\0';

	dev_add_nvme_wwid(idname, 2, &dev->wwids);

	return;
bad:
	log_debug("dev_read_nvme_wwids ignore invalid nguid %s for %s", nguid, dev_name(dev));
}

static void _save_eui64(struct device *dev, unsigned char *eui64)
{
	char idname[DEV_WWID_SIZE] = {0};
	int max, pos, num, i;

	max = sizeof(idname);
	pos = 0;

	num = snprintf(idname + pos, max - pos, "eui.");
	if (num >= max - pos)
		goto bad;
	pos += num;

	for (i = 0; i < 8; ++i) {
		num = snprintf(idname + pos, max - pos, "%02x", eui64[i]);
		if (num >= max - pos)
			goto bad;
		pos += num;
	}

	idname[DEV_WWID_SIZE-1] = '\0';

	dev_add_nvme_wwid(idname, 1, &dev->wwids);

	return;
bad:
	log_debug("dev_read_nvme_wwids ignore invalid eui64 %s for %s", eui64, dev_name(dev));
}

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
static void *_nvme_alloc(size_t len)
{
	size_t _len = ROUND_UP(len, 0x1000);
	void *p;

	if (posix_memalign((void *)&p, getpagesize(), _len))
		return NULL;

	memset(p, 0, _len);
	return p;
}

void dev_read_nvme_wwids(struct device *dev)
{
	const char *devpath;
	unsigned char *data = NULL;
	struct nvme_id_ns *ns = NULL;
	struct nvme_id_ctrl *ctrl_id = NULL;
	unsigned char nguid[16] = {0};
	unsigned char eui64[8] = {0};
	unsigned char uuid[NVME_UUID_LEN] = {0};
	uint32_t nsid = 0;
	int fd, i, len;

	dev->flags |= DEV_ADDED_NVME_WWIDS;

	/* shouldn't happen */
	if (dm_list_empty(&dev->aliases))
		return;

	devpath = dev_name(dev);

	if ((fd = open(devpath, O_RDONLY)) < 0) {
		log_debug("dev_read_nvme_wwids cannot open %s", devpath);
		return;
	}

	if (nvme_get_nsid(fd, &nsid)) {
		log_print("dev_read_nvme_wwids nvme_get_nsid error %d %s", errno, devpath);
		goto out;
	}

	if (!(ns = _nvme_alloc(sizeof(*ns))))
		goto_out;

	if (nvme_identify_ns(fd, nsid, ns)) {
		log_debug("dev_read_nvme_wwids nvme_identify_ns error %d %s", errno, devpath);
		goto out;
	}

	memcpy(nguid, ns->nguid, 16);
	memcpy(eui64, ns->eui64, 8);

	if (!iszero(nguid, 16))
		_save_nguid(dev, nguid);
	if (!iszero(eui64, 8))
		_save_eui64(dev, eui64);

	if (!(ctrl_id = _nvme_alloc(sizeof(struct nvme_id_ctrl))))
		goto_out;

	/* Avoid using nvme_identify_ns_descs before ver 1.3. */
	if (!nvme_identify_ctrl(fd, ctrl_id)) {
		if (le32_to_cpu(ctrl_id->ver) < 0x10300)
			goto_out;
	}

	if (!(data = _nvme_alloc(NVME_IDENTIFY_DATA_SIZE)))
		goto_out;

	if (nvme_identify_ns_descs(fd, nsid, (struct nvme_ns_id_desc *)data)) {
		log_debug("dev_read_nvme_wwids nvme_identify_ns_descs error %d %s", errno, devpath);
		goto out;
	}

	for (i = 0; i < NVME_IDENTIFY_DATA_SIZE; i += len) {
		struct nvme_ns_id_desc *cur = (struct nvme_ns_id_desc *)(data + i);

		if (cur->nidl == 0)
			break;

		memset(eui64, 0, sizeof(eui64));
		memset(nguid, 0, sizeof(nguid));
		memset(uuid, 0, sizeof(uuid));

		switch (cur->nidt) {
		case NVME_NIDT_EUI64:
			memcpy(eui64, data + i + sizeof(*cur), sizeof(eui64));
			len = sizeof(eui64);
			break;
		case NVME_NIDT_NGUID:
			memcpy(nguid, data + i + sizeof(*cur), sizeof(nguid));
			len = sizeof(nguid);
			break;
		case NVME_NIDT_UUID:
			memcpy(uuid, data + i + sizeof(*cur), NVME_UUID_LEN);
			len = sizeof(uuid);
			break;
		case NVME_NIDT_CSI:
			len = 1;
			break;
		default:
			len = cur->nidl;
			break;
		}

		len += sizeof(*cur);

		if (!iszero(uuid, NVME_UUID_LEN))
			_save_uuid(dev, uuid);
		else if (!iszero(nguid, 16))
			_save_nguid(dev, nguid);
		else if (!iszero(eui64, 8))
			_save_eui64(dev, eui64);
	}
out:
	free(ctrl_id);
	free(ns);
	free(data);

	if (close(fd))
		log_sys_debug("close", devpath);
}

static int prtype_from_nvme(uint8_t nvme_type)
{
	switch (nvme_type) {
	case 0:
		return 0;
	case 1:
		return PR_TYPE_WE;
	case 2:
		return PR_TYPE_EA;
	case 3:
		return PR_TYPE_WERO;
	case 4:
		return PR_TYPE_EARO;
	case 5:
		return PR_TYPE_WEAR;
	case 6:
		return PR_TYPE_EAAR;
	default:
		return -1;
	};
}

int dev_read_reservation_nvme(struct cmd_context *cmd, struct device *dev, uint64_t *holder_ret, int *prtype_ret)
{
	const char *devname;
	struct nvme_resv_report_args args = { 0 };
	struct nvme_resv_status *status = NULL;
	uint64_t rkey;
	uint32_t nsid;
	int status_size = NVME_PR_BUF_SIZE;
	int status_entries;
	int regctl;
	int err;
	int fd;
	int i;
	int ret = 0;

	devname = dev_name(dev);

	if ((fd = open(devname, O_RDONLY)) < 0) {
		log_error("dev_read_reservation %s open error %d", devname, errno);
		return 0;
	}

	if (nvme_get_nsid(fd, &nsid)) {
		log_error("dev_read_reservation %s nvme_get_nsid error %d", devname, errno);
		goto out;
	}

	if (!(status = malloc(status_size)))
		goto out;

	args.args_size = sizeof(args);
	args.fd = fd;
	args.nsid = nsid;
	args.eds = 1;
	args.len = status_size;
	args.report = status;
	args.timeout = 0;
	args.result = NULL;

	if ((err = nvme_resv_report(&args))) {
		log_error("dev_read_reservation %s error %d", devname, err);
		goto out;
	}

	*prtype_ret = prtype_from_nvme(status->rtype);
	ret = 1;

	if (!holder_ret)
		goto out;

	*holder_ret = 0;

	/* all are holders for these types so don't bother checking */
	if (*prtype_ret == PR_TYPE_WEAR || *prtype_ret == PR_TYPE_EAAR)
		goto out;

	regctl = status->regctl[0] | (status->regctl[1] << 8);
	status_entries = (status_size - 64) / 64;
	if (status_entries < regctl) {
		log_warn("dev_read_reservation %s too many entries %d limit %d", devname, regctl, status_entries);
		regctl = status_entries;
	}

	for (i = 0; i < regctl; i++) {
		if (status->regctl_eds[i].rcsts) {
			rkey = le64_to_cpu(status->regctl_eds[i].rkey);
			*holder_ret = rkey;
			break;
		}
	}

out:
	free(status);
	close(fd);
	return ret;
}

int dev_find_key_nvme(struct cmd_context *cmd, struct device *dev, int may_fail,
		      uint64_t find_key, int *found_key,
		      int find_host_id, uint64_t *found_host_id_key,
		      int find_all, int *found_count, uint64_t **found_all)
{
	const char *devname;
	struct nvme_resv_report_args args = { 0 };
	struct nvme_resv_status *status = NULL;
	uint64_t *all_keys;
	uint64_t key;
	uint32_t nsid;
	int status_size = NVME_PR_BUF_SIZE;
	int status_entries;
	int regctl;
	int num_keys;
	int err;
	int fd;
	int i;
	int ret = 0;

	devname = dev_name(dev);

	if ((fd = open(devname, O_RDONLY)) < 0) {
		log_error("dev_read_reservation %s open error %d", devname, errno);
		return 0;
	}

	if (nvme_get_nsid(fd, &nsid)) {
		if (may_fail)
			log_debug("dev_read_reservation %s nvme_get_nsid error %d", devname, errno);
		else
			log_error("dev_read_reservation %s nvme_get_nsid error %d", devname, errno);
		goto out;
	}

	if (!(status = malloc(status_size)))
		goto out;

	args.args_size = sizeof(args);
	args.fd = fd;
	args.nsid = nsid;
	args.eds = 1;
	args.len = status_size;
	args.report = status;
	args.timeout = 0;
	args.result = NULL;

	if ((err = nvme_resv_report(&args))) {
		if (may_fail)
			log_debug("dev_read_reservation %s error %d", devname, err);
		else
			log_error("dev_read_reservation %s error %d", devname, err);
		goto out;
	}

	regctl = status->regctl[0] | (status->regctl[1] << 8);
	status_entries = (status_size - 64) / 64;
	if (status_entries < regctl) {
		log_warn("dev_read_reservation %s too many entries %d limit %d", devname, regctl, status_entries);
		regctl = status_entries;
	}
	num_keys = regctl;

	log_debug("dev_find_key %s num %d", devname, num_keys);

	/* caller wants just a count of all keys */
	if (find_all && found_count && !found_all) {
		*found_count = num_keys;
		ret = 1;
		goto out;
	}

	/* caller wants a count and array of all keys */
	if (find_all && found_count && found_all) {
		*found_count = num_keys;
		*found_all = NULL;

		if (!num_keys) {
			ret = 1;
			goto out;
		}
		if (!(all_keys = dm_pool_zalloc(cmd->mem, num_keys * sizeof(uint64_t)))) {
			ret = 0;
			goto out;
		}
		*found_all = all_keys;
	}

	if (!num_keys) {
		ret = 1;
		goto out;
	}

	for (i = 0; i < num_keys; i++) {
		key = le64_to_cpu(status->regctl_eds[i].rkey);

		log_debug("dev_find_key %s 0x%llx", devname, (unsigned long long)key);

		if (find_all && found_count && found_all)
			all_keys[i] = key;

		if (find_key && (find_key == key)) {
			if (found_key)
				*found_key = 1;
			if (!find_all)
				break;
		}

		if (find_host_id && (find_host_id == (key & 0xFFFF))) {
			if (found_host_id_key)
				*found_host_id_key = key;
			if (!find_all)
				break;
		}
	}

	ret = 1;

out:
	free(status);
	close(fd);
	return ret;
}

#else

void dev_read_nvme_wwids(struct device *dev)
{
}

int dev_read_reservation_nvme(struct cmd_context *cmd, struct device *dev, uint64_t *holder_ret, int *prtype_ret)
{
	return 0;
}


int dev_find_key_nvme(struct cmd_context *cmd, struct device *dev,
		      uint64_t find_key, int *found_key,
		      int find_host_id, uint64_t *found_host_id_key,
		      int find_all, int *found_count, uint64_t **found_all)
{
	return 0;
}

#endif
