/*
 * Copyright (C) 2002 Sistina Software
 *
 * This file is released under the GPL.
 *
 */

/*
 * Changelog
 *
 *   05/02/2002 - First drop [HM]
 */

#include "tools.h"

int disks_found = 0;
int parts_found = 0;
int pv_disks_found = 0;
int pv_parts_found = 0;
int max_len;

static int _get_max_dev_name_len(struct dev_filter *filter)
{
	int len = 0;
	int maxlen = 0;
	struct dev_iter *iter;
	struct device *dev;

	if (!(iter = dev_iter_create(filter))) {
		log_error("dev_iter_create failed");
		return 0;
	}

	/* Do scan */
	for (dev = dev_iter_get(iter); dev; dev = dev_iter_get(iter)) {
		len = strlen(dev_name(dev));
		if (len > maxlen)
			maxlen = len;
	}
	dev_iter_destroy(iter);

	return maxlen;
}

static void _count(struct device *dev, int *disks, int *parts)
{
	int c = dev_name(dev)[strlen(dev_name(dev)) - 1];

	if (!isdigit(c))
		(*disks)++;
	else
		(*parts)++;
}

static void _print(struct cmd_context *cmd, const struct device *dev,
		   uint64_t size, const char *what)
{
	log_print("%-*s [%15s] %s", max_len, dev_name(dev),
		  display_size(cmd, size / 2, SIZE_SHORT), what ? : "");
}

static int _check_device(struct cmd_context *cmd, struct device *dev)
{
	char buffer;
	uint64_t size;

	if (!dev_open(dev)) {
		return 0;
	}
	if (!dev_read(dev, UINT64_C(0), (size_t) 1, &buffer)) {
		dev_close(dev);
		return 0;
	}
	if (!dev_get_size(dev, &size)) {
		log_error("Couldn't get size of \"%s\"", dev_name(dev));
	}
	_print(cmd, dev, size, NULL);
	_count(dev, &disks_found, &parts_found);
	if (!dev_close(dev)) {
		log_error("dev_close on \"%s\" failed", dev_name(dev));
		return 0;
	}
	return 1;
}

int lvmdiskscan(struct cmd_context *cmd, int argc, char **argv)
{
	uint64_t size;
	struct dev_iter *iter;
	struct device *dev;
	struct label *label;

	if (arg_count(cmd, lvmpartition_ARG))
		log_print("WARNING: only considering LVM devices");

	max_len = _get_max_dev_name_len(cmd->filter);

	if (!(iter = dev_iter_create(cmd->filter))) {
		log_error("dev_iter_create failed");
		return ECMD_FAILED;
	}

	/* Do scan */
	for (dev = dev_iter_get(iter); dev; dev = dev_iter_get(iter)) {
		/* Try if it is a PV first */
		if ((label_read(dev, &label))) {
			if (!dev_get_size(dev, &size)) {
				log_error("Couldn't get size of \"%s\"",
					  dev_name(dev));
				continue;
			}
			_print(cmd, dev, size, "LVM physical volume");
			_count(dev, &pv_disks_found, &pv_parts_found);
			continue;
		}
		/* If user just wants PVs we are done */
		if (arg_count(cmd, lvmpartition_ARG))
			continue;

		/* What other device is it? */
		if (!_check_device(cmd, dev))
			continue;
	}
	dev_iter_destroy(iter);

	/* Display totals */
	if (!arg_count(cmd, lvmpartition_ARG)) {
		log_print("%d disk%s",
			  disks_found, disks_found == 1 ? "" : "s");
		log_print("%d partition%s",
			  parts_found, parts_found == 1 ? "" : "s");
	}
	log_print("%d LVM physical volume whole disk%s",
		  pv_disks_found, pv_disks_found == 1 ? "" : "s");
	log_print("%d LVM physical volume%s",
		  pv_parts_found, pv_parts_found == 1 ? "" : "s");

	return ECMD_PROCESSED;
}
