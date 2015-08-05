/*
 * Copyright (C) 2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "dmlib.h"
#include "libdm-targets.h"
#include "libdm-common.h"

#define DM_STATS_REGION_NOT_PRESENT UINT64_MAX

#define NSEC_PER_MSEC   1000000L
#define NSEC_PER_SEC    1000000000L

/*
 * See Documentation/device-mapper/statistics.txt for full descriptions
 * of the device-mapper statistics counter fields.
 */
struct dm_stats_counters {
	uint64_t reads;		    /* Num reads completed */
	uint64_t reads_merged;	    /* Num reads merged */
	uint64_t read_sectors;	    /* Num sectors read */
	uint64_t read_nsecs;	    /* Num milliseconds spent reading */
	uint64_t writes;	    /* Num writes completed */
	uint64_t writes_merged;	    /* Num writes merged */
	uint64_t write_sectors;	    /* Num sectors written */
	uint64_t write_nsecs;	    /* Num milliseconds spent writing */
	uint64_t io_in_progress;    /* Num I/Os currently in progress */
	uint64_t io_nsecs;	    /* Num milliseconds spent doing I/Os */
	uint64_t weighted_io_nsecs; /* Weighted num milliseconds doing I/Os */
	uint64_t total_read_nsecs;  /* Total time spent reading in milliseconds */
	uint64_t total_write_nsecs; /* Total time spent writing in milliseconds */
};

struct dm_stats_region {
	uint64_t region_id; /* as returned by @stats_list */
	uint64_t start;
	uint64_t len;
	uint64_t step;
	char *program_id;
	char *aux_data;
	uint64_t timescale; /* precise_timestamps is per-region */
	struct dm_stats_counters *counters;
};

struct dm_stats {
	/* device binding */
	int major;  /* device major that this dm_stats object is bound to */
	int minor;  /* device minor that this dm_stats object is bound to */
	char *name; /* device-mapper device name */
	char *uuid; /* device-mapper UUID */
	char *program_id; /* default program_id for this handle */
	struct dm_pool *mem; /* memory pool for region and counter tables */
	uint64_t nr_regions; /* total number of present regions */
	uint64_t max_region; /* size of the regions table */
	uint64_t interval_ns;  /* sampling interval in nanoseconds */
	uint64_t timescale; /* sample value multiplier */
	struct dm_stats_region *regions;
	/* statistics cursor */
	uint64_t cur_region;
	uint64_t cur_area;
};

#define PROC_SELF_COMM "/proc/self/comm"
static char *_program_id_from_proc(void)
{
	FILE *comm = NULL;
	char buf[256];

	if (!(comm = fopen(PROC_SELF_COMM, "r")))
		return_NULL;

	if (!fgets(buf, sizeof(buf), comm))
		return_NULL;

	if (fclose(comm))
		return_NULL;

	return dm_strdup(buf);
}

struct dm_stats *dm_stats_create(const char *program_id)
{
	struct dm_stats *dms = NULL;

	if (!(dms = dm_malloc(sizeof(*dms))))
		return_NULL;
	if (!(dms->mem = dm_pool_create("stats_pool", 4096)))
		return_NULL;

	if (!program_id || !strlen(program_id))
		dms->program_id = _program_id_from_proc();
	else
		dms->program_id = dm_strdup(program_id);

	dms->major = -1;
	dms->minor = -1;
	dms->name = NULL;
	dms->uuid = NULL;

	/* all regions currently use msec precision */
	dms->timescale = NSEC_PER_MSEC;

	dms->nr_regions = DM_STATS_REGION_NOT_PRESENT;
	dms->max_region = DM_STATS_REGION_NOT_PRESENT;
	dms->regions = NULL;

	return dms;
}

/**
 * Test whether the stats region pointed to by region is present.
 */
static int _stats_region_present(const struct dm_stats_region *region)
{
	return !(region->region_id == DM_STATS_REGION_NOT_PRESENT);
}

static void _stats_region_destroy(struct dm_stats_region *region)
{
	if (!_stats_region_present(region))
		return;

	/**
	 * Don't free counters here explicitly; it will be dropped
	 * from the pool along with the corresponding regions table.
	 */

	if (region->program_id)
		dm_free(region->program_id);
	if (region->aux_data)
		dm_free(region->aux_data);
}

static void _stats_regions_destroy(struct dm_stats *dms)
{
	struct dm_pool *mem = dms->mem;
	uint64_t i;

	if (!dms->regions)
		return;

	/* walk backwards to obey pool order */
	for (i = dms->max_region; (i != DM_STATS_REGION_NOT_PRESENT); i--)
		_stats_region_destroy(&dms->regions[i]);
	dm_pool_free(mem, dms->regions);
}

