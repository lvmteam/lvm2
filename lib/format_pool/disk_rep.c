/*
 * Copyright (C) 1997-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2006 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "label.h"
#include "metadata.h"
#include "lvmcache.h"
#include "xlate.h"
#include "disk_rep.h"
#include "toolcontext.h"

#include <assert.h>

/* FIXME: memcpy might not be portable */
#define CPIN_8(x, y, z) {memcpy((x), (y), (z));}
#define CPOUT_8(x, y, z) {memcpy((y), (x), (z));}
#define CPIN_16(x, y) {(x) = xlate16_be((y));}
#define CPOUT_16(x, y) {(y) = xlate16_be((x));}
#define CPIN_32(x, y) {(x) = xlate32_be((y));}
#define CPOUT_32(x, y) {(y) = xlate32_be((x));}
#define CPIN_64(x, y) {(x) = xlate64_be((y));}
#define CPOUT_64(x, y) {(y) = xlate64_be((x));}

static int __read_pool_disk(const struct format_type *fmt, struct device *dev,
			    struct dm_pool *mem __attribute__((unused)), struct pool_list *pl,
			    const char *vg_name __attribute__((unused)))
{
	char buf[512] __attribute__((aligned(8)));

	/* FIXME: Need to check the cache here first */
	if (!dev_read(dev, UINT64_C(0), 512, buf)) {
		log_very_verbose("Failed to read PV data from %s",
				 dev_name(dev));
		return 0;
	}

	if (!read_pool_label(pl, fmt->labeller, dev, buf, NULL))
		return_0;

	return 1;
}

static void _add_pl_to_list(struct cmd_context *cmd, struct dm_list *head, struct pool_list *data)
{
	struct pool_list *pl;

	dm_list_iterate_items(pl, head) {
		if (id_equal(&data->pv_uuid, &pl->pv_uuid)) {
			char uuid[ID_LEN + 7] __attribute__((aligned(8)));

			id_write_format(&pl->pv_uuid, uuid, ID_LEN + 7);

			if (!dev_subsystem_part_major(cmd->dev_types, data->dev)) {
				log_very_verbose("Ignoring duplicate PV %s on "
						 "%s", uuid,
						 dev_name(data->dev));
				return;
			}
			log_very_verbose("Duplicate PV %s - using %s %s",
					 uuid, dev_subsystem_name(cmd->dev_types, data->dev),
					 dev_name(data->dev));
			dm_list_del(&pl->list);
			break;
		}
	}
	dm_list_add(head, &data->list);
}

int read_pool_label(struct pool_list *pl, struct labeller *l,
		    struct device *dev, char *buf, struct label **label)
{
	struct lvmcache_info *info;
	struct id pvid;
	struct id vgid;
	char uuid[ID_LEN + 7] __attribute__((aligned(8)));
	struct pool_disk *pd = &pl->pd;

	pool_label_in(pd, buf);

	get_pool_pv_uuid(&pvid, pd);
	id_write_format(&pvid, uuid, ID_LEN + 7);
	log_debug_metadata("Calculated uuid %s for %s", uuid, dev_name(dev));

	get_pool_vg_uuid(&vgid, pd);
	id_write_format(&vgid, uuid, ID_LEN + 7);
	log_debug_metadata("Calculated uuid %s for %s", uuid, pd->pl_pool_name);

	if (!(info = lvmcache_add(l, (char *) &pvid, dev, pd->pl_pool_name,
				  (char *) &vgid, 0)))
		return_0;
	if (label)
		*label = lvmcache_get_label(info);

	lvmcache_set_device_size(info, ((uint64_t)xlate32_be(pd->pl_blocks)) << SECTOR_SHIFT);
	lvmcache_del_mdas(info);
	lvmcache_make_valid(info);

	pl->dev = dev;
	pl->pv = NULL;
	memcpy(&pl->pv_uuid, &pvid, sizeof(pvid));

	return 1;
}

/**
 * pool_label_out - copies a pool_label_t into a char buffer
 * @pl: ptr to a pool_label_t struct
 * @buf: ptr to raw space where label info will be copied
 *
 * This function is important because it takes care of all of
 * the endian issues when copying to disk.  This way, when
 * machines of different architectures are used, they will
 * be able to interpret ondisk labels correctly.  Always use
 * this function before writing to disk.
 */
