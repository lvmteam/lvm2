/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "log.h"
#include "format1.h"
#include "dbg_malloc.h"
#include "pool.h"

#include <stdio.h>

static void _dump_pv(struct physical_volume *pv, FILE *fp)
{
	fprintf(fp, "physical_volume {\n");
	fprintf(fp, "\tname = '%s'\n", pv->dev->name);
	fprintf(fp, "\tvg_name = '%s'\n", pv->vg_name);
	fprintf(fp, "\tsize = %llu\n", pv->size);
	fprintf(fp, "\tpe_size = %llu\n", pv->pe_size);
	fprintf(fp, "\tpe_start = %llu\n", pv->pe_start);
	fprintf(fp, "\tpe_count = %u\n", pv->pe_count);
	fprintf(fp, "\tpe_allocated = %u\n", pv->pe_allocated);
	fprintf(fp, "}\n\n");
}

static void _dump_lv(struct logical_volume *lv, FILE *fp)
{
	int i;

	fprintf(fp, "logical_volume {\n");
	fprintf(fp, "\tname = '%s'\n", lv->name);
	fprintf(fp, "\tsize = %llu\n", lv->size);
	fprintf(fp, "\tle_count = %u\n", lv->le_count);

	fprintf(fp, "\tmap {\n");
	for (i = 0; i < lv->le_count; i++) {
		struct physical_volume *pv = lv->map[i].pv;

		fprintf(fp, "\t\tpv = '%s', ", pv ? pv->dev->name : "null ???");
		fprintf(fp, "\textent = %u\n", lv->map[i].pe);
	}
	fprintf(fp, "\t}\n}\n\n");
}

static void _dump_vg(struct volume_group *vg, FILE *fp)
{
	struct list_head *tmp;

	fprintf(fp, "volume_group {\n");
	fprintf(fp, "\tname = '%s'\n", vg->name);
	fprintf(fp, "\textent_size = %llu\n", vg->extent_size);
	fprintf(fp, "\textent_count = %d\n", vg->extent_count);
	fprintf(fp, "\tfree_count = %d\n", vg->free_count);
	fprintf(fp, "\tmax_lv = %d\n", vg->max_lv);
	fprintf(fp, "\tmax_pv = %d\n", vg->max_pv);
	fprintf(fp, "\tpv_count = %d\n", vg->pv_count);
	fprintf(fp, "\tlv_count = %d\n", vg->lv_count);
	fprintf(fp, "}\n\n");

	list_for_each(tmp, &vg->pvs) {
		struct pv_list *pvl = list_entry(tmp, struct pv_list, list);
		_dump_pv(&pvl->pv, fp);
	}

	list_for_each(tmp, &vg->lvs) {
		struct lv_list *lvl = list_entry(tmp, struct lv_list, list);
		_dump_lv(&lvl->lv, fp);
	}
}

int main(int argc, char **argv)
{
	struct io_space *ios;
	struct volume_group *vg;
	struct pool *mem;

	if (argc != 2) {
		fprintf(stderr, "usage: read_vg_t <vg_name>\n");
		exit(1);
	}

	init_log(stderr);
	init_debug(_LOG_INFO);

	if (!dev_cache_init()) {
		fprintf(stderr, "init of dev-cache failed\n");
		exit(1);
	}

	if (!dev_cache_add_dir("/dev/loop")) {
		fprintf(stderr, "couldn't add /dev to dir-cache\n");
		exit(1);
	}

	if (!(mem = pool_create(10 * 1024))) {
		fprintf(stderr, "couldn't create pool\n");
		exit(1);
	}

	ios = create_lvm1_format("/dev", mem, NULL);

	if (!ios) {
		fprintf(stderr, "failed to create io_space for format1\n");
		exit(1);
	}

	vg = ios->vg_read(ios, argv[1]);

	if (!vg) {
		fprintf(stderr, "couldn't read vg %s\n", argv[1]);
		exit(1);
	}

	_dump_vg(vg, stdout);

	ios->destroy(ios);

	pool_destroy(mem);
	dev_cache_exit();
	dump_memory();
	fin_log();
	return 0;
}