static int _set_stats_device(struct dm_stats *dms, struct dm_task *dmt)
{
	if (dms->name)
		return dm_task_set_name(dmt, dms->name);
	if (dms->uuid)
		return dm_task_set_uuid(dmt, dms->uuid);
	if (dms->major > 0)
		return dm_task_set_major(dmt, dms->major)
			&& dm_task_set_minor(dmt, dms->minor);
	return_0;
}

static int _stats_bound(struct dm_stats *dms)
{
	if (dms->major > 0 || dms->name || dms->uuid)
		return 1;
	/* %p format specifier expects a void pointer. */
	log_debug("Stats handle at %p is not bound.", (void *) dms);
	return 0;
}

static void _stats_clear_binding(struct dm_stats *dms)
{
	if (dms->name)
		dm_pool_free(dms->mem, dms->name);
	if (dms->uuid)
		dm_pool_free(dms->mem, dms->uuid);

	dms->name = dms->uuid = NULL;
	dms->major = dms->minor = -1;
}

int dm_stats_bind_devno(struct dm_stats *dms, int major, int minor)
{
	_stats_clear_binding(dms);
	_stats_regions_destroy(dms);

	dms->major = major;
	dms->minor = minor;

	return 1;
}

int dm_stats_bind_name(struct dm_stats *dms, const char *name)
{
	_stats_clear_binding(dms);
	_stats_regions_destroy(dms);

	if (!(dms->name = dm_pool_strdup(dms->mem, name)))
		return_0;

	return 1;
}

int dm_stats_bind_uuid(struct dm_stats *dms, const char *uuid)
{
	_stats_clear_binding(dms);
	_stats_regions_destroy(dms);

	if (!(dms->uuid = dm_pool_strdup(dms->mem, uuid)))
		return_0;

	return 1;
}

static struct dm_task *_stats_send_message(struct dm_stats *dms, char *msg)
{
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_TARGET_MSG)))
		return_0;

	if (!_set_stats_device(dms, dmt))
		goto_out;

	if (!dm_task_set_message(dmt, msg))
		goto_out;

	if (!dm_task_run(dmt))
		goto_out;

	return dmt;
out:
	dm_task_destroy(dmt);
	return NULL;
}

static int _stats_parse_list_region(struct dm_stats_region *region, char *line)
{
	/* FIXME: the kernel imposes no length limit here */
	char program_id[256], aux_data[256];
	int r;

	/* line format:
	 * <region_id>: <start_sector>+<length> <step> <program_id> <aux_data>
	 */
	r = sscanf(line, FMTu64 ": " FMTu64 "+" FMTu64 " " FMTu64 "%255s %255s",
		   &region->region_id, &region->start, &region->len, &region->step,
		   program_id, aux_data);

	if (r != 6)
		return_0;

	if (!strcmp(program_id, "-"))
		program_id[0] = '\0';
	if (!strcmp(aux_data, "-"))
		aux_data[0] = '\0';

	if (!(region->program_id = dm_strdup(program_id)))
		return_0;
	if (!(region->aux_data = dm_strdup(aux_data))) {
		dm_free(region->program_id);
		return_0;
	}

	region->counters = NULL;
	return 1;
}

static int _stats_parse_list(struct dm_stats *dms, const char *resp)
{
	struct dm_pool *mem = dms->mem;
	struct dm_stats_region cur;
	uint64_t max_region = 0, nr_regions = 0;
	FILE *list_rows;
	/* FIXME: determine correct maximum line length based on kernel format */
	char line[256];

	if (!resp) {
		log_error("Could not parse NULL @stats_list response.");
		return 0;
	}

	if (dms->regions)
		_stats_regions_destroy(dms);

	/* no regions */
	if (!strlen(resp)) {
		dms->nr_regions = dms->max_region = 0;
		dms->regions = NULL;
		return 1;
	}

	/*
	 * dm_task_get_message_response() returns a 'const char *' but
	 * since fmemopen also permits "w" it expects a 'char *'.
	 */
	if (!(list_rows = fmemopen((char *)resp, strlen(resp), "r")))
		return_0;

	if (!dm_pool_begin_object(mem, 1024))
		goto_out;

	while(fgets(line, sizeof(line), list_rows)) {

		if (!_stats_parse_list_region(&cur, line))
			goto_out;

		/* handle holes in the list of region_ids */
		if (cur.region_id > max_region) {
			struct dm_stats_region fill;
			memset(&fill, 0, sizeof(fill));
			fill.region_id = DM_STATS_REGION_NOT_PRESENT;
			do {
				if (!dm_pool_grow_object(mem, &fill, sizeof(fill)))
					goto_out;
			} while (max_region++ < (cur.region_id - 1));
		}

		if (!dm_pool_grow_object(mem, &cur, sizeof(cur)))
			goto_out;

		max_region++;
		nr_regions++;
	}

	dms->nr_regions = nr_regions;
	dms->max_region = max_region - 1;
	dms->regions = dm_pool_end_object(mem);

	fclose(list_rows);
	return 1;
out:
	fclose(list_rows);
	dm_pool_abandon_object(mem);
	return 0;
}