void pool_label_out(struct pool_disk *pl, void *buf)
{
	struct pool_disk *bufpl = (struct pool_disk *) buf;

	CPOUT_64(pl->pl_magic, bufpl->pl_magic);
	CPOUT_64(pl->pl_pool_id, bufpl->pl_pool_id);
	CPOUT_8(pl->pl_pool_name, bufpl->pl_pool_name, POOL_NAME_SIZE);
	CPOUT_32(pl->pl_version, bufpl->pl_version);
	CPOUT_32(pl->pl_subpools, bufpl->pl_subpools);
	CPOUT_32(pl->pl_sp_id, bufpl->pl_sp_id);
	CPOUT_32(pl->pl_sp_devs, bufpl->pl_sp_devs);
	CPOUT_32(pl->pl_sp_devid, bufpl->pl_sp_devid);
	CPOUT_32(pl->pl_sp_type, bufpl->pl_sp_type);
	CPOUT_64(pl->pl_blocks, bufpl->pl_blocks);
	CPOUT_32(pl->pl_striping, bufpl->pl_striping);
	CPOUT_32(pl->pl_sp_dmepdevs, bufpl->pl_sp_dmepdevs);
	CPOUT_32(pl->pl_sp_dmepid, bufpl->pl_sp_dmepid);
	CPOUT_32(pl->pl_sp_weight, bufpl->pl_sp_weight);
	CPOUT_32(pl->pl_minor, bufpl->pl_minor);
	CPOUT_32(pl->pl_padding, bufpl->pl_padding);
	CPOUT_8(pl->pl_reserve, bufpl->pl_reserve, 184);
}

/**
 * pool_label_in - copies a char buffer into a pool_label_t
 * @pl: ptr to a pool_label_t struct
 * @buf: ptr to raw space where label info is copied from
 *
 * This function is important because it takes care of all of
 * the endian issues when information from disk is about to be
 * used.  This way, when machines of different architectures
 * are used, they will be able to interpret ondisk labels
 * correctly.  Always use this function before using labels that
 * were read from disk.
 */
void pool_label_in(struct pool_disk *pl, void *buf)
{
	struct pool_disk *bufpl = (struct pool_disk *) buf;

	CPIN_64(pl->pl_magic, bufpl->pl_magic);
	CPIN_64(pl->pl_pool_id, bufpl->pl_pool_id);
	CPIN_8(pl->pl_pool_name, bufpl->pl_pool_name, POOL_NAME_SIZE);
	CPIN_32(pl->pl_version, bufpl->pl_version);
	CPIN_32(pl->pl_subpools, bufpl->pl_subpools);
	CPIN_32(pl->pl_sp_id, bufpl->pl_sp_id);
	CPIN_32(pl->pl_sp_devs, bufpl->pl_sp_devs);
	CPIN_32(pl->pl_sp_devid, bufpl->pl_sp_devid);
	CPIN_32(pl->pl_sp_type, bufpl->pl_sp_type);
	CPIN_64(pl->pl_blocks, bufpl->pl_blocks);
	CPIN_32(pl->pl_striping, bufpl->pl_striping);
	CPIN_32(pl->pl_sp_dmepdevs, bufpl->pl_sp_dmepdevs);
	CPIN_32(pl->pl_sp_dmepid, bufpl->pl_sp_dmepid);
	CPIN_32(pl->pl_sp_weight, bufpl->pl_sp_weight);
	CPIN_32(pl->pl_minor, bufpl->pl_minor);
	CPIN_32(pl->pl_padding, bufpl->pl_padding);
	CPIN_8(pl->pl_reserve, bufpl->pl_reserve, 184);
}

static char _calc_char(unsigned int id)
{
	/*
	 * [0-9A-Za-z!#] - 64 printable chars (6-bits)
	 */

	if (id < 10)
		return id + 48;
	if (id < 36)
		return (id - 10) + 65;
	if (id < 62)
		return (id - 36) + 97;
	if (id == 62)
		return '!';
	if (id == 63)
		return '#';

	return '%';
}

void get_pool_uuid(char *uuid, uint64_t poolid, uint32_t spid, uint32_t devid)
{
	int i;
	unsigned shifter = 0x003F;

	assert(ID_LEN == 32);
	memset(uuid, 0, ID_LEN);
	strcat(uuid, "POOL0000000000");

	/* We grab the entire 64 bits (+2 that get shifted in) */
	for (i = 13; i < 24; i++) {
		uuid[i] = _calc_char(((unsigned) poolid) & shifter);
		poolid = poolid >> 6;
	}

	/* We grab the entire 32 bits (+4 that get shifted in) */
	for (i = 24; i < 30; i++) {
		uuid[i] = _calc_char((unsigned) (spid & shifter));
		spid = spid >> 6;
	}

	/*
	 * Since we can only have 128 devices, we only worry about the
	 * last 12 bits
	 */
	for (i = 30; i < 32; i++) {
		uuid[i] = _calc_char((unsigned) (devid & shifter));
		devid = devid >> 6;
	}

}

