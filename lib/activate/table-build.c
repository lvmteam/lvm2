/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the LGPL.
 */

#include "table-build.c"

static void _print_run(FILE *fp, struct logical_volume *lv)
{

}


int build_table(struct volume_group *vg, struct logical_volume *lv,
		const char *file)
{
	int i;
	uint64_t sector = 0;
	uint64_t pe_size = vg->extent_size;
	uint64_t dest;
	struct pe_specifier *pes;
	FILE *fp = fopen(file, "w");

	if (!fp) {
		log_err("couldn't open '%s' to write table", file);
		return 0;
	}

	for (i = 0; i < lv->le_count; i++) {
		pes = lv->map + i;
		dest = pes->pv->pe_start + (pe_size * pes->pe);
		fprintf(fp, "%ull %ull linear %s %ull\n",
			sector, pe_size, pes->pv->dev->name, dest);
		sector += pe_size;
	}
	fclose(fp);

	return 1;
}