int dm_stats_list(struct dm_stats *dms, const char *program_id)
{
	struct dm_task *dmt;
	char msg[256];
	int r;

	if (!_stats_bound(dms))
		return_0;

	/* allow zero-length program_id for list */
	if (!program_id)
		program_id = dms->program_id;

	r = dm_snprintf(msg, sizeof(msg), "@stats_list %s", program_id);

	if (r < 0) {
		log_error("Failed to prepare stats message.");
		return 0;
	}

	if (!(dmt = _stats_send_message(dms, msg)))
		return 0;

	if (!_stats_parse_list(dms, dm_task_get_message_response(dmt))) {
		log_error("Could not parse @stats_list response.");
		goto out;
	}

	dm_task_destroy(dmt);
	return 1;

out:
	dm_task_destroy(dmt);
	return 0;
}

static int _stats_parse_region(struct dm_pool *mem, const char *resp,
			       struct dm_stats_region *region,
			       uint64_t timescale)
{
	struct dm_stats_counters cur;
	FILE *stats_rows = NULL;
	uint64_t start, len;
	char row[256];
	int r;

	if (!resp) {
		log_error("Could not parse empty @stats_print response.");
		return 0;
	}

	region->start = UINT64_MAX;

	if (!dm_pool_begin_object(mem, 512))
		goto_out;

	/*
	 * dm_task_get_message_response() returns a 'const char *' but
	 * since fmemopen also permits "w" it expects a 'char *'.
	 */
	stats_rows = fmemopen((char *)resp, strlen(resp), "r");
	if (!stats_rows)
		goto_out;

	/*
	 * Output format for each step-sized area of a region:
	 *
	 * <start_sector>+<length> counters
	 *
	 * The first 11 counters have the same meaning as
	 * /sys/block/ * /stat or /proc/diskstats.
	 *
	 * Please refer to Documentation/iostats.txt for details.
	 *
	 * 1. the number of reads completed
	 * 2. the number of reads merged
	 * 3. the number of sectors read
	 * 4. the number of milliseconds spent reading
	 * 5. the number of writes completed
	 * 6. the number of writes merged
	 * 7. the number of sectors written
	 * 8. the number of milliseconds spent writing
	 * 9. the number of I/Os currently in progress
	 * 10. the number of milliseconds spent doing I/Os
	 * 11. the weighted number of milliseconds spent doing I/Os
	 *
	 * Additional counters:
	 * 12. the total time spent reading in milliseconds
	 * 13. the total time spent writing in milliseconds
	 *
	*/
	while (fgets(row, sizeof(row), stats_rows)) {
		r = sscanf(row, FMTu64 "+" FMTu64 /* start+len */
			   /* reads */
			   FMTu64 " " FMTu64 " " FMTu64 " " FMTu64 " "
			   /* writes */
			   FMTu64 " " FMTu64 " " FMTu64 " " FMTu64 " "
			   /* in flight & io nsecs */
			   FMTu64 " " FMTu64 " " FMTu64 " "
			   /* tot read/write nsecs */
			   FMTu64 " " FMTu64, &start, &len,
			   &cur.reads, &cur.reads_merged, &cur.read_sectors,
			   &cur.read_nsecs,
			   &cur.writes, &cur.writes_merged, &cur.write_sectors,
			   &cur.write_nsecs,
			   &cur.io_in_progress,
			   &cur.io_nsecs, &cur.weighted_io_nsecs,
			   &cur.total_read_nsecs, &cur.total_write_nsecs);
		if (r != 15) {
			log_error("Could not parse @stats_print row.");
			goto out;
		}

		/* scale time values up if needed */
		if (timescale != 1) {
			cur.read_nsecs *= timescale;
			cur.write_nsecs *= timescale;
			cur.io_nsecs *= timescale;
			cur.weighted_io_nsecs *= timescale;
			cur.total_read_nsecs *= timescale;
			cur.total_write_nsecs *= timescale;
		}

		if(!dm_pool_grow_object(mem, &cur, sizeof(cur)))
			goto_out;
		if (region->start == UINT64_MAX) {
			region->start = start;
			region->step = len; /* area size is always uniform. */
		}
	}

	region->len = (start + len) - region->start;
	region->timescale = timescale;
	region->counters = dm_pool_end_object(mem);

	fclose(stats_rows);
	return 1;

out:

	if (stats_rows)
		fclose(stats_rows);
	dm_pool_abandon_object(mem);
	return 0;
}