struct _read_pool_pv_baton {
	const struct format_type *fmt;
	struct dm_pool *mem, *tmpmem;
	struct pool_list *pl;
	struct dm_list *head;
	const char *vgname;
	uint32_t *sp_devs;
	uint32_t sp_count;
	int failed;
	int empty;
};

static int _read_pool_pv(struct lvmcache_info *info, void *baton)
{
	struct _read_pool_pv_baton *b = baton;

	b->empty = 0;

	if (lvmcache_device(info) &&
	    !(b->pl = read_pool_disk(b->fmt, lvmcache_device(info), b->mem, b->vgname)))
		return 0;

	/*
	 * We need to keep track of the total expected number
	 * of devices per subpool
	 */
	if (!b->sp_count) {
		/* FIXME pl left uninitialised if !info->dev */
		if (!b->pl) {
			log_error(INTERNAL_ERROR "device is missing");
			dm_pool_destroy(b->tmpmem);
			b->failed = 1;
			return 0;
		}
		b->sp_count = b->pl->pd.pl_subpools;
		if (!(b->sp_devs =
		      dm_pool_zalloc(b->tmpmem,
				     sizeof(uint32_t) * b->sp_count))) {
			log_error("Unable to allocate %d 32-bit uints",
				  b->sp_count);
			dm_pool_destroy(b->tmpmem);
			b->failed = 1;
			return 0;
		}
	}

	/*
	 * watch out for a pool label with a different subpool
	 * count than the original - give up if it does
	 */
	if (b->sp_count != b->pl->pd.pl_subpools)
		return 0;

	_add_pl_to_list(lvmcache_fmt(info)->cmd, b->head, b->pl);

	if (b->sp_count > b->pl->pd.pl_sp_id && b->sp_devs[b->pl->pd.pl_sp_id] == 0)
		b->sp_devs[b->pl->pd.pl_sp_id] = b->pl->pd.pl_sp_devs;

	return 1;
}

static int _read_vg_pds(struct _read_pool_pv_baton *b,
			struct lvmcache_vginfo *vginfo,
			uint32_t *devcount)
{
	uint32_t i;

	b->sp_count = 0;
	b->sp_devs = NULL;
	b->failed = 0;
	b->pl = NULL;

	/* FIXME: maybe should return a different error in memory
	 * allocation failure */
	if (!(b->tmpmem = dm_pool_create("pool read_vg", 512)))
		return_0;

	lvmcache_foreach_pv(vginfo, _read_pool_pv, b);

	*devcount = 0;
	for (i = 0; i < b->sp_count; i++)
		*devcount += b->sp_devs[i];

	dm_pool_destroy(b->tmpmem);

	if (b->pl && *b->pl->pd.pl_pool_name)
		return 1;

	return 0;

}

int read_pool_pds(const struct format_type *fmt, const char *vg_name,
		  struct dm_pool *mem, struct dm_list *pdhead)
{
	struct lvmcache_vginfo *vginfo;
	uint32_t totaldevs;
	int full_scan = -1;

	struct _read_pool_pv_baton baton;

	baton.vgname = vg_name;
	baton.mem = mem;
	baton.fmt = fmt;
	baton.head = pdhead;
	baton.empty = 1;

	do {
		/*
		 * If the cache scanning doesn't work, this will never work
		 */
		if (vg_name && (vginfo = lvmcache_vginfo_from_vgname(vg_name, NULL)) &&
		    _read_vg_pds(&baton, vginfo, &totaldevs) && !baton.empty)
		{
			/*
			 * If we found all the devices we were expecting, return
			 * success
			 */
			if (dm_list_size(pdhead) == totaldevs)
				return 1;

			/*
			 * accept partial pool if we've done a full rescan of
			 * the cache
			 */
			if (full_scan > 0)
				return 1;
		}

		/* Failed */
		dm_list_init(pdhead);

		full_scan++;
		if (full_scan > 1) {
			log_debug_metadata("No devices for vg %s found in cache",
					   vg_name);
			return 0;
		}
		lvmcache_label_scan(fmt->cmd, full_scan);

	} while (1);

}

struct pool_list *read_pool_disk(const struct format_type *fmt,
				 struct device *dev, struct dm_pool *mem,
				 const char *vg_name)
{
	struct pool_list *pl;

	if (!dev_open_readonly(dev))
		return_NULL;

	if (!(pl = dm_pool_zalloc(mem, sizeof(*pl)))) {
		log_error("Unable to allocate pool list structure");
		return 0;
	}

	if (!__read_pool_disk(fmt, dev, mem, pl, vg_name))
		return_NULL;

	if (!dev_close(dev))
		stack;

	return pl;

}