static uint64_t _nr_areas(uint64_t len, uint64_t step)
{
	/*
	 * drivers/md/dm-stats.c::message_stats_create()
	 * A region may be sub-divided into areas with their own counters.
	 * If step is non-zero, divide len into that many areas, otherwise
	 * treat the entire region as a single area. Any partial area at the
	 * end of the region is treated as an additional complete area.
	 */
	return (len && step)
		? (len / (step ? step : len)) + !!(len % step) : 0;
}

static uint64_t _nr_areas_region(struct dm_stats_region *region)
{
	return _nr_areas(region->len, region->step);
}

static void _stats_walk_next(const struct dm_stats *dms, int region,
			     uint64_t *cur_r, uint64_t *cur_a)
{
	struct dm_stats_region *cur = NULL;
	int present;

	if (!dms || !dms->regions)
		return;

	cur = &dms->regions[*cur_r];
	present = _stats_region_present(cur);

	if (region && present)
		*cur_a = _nr_areas_region(cur);

	if (region || !present || ++(*cur_a) == _nr_areas_region(cur)) {
		*cur_a = 0;
		while(!dm_stats_region_present(dms, ++(*cur_r))
		      && *cur_r < dms->max_region)
			; /* keep walking until a present region is found
			   * or the end of the table is reached. */
	}

}

static void _stats_walk_start(const struct dm_stats *dms,
			      uint64_t *cur_r, uint64_t *cur_a)
{
	if (!dms || !dms->regions)
		return;

	*cur_r = 0;
	*cur_a = 0;

	/* advance to the first present region */
	if (!dm_stats_region_present(dms, dms->cur_region))
		_stats_walk_next(dms, 0, cur_r, cur_a);
}

void dm_stats_walk_start(struct dm_stats *dms)
{
	_stats_walk_start(dms, &dms->cur_region, &dms->cur_area);
}

void dm_stats_walk_next(struct dm_stats *dms)
{
	_stats_walk_next(dms, 0, &dms->cur_region, &dms->cur_area);
}

void dm_stats_walk_next_region(struct dm_stats *dms)
{
	_stats_walk_next(dms, 1, &dms->cur_region, &dms->cur_area);
}

static int _stats_walk_end(const struct dm_stats *dms,
			   uint64_t *cur_r, uint64_t *cur_a)
{
	struct dm_stats_region *region = NULL;
	int end = 0;

	if (!dms || !dms->regions)
		return 1;

	region = &dms->regions[*cur_r];
	end = (*cur_r > dms->max_region
	       || (*cur_r == dms->max_region
		&& *cur_a >= _nr_areas_region(region)));

	return end;
}

int dm_stats_walk_end(struct dm_stats *dms)
{
	return _stats_walk_end(dms, &dms->cur_region, &dms->cur_area);
}

uint64_t dm_stats_get_region_nr_areas(const struct dm_stats *dms,
				      uint64_t region_id)
{
	struct dm_stats_region *region = &dms->regions[region_id];
	return _nr_areas_region(region);
}

uint64_t dm_stats_get_current_nr_areas(const struct dm_stats *dms)
{
	return dm_stats_get_region_nr_areas(dms, dms->cur_region);
}

uint64_t dm_stats_get_nr_areas(const struct dm_stats *dms)
{
	uint64_t nr_areas = 0;
	/* use a separate cursor */
	uint64_t cur_region, cur_area;

	_stats_walk_start(dms, &cur_region, &cur_area);
	do {
		nr_areas += dm_stats_get_current_nr_areas(dms);
		_stats_walk_next(dms, 1, &cur_region, &cur_area);
	} while (!_stats_walk_end(dms, &cur_region, &cur_area));
	return nr_areas;
}

int dm_stats_create_region(struct dm_stats *dms, uint64_t *region_id,
			   uint64_t start, uint64_t len, int64_t step,
			   const char *program_id, const char *aux_data)
{
	struct dm_task *dmt = NULL;
	char msg[1024], range[64];
	const char *err_fmt = "Could not prepare @stats_create %s.";
	const char *resp;

	if (!_stats_bound(dms))
		return_0;

	if (!program_id || !strlen(program_id))
		program_id = dms->program_id;

	if (start || len) {
		if (!dm_snprintf(range, sizeof(range), FMTu64 "+" FMTu64,
				 start, len)) {
			log_error(err_fmt, "range");
			goto out;
		}
	}

	if (!dm_snprintf(msg, sizeof(msg), "@stats_create %s %s" FMTu64 " %s %s",
			 (start || len) ? range : "-",
			 (step < 0) ? "/" : "",
			 (uint64_t)llabs(step), program_id, aux_data)) {
		log_error(err_fmt, "message");
		goto out;
	}

	if (!(dmt = _stats_send_message(dms, msg)))
		goto out;

	resp = dm_task_get_message_response(dmt);
	if (!resp) {
		log_error("Could not parse empty @stats_create response.");
		goto out;
	}

	if (region_id) {
		char *endptr = NULL;
		*region_id = strtoull(resp, &endptr, 10);
		if (resp == endptr)
			goto_out;
	}

	dm_task_destroy(dmt);

	return 1;
out:
	if(dmt)
		dm_task_destroy(dmt);
	return 0;
}

int dm_stats_delete_region(struct dm_stats *dms, uint64_t region_id)
{
	struct dm_task *dmt;
	char msg[1024];

	if (!_stats_bound(dms))
		return_0;

	if (!dm_snprintf(msg, sizeof(msg), "@stats_delete " FMTu64, region_id)) {
		log_error("Could not prepare @stats_delete message.");
		goto out;
	}

	dmt = _stats_send_message(dms, msg);
	if (!dmt)
		goto_out;
	dm_task_destroy(dmt);
	return 1;

out:
	return 0;
}

int dm_stats_clear_region(struct dm_stats *dms, uint64_t region_id)
{
	struct dm_task *dmt;
	char msg[1024];

	if (!_stats_bound(dms))
		return_0;

	if (!dm_snprintf(msg, sizeof(msg), "@stats_clear " FMTu64, region_id)) {
		log_error("Could not prepare @stats_clear message.");
		goto out;
	}

	dmt = _stats_send_message(dms, msg);
	if (!dmt)
		goto_out;
	dm_task_destroy(dmt);
	return 1;

out:
	return 0;
}

static struct dm_task *_stats_print_region(struct dm_stats *dms,
				    uint64_t region_id, unsigned start_line,
				    unsigned num_lines, unsigned clear)
{
	struct dm_task *dmt = NULL;
	/* @stats_print[_clear] <region_id> [<start_line> <num_lines>] */
	const char *clear_str = "_clear", *lines_fmt = "%u %u";
	const char *msg_fmt = "@stats_print%s " FMTu64 " %s";
	const char *err_fmt = "Could not prepare @stats_print %s.";
	char msg[1024], lines[64];

	if (start_line || num_lines)
		if (!dm_snprintf(lines, sizeof(lines),
				 lines_fmt, start_line, num_lines)) {
			log_error(err_fmt, "row specification");
			goto out;
		}

	if (!dm_snprintf(msg, sizeof(msg), msg_fmt, (clear) ? clear_str : "",
			 region_id, (start_line || num_lines) ? lines : "")) {
		log_error(err_fmt, "message");
		goto out;
	}

	if (!(dmt = _stats_send_message(dms, msg)))
		goto out;

	return dmt;

out:
	return NULL;
}

char *dm_stats_print_region(struct dm_stats *dms, uint64_t region_id,
			    unsigned start_line, unsigned num_lines,
			    unsigned clear)
{
	char *resp = NULL;
	struct dm_task *dmt = NULL;

	if (!_stats_bound(dms))
		return_0;

	dmt = _stats_print_region(dms, region_id,
				  start_line, num_lines, clear);

	if (!dmt)
		return 0;

	resp = dm_pool_strdup(dms->mem, dm_task_get_message_response(dmt));
	dm_task_destroy(dmt);

	if (!resp)
		log_error("Could not allocate memory for response buffer.");

	return resp;
}

void dm_stats_buffer_destroy(struct dm_stats *dms, char *buffer)
{
	dm_pool_free(dms->mem, buffer);
}

uint64_t dm_stats_get_nr_regions(const struct dm_stats *dms)
{
	if (!dms || !dms->regions)
		return 0;
	return dms->nr_regions;
}

/**
 * Test whether region_id is present in this set of stats data
 */
int dm_stats_region_present(const struct dm_stats *dms, uint64_t region_id)
{
	if (!dms->regions)
		return 0;

	if (region_id > dms->max_region)
		return 0;

	return _stats_region_present(&dms->regions[region_id]);
}

static int _dm_stats_populate_region(struct dm_stats *dms, uint64_t region_id,
				     const char *resp)
{
	struct dm_stats_region *region = &dms->regions[region_id];

	if (!_stats_bound(dms))
		return_0;

	if (!_stats_parse_region(dms->mem, resp, region, dms->timescale)) {
		log_error("Could not parse @stats_print message response.");
		return 0;
	}
	region->region_id = region_id;
	return 1;
}

int dm_stats_populate(struct dm_stats *dms, const char *program_id,
		      uint64_t region_id)
{
	int all_regions = (region_id == DM_STATS_REGIONS_ALL);

	if (!_stats_bound(dms))
		return_0;

	/* allow zero-length program_id for populate */
	if (!program_id)
		program_id = dms->program_id;

	if (all_regions && !dm_stats_list(dms, program_id)) {
		log_error("Could not parse @stats_list response.");
		goto out;
	}

	/* successful list but no regions registered */
	if (!dms->nr_regions)
		return 0;

	dm_stats_walk_start(dms);
	do {
		struct dm_task *dmt = NULL; /* @stats_print task */
		const char *resp;

		region_id = (all_regions)
			     ? dm_stats_get_current_region(dms) : region_id;

		/* obtain all lines and clear counter values */
		if (!(dmt = _stats_print_region(dms, region_id, 0, 0, 1)))
			goto_out;

		resp = dm_task_get_message_response(dmt);
		if (!_dm_stats_populate_region(dms, region_id, resp)) {
			dm_task_destroy(dmt);
			goto_out;
		}

		dm_task_destroy(dmt);
		dm_stats_walk_next_region(dms);

	} while (all_regions && !dm_stats_walk_end(dms));

	return 1;

out:
	_stats_regions_destroy(dms);
	dms->regions = NULL;
	return 0;
}

/**
 * destroy a dm_stats object and all associated regions and counter sets.
 */
void dm_stats_destroy(struct dm_stats *dms)
{
	_stats_regions_destroy(dms);
	_stats_clear_binding(dms);
	dm_pool_destroy(dms->mem);
	dm_free(dms->program_id);
	dm_free(dms);
}

/**
 * Methods for accessing counter fields. All methods share the
 * following naming scheme and prototype:
 *
 * uint64_t dm_stats_get_COUNTER(struct dm_stats *, uint64_t, uint64_t)
 *
 * Where the two integer arguments are the region_id and area_id
 * respectively.
 */
#define MK_STATS_GET_COUNTER_FN(counter)				\
uint64_t dm_stats_get_ ## counter(const struct dm_stats *dms,		\
				uint64_t region_id, uint64_t area_id)	\
{									\
	region_id = (region_id == DM_STATS_REGION_CURRENT)		\
		     ? dms->cur_region : region_id ;			\
	area_id = (area_id == DM_STATS_REGION_CURRENT)			\
		   ? dms->cur_area : area_id ;				\
	return dms->regions[region_id].counters[area_id].counter;	\
}

MK_STATS_GET_COUNTER_FN(reads)
MK_STATS_GET_COUNTER_FN(reads_merged)
MK_STATS_GET_COUNTER_FN(read_sectors)
MK_STATS_GET_COUNTER_FN(read_nsecs)
MK_STATS_GET_COUNTER_FN(writes)
MK_STATS_GET_COUNTER_FN(writes_merged)
MK_STATS_GET_COUNTER_FN(write_sectors)
MK_STATS_GET_COUNTER_FN(write_nsecs)
MK_STATS_GET_COUNTER_FN(io_in_progress)
MK_STATS_GET_COUNTER_FN(io_nsecs)
MK_STATS_GET_COUNTER_FN(weighted_io_nsecs)
MK_STATS_GET_COUNTER_FN(total_read_nsecs)
MK_STATS_GET_COUNTER_FN(total_write_nsecs)
#undef MK_STATS_GET_COUNTER_FN

int dm_stats_get_rd_merges_per_sec(const struct dm_stats *dms, double *rrqm,
				   uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	*rrqm = ((double) c->reads_merged) / (double) dms->interval_ns;
	return 1;
}

int dm_stats_get_wr_merges_per_sec(const struct dm_stats *dms, double *wrqm,
				   uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	*wrqm = ((double) c->writes_merged) / (double) dms->interval_ns;
	return 1;
}

int dm_stats_get_reads_per_sec(const struct dm_stats *dms, double *rd_s,
			       uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	*rd_s = ((double) c->reads * NSEC_PER_SEC) / (double) dms->interval_ns;
	return 1;
}

int dm_stats_get_writes_per_sec(const struct dm_stats *dms, double *wr_s,
				uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	*wr_s = ((double) c->writes * (double) NSEC_PER_SEC)
		 / (double) dms->interval_ns;

	return 1;
}

int dm_stats_get_read_sectors_per_sec(const struct dm_stats *dms, double *rsec_s,
				      uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	*rsec_s = ((double) c->read_sectors * (double) NSEC_PER_SEC)
		   / (double) dms->interval_ns;

	return 1;
}

int dm_stats_get_write_sectors_per_sec(const struct dm_stats *dms, double *wsec_s,
				       uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	*wsec_s = ((double) c->write_sectors * (double) NSEC_PER_SEC)
		   / (double) dms->interval_ns;
	return 1;
}

int dm_stats_get_average_request_size(const struct dm_stats *dms, double *arqsz,
				      uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;
	uint64_t nr_ios, nr_sectors;

	if (!dms->interval_ns)
		return_0;

	*arqsz = 0.0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	nr_ios = c->reads + c->writes;
	nr_sectors = c->read_sectors + c->write_sectors;
	if (nr_ios)
		*arqsz = (double) nr_sectors / (double) nr_ios;
	return 1;
}

int dm_stats_get_average_queue_size(const struct dm_stats *dms, double *qusz,
				    uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;
	uint64_t io_ticks;

	if (!dms->interval_ns)
		return_0;

	*qusz = 0.0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	io_ticks = c->weighted_io_nsecs;
	if (io_ticks)
		*qusz = (double) io_ticks / (double) dms->interval_ns;
	return 1;
}

int dm_stats_get_average_wait_time(const struct dm_stats *dms, double *await,
				   uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;
	uint64_t io_ticks, nr_ios;

	if (!dms->interval_ns)
		return_0;

	*await = 0.0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	io_ticks = c->read_nsecs + c->write_nsecs;
	nr_ios = c->reads + c->writes;
	if (nr_ios)
		*await = (double) io_ticks / (double) nr_ios;
	return 1;
}

int dm_stats_get_average_rd_wait_time(const struct dm_stats *dms,
				      double *await, uint64_t region_id,
				      uint64_t area_id)
{
	struct dm_stats_counters *c;
	uint64_t rd_io_ticks, nr_rd_ios;

	if (!dms->interval_ns)
		return_0;

	*await = 0.0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	rd_io_ticks = c->read_nsecs;
	nr_rd_ios = c->reads;
	if (rd_io_ticks)
		*await = (double) rd_io_ticks / (double) nr_rd_ios;
	return 1;
}

int dm_stats_get_average_wr_wait_time(const struct dm_stats *dms,
				      double *await, uint64_t region_id,
				      uint64_t area_id)
{
	struct dm_stats_counters *c;
	uint64_t wr_io_ticks, nr_wr_ios;

	if (!dms->interval_ns)
		return_0;

	*await = 0.0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);
	wr_io_ticks = c->write_nsecs;
	nr_wr_ios = c->writes;
	if (wr_io_ticks && nr_wr_ios)
		*await = (double) wr_io_ticks / (double) nr_wr_ios;
	return 1;
}

int dm_stats_get_service_time(const struct dm_stats *dms, double *svctm,
			      uint64_t region_id, uint64_t area_id)
{
	dm_percent_t util;
	double tput;

	if (!dm_stats_get_throughput(dms, &tput, region_id, area_id))
		return 0;

	if (!dm_stats_get_utilization(dms, &util, region_id, area_id))
		return 0;

	/* avoid NAN with zero counter values */
	if ( (uint64_t) tput == 0 || (uint64_t) util == 0) {
		*svctm = 0.0;
		return 1;
	}
	*svctm = ((double) NSEC_PER_SEC * dm_percent_to_float(util))
		  / (100.0 * tput);
	return 1;
}

int dm_stats_get_throughput(const struct dm_stats *dms, double *tput,
			    uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);

	*tput = (( NSEC_PER_SEC * ((double) c->reads + (double) c->writes))
		 / (double) (dms->interval_ns));
	return 1;
}

int dm_stats_get_utilization(const struct dm_stats *dms, dm_percent_t *util,
			     uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_counters *c;
	uint64_t io_nsecs;

	if (!dms->interval_ns)
		return_0;

	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_REGION_CURRENT)
		   ? dms->cur_area : area_id ;

	c = &(dms->regions[region_id].counters[area_id]);

	/**
	 * If io_nsec > interval_ns there is something wrong with the clock
	 * for the last interval; do not allow a value > 100% utilization
	 * to be passed to a dm_make_percent() call. We expect to see these
	 * at startup if counters have not been cleared before the first read.
	 */
	io_nsecs = (c->io_nsecs <= dms->interval_ns) ? c->io_nsecs : dms->interval_ns;
	*util = dm_make_percent(io_nsecs, dms->interval_ns);

	return 1;
}

void dm_stats_set_sampling_interval_ms(struct dm_stats *dms, uint64_t interval_ms)
{
	/* All times use nsecs internally. */
	dms->interval_ns = interval_ms * NSEC_PER_MSEC;
}

void dm_stats_set_sampling_interval_ns(struct dm_stats *dms, uint64_t interval_ns)
{
	dms->interval_ns = interval_ns;
}

uint64_t dm_stats_get_sampling_interval_ms(const struct dm_stats *dms)
{
	/* All times use nsecs internally. */
	return (dms->interval_ns / NSEC_PER_MSEC);
}

uint64_t dm_stats_get_sampling_interval_ns(const struct dm_stats *dms)
{
	/* All times use nsecs internally. */
	return (dms->interval_ns);
}

int dm_stats_set_program_id(struct dm_stats *dms, int allow_empty,
			    const char *program_id)
{
	if (!allow_empty && (!program_id || !strlen(program_id))) {
		log_error("Empty program_id not permitted without "
			  "allow_empty=1");
		return 0;
	}

	if (!program_id)
		program_id = "";

	if (dms->program_id)
		dm_free(dms->program_id);

	if (!(dms->program_id = dm_strdup(program_id)))
		return_0;

	return 1;
}

uint64_t dm_stats_get_current_region(const struct dm_stats *dms)
{
	return dms->cur_region;
}

uint64_t dm_stats_get_current_area(const struct dm_stats *dms)
{
	return dms->cur_area;
}

uint64_t dm_stats_get_region_start(const struct dm_stats *dms, uint64_t *start,
			      uint64_t region_id)
{
	if (!dms || !dms->regions)
		return_0;
	*start = dms->regions[region_id].start;
	return 1;
}

uint64_t dm_stats_get_region_len(const struct dm_stats *dms, uint64_t *len,
			    uint64_t region_id)
{
	if (!dms || !dms->regions)
		return_0;
	*len = dms->regions[region_id].len;
	return 1;
}

uint64_t dm_stats_get_region_area_len(const struct dm_stats *dms, uint64_t *step,
			    uint64_t region_id)
{
	if (!dms || !dms->regions)
		return_0;
	*step = dms->regions[region_id].step;
	return 1;
}

uint64_t dm_stats_get_current_region_start(const struct dm_stats *dms,
					   uint64_t *start)
{
	return dm_stats_get_region_start(dms, start, dms->cur_region);
}

uint64_t dm_stats_get_current_region_len(const struct dm_stats *dms,
					 uint64_t *len)
{
	return dm_stats_get_region_len(dms, len, dms->cur_region);
}

uint64_t dm_stats_get_current_region_area_len(const struct dm_stats *dms,
					      uint64_t *step)
{
	return dm_stats_get_region_area_len(dms, step, dms->cur_region);
}

uint64_t dm_stats_get_area_start(const struct dm_stats *dms, uint64_t *start,
				 uint64_t region_id, uint64_t area_id)
{
	if (!dms || !dms->regions)
		return_0;
	*start = dms->regions[region_id].step * area_id;
	return 1;
}

uint64_t dm_stats_get_current_area_start(const struct dm_stats *dms,
					 uint64_t *start)
{
	return dm_stats_get_area_start(dms, start,
				       dms->cur_region, dms->cur_area);
}

uint64_t dm_stats_get_current_area_len(const struct dm_stats *dms,
				       uint64_t *len)
{
	return dm_stats_get_region_area_len(dms, len, dms->cur_region);
}

const char *dm_stats_get_region_program_id(const struct dm_stats *dms,
					   uint64_t region_id)
{
	const char *program_id = dms->regions[region_id].program_id;
	return (program_id) ? program_id : "";
}

const char *dm_stats_get_region_aux_data(const struct dm_stats *dms,
					 uint64_t region_id)
{
	const char *aux_data = dms->regions[region_id].aux_data;
	return (aux_data) ? aux_data : "" ;
}

const char *dm_stats_get_current_region_program_id(const struct dm_stats *dms)
{
	return dm_stats_get_region_program_id(dms, dms->cur_region);
}

const char *dm_stats_get_current_region_aux_data(const struct dm_stats *dms)
{
	return dm_stats_get_region_aux_data(dms, dms->cur_region);
}
