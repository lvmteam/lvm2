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

#include "math.h" /* log10() */

#define DM_STATS_REGION_NOT_PRESENT UINT64_MAX

#define NSEC_PER_USEC   1000L
#define NSEC_PER_MSEC   1000000L
#define NSEC_PER_SEC    1000000000L

#define PRECISE_ARG "precise_timestamps"
#define HISTOGRAM_ARG "histogram:"

/* Histogram bin */
struct dm_histogram_bin {
	uint64_t upper; /* Upper bound on this bin. */
	uint64_t count; /* Count value for this bin. */
};

struct dm_histogram {
	/* The stats handle this histogram belongs to. */
	const struct dm_stats *dms;
	/* The region this histogram belongs to. */
	const struct dm_stats_region *region;
	uint64_t sum; /* Sum of histogram bin counts. */
	int nr_bins; /* Number of histogram bins assigned. */
	struct dm_histogram_bin bins[0];
};

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
	struct dm_histogram *histogram; /* Histogram. */
};

struct dm_stats_region {
	uint64_t region_id; /* as returned by @stats_list */
	uint64_t start;
	uint64_t len;
	uint64_t step;
	char *program_id;
	char *aux_data;
	uint64_t timescale; /* precise_timestamps is per-region */
	struct dm_histogram *bounds; /* histogram configuration */
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
	struct dm_pool *hist_mem; /* separate pool for histogram tables */
	uint64_t nr_regions; /* total number of present regions */
	uint64_t max_region; /* size of the regions table */
	uint64_t interval_ns;  /* sampling interval in nanoseconds */
	uint64_t timescale; /* default sample value multiplier */
	int precise; /* use precise_timestamps when creating regions */
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

	if (!fgets(buf, sizeof(buf), comm)) {
		log_error("Could not read from %s", PROC_SELF_COMM);
		if (fclose(comm))
			stack;
		return NULL;
	}

	if (fclose(comm))
		stack;

	return dm_strdup(buf);
}

static uint64_t _nr_areas(uint64_t len, uint64_t step)
{
	/* Default is one area. */
	if (!len || !step)
		return 1;
	/*
	 * drivers/md/dm-stats.c::message_stats_create()
	 * A region may be sub-divided into areas with their own counters.
	 * Any partial area at the end of the region is treated as an
	 * additional complete area.
	 */
	return (len + step - 1) / step;
}

static uint64_t _nr_areas_region(struct dm_stats_region *region)
{
	return _nr_areas(region->len, region->step);
}

struct dm_stats *dm_stats_create(const char *program_id)
{
	size_t hist_hint = sizeof(struct dm_histogram_bin);
	struct dm_stats *dms = NULL;

	if (!(dms = dm_zalloc(sizeof(*dms))))
		return_NULL;

	/* FIXME: better hint. */
	if (!(dms->mem = dm_pool_create("stats_pool", 4096))) {
		dm_free(dms);
		return_NULL;
	}

	if (!(dms->hist_mem = dm_pool_create("histogram_pool", hist_hint)))
		goto_bad;

	if (!program_id || !strlen(program_id))
		dms->program_id = _program_id_from_proc();
	else
		dms->program_id = dm_strdup(program_id);

	if (!dms->program_id) {
		dm_pool_destroy(dms->hist_mem);
		goto_bad;
	}

	dms->major = -1;
	dms->minor = -1;
	dms->name = NULL;
	dms->uuid = NULL;

	/* by default all regions use msec precision */
	dms->timescale = NSEC_PER_MSEC;
	dms->precise = 0;

	dms->nr_regions = DM_STATS_REGION_NOT_PRESENT;
	dms->max_region = DM_STATS_REGION_NOT_PRESENT;
	dms->regions = NULL;

	return dms;

bad:
	dm_pool_destroy(dms->mem);
	dm_free(dms);
	return NULL;
}

/**
 * Test whether the stats region pointed to by region is present.
 */
static int _stats_region_present(const struct dm_stats_region *region)
{
	return !(region->region_id == DM_STATS_REGION_NOT_PRESENT);
}

static void _stats_histograms_destroy(struct dm_pool *mem,
				      struct dm_stats_region *region)
{
	/* Unpopulated handle. */
	if (!region->counters)
		return;

	/*
	 * Free everything in the pool back to the first histogram.
	 */
	if (region->counters[0].histogram)
		dm_pool_free(mem, region->counters[0].histogram);
}

static void _stats_region_destroy(struct dm_stats_region *region)
{
	if (!_stats_region_present(region))
		return;

	/**
	 * Don't free counters here explicitly; it will be dropped
	 * from the pool along with the corresponding regions table.
	 *
	 * The following objects are all allocated with dm_malloc.
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
	for (i = dms->max_region; (i != DM_STATS_REGION_NOT_PRESENT); i--) {
		_stats_histograms_destroy(dms->hist_mem, &dms->regions[i]);
		_stats_region_destroy(&dms->regions[i]);
	}

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

static int _stats_check_precise_timestamps(const struct dm_stats *dms)
{
	/* Already checked? */
	if (dms && dms->precise)
		return 1;

	return dm_message_supports_precise_timestamps();
}

int dm_stats_driver_supports_precise(void)
{
	return _stats_check_precise_timestamps(NULL);
}

int dm_stats_driver_supports_histogram(void)
{
	return _stats_check_precise_timestamps(NULL);
}

static int _fill_hist_arg(char *hist_arg, size_t hist_len, uint64_t scale,
			  struct dm_histogram *bounds)
{
	int i, l, len = 0, nr_bins;
	char *arg = hist_arg;
	uint64_t value;

	nr_bins = bounds->nr_bins;

	for (i = 0; i < nr_bins; i++) {
		value = bounds->bins[i].upper / scale;
		if ((l = dm_snprintf(arg, hist_len - len, FMTu64"%s", value,
				     (i == (nr_bins - 1)) ? "" : ",")) < 0)
			return_0;
		len += l;
		arg += l;
	}
	return 1;
}

static void *_get_hist_arg(struct dm_histogram *bounds, uint64_t scale,
			   size_t *len)
{
	struct dm_histogram_bin *entry, *bins;
	size_t hist_len = 1; /* terminating '\0' */
	double value;

	entry = bins = bounds->bins;

	entry += bounds->nr_bins - 1;
	while(entry >= bins) {
		value = (double) (entry--)->upper;
		/* Use lround to avoid size_t -> double cast warning. */
		hist_len += 1 + (size_t) lround(log10(value / scale));
		if (entry != bins)
			hist_len++; /* ',' */
	}

	*len = hist_len;

	return dm_zalloc(hist_len);
}

static char *_build_histogram_arg(struct dm_histogram *bounds, int *precise)
{
	struct dm_histogram_bin *entry, *bins;
	size_t hist_len;
	char *hist_arg;
	uint64_t scale;

	entry = bins = bounds->bins;

	/* Empty histogram is invalid. */
	if (!bounds->nr_bins) {
		log_error("Cannot format empty histogram description.");
		return NULL;
	}

	/* Validate entries and set *precise if precision < 1ms. */
	entry += bounds->nr_bins - 1;
	while (entry >= bins) {
		if (entry != bins) {
			if (entry->upper < (entry - 1)->upper) {
				log_error("Histogram boundaries must be in "
					  "order of increasing magnitude.");
				return 0;
			}
		}

		/*
		 * Only enable precise_timestamps automatically if any
		 * value in the histogram bounds uses precision < 1ms.
		 */
		if (((entry--)->upper % NSEC_PER_MSEC) && !*precise)
			*precise = 1;
	}

	scale = (*precise) ? 1 : NSEC_PER_MSEC;

	/* Calculate hist_len and allocate a character buffer. */
	if (!(hist_arg = _get_hist_arg(bounds, scale, &hist_len))) {
		log_error("Could not allocate memory for histogram argument.");
		return 0;
	}

	/* Fill hist_arg with boundary strings. */
	if (!_fill_hist_arg(hist_arg, hist_len, scale, bounds))
		goto_bad;

	return hist_arg;

bad:
	log_error("Could not build histogram arguments.");
	dm_free(hist_arg);

	return NULL;
}

static struct dm_task *_stats_send_message(struct dm_stats *dms, char *msg)
{
	struct dm_task *dmt;

	if (!(dmt = dm_task_create(DM_DEVICE_TARGET_MSG)))
		return_0;

	if (!_set_stats_device(dms, dmt))
		goto_bad;

	if (!dm_task_set_message(dmt, msg))
		goto_bad;

	if (!dm_task_run(dmt))
		goto_bad;

	return dmt;

bad:
	dm_task_destroy(dmt);
	return NULL;
}

/*
 * Parse a histogram specification returned by the kernel in a
 * @stats_list response.
 */
static int _stats_parse_histogram_spec(struct dm_stats *dms,
				       struct dm_stats_region *region,
				       const char *histogram)
{
	static const char *_valid_chars = "0123456789,";
	uint64_t scale = region->timescale, this_val = 0;
	struct dm_pool *mem = dms->hist_mem;
	struct dm_histogram_bin cur;
	struct dm_histogram hist;
	int nr_bins = 1;
	const char *c, *v, *val_start;
	char *p, *endptr = NULL;

	/* Advance past "histogram:". */
	histogram = strchr(histogram, ':');
	if (!histogram) {
		log_error("Could not parse histogram description.");
		return 0;
	}
	histogram++;

	/* @stats_list rows are newline terminated. */
	if ((p = strchr(histogram, '\n')))
		*p = '\0';

	if (!dm_pool_begin_object(mem, sizeof(cur)))
		return_0;

	memset(&hist, 0, sizeof(hist));

	hist.nr_bins = 0; /* fix later */
	hist.region = region;
	hist.dms = dms;

	if (!dm_pool_grow_object(mem, &hist, sizeof(hist)))
		goto_bad;

	c = histogram;
	do {
		for (v = _valid_chars; *v; v++)
			if (*c == *v)
				break;
		if (!*v) {
			stack;
			goto badchar;
		}

		if (*c == ',') {
			log_error("Invalid histogram description: %s",
				  histogram);
			goto bad;
		} else {
			val_start = c;
			endptr = NULL;

			this_val = strtoull(val_start, &endptr, 10);
			if (!endptr) {
				log_error("Could not parse histogram boundary.");
				goto bad;
			}

			c = endptr; /* Advance to units, comma, or end. */

			if (*c == ',')
				c++;
			else if (*c || (*c == ' ')) { /* Expected ',' or NULL. */
				stack;
				goto badchar;
			}

			if (*c == ',')
				c++;

			cur.upper = scale * this_val;
			cur.count = 0;

			if (!dm_pool_grow_object(mem, &cur, sizeof(cur)))
				goto_bad;

			nr_bins++;
		}
	} while (*c && (*c != ' '));

	/* final upper bound. */
	cur.upper = UINT64_MAX;
	if (!dm_pool_grow_object(mem, &cur, sizeof(cur)))
		goto_bad;

	region->bounds = dm_pool_end_object(mem);

	if (!region->bounds)
		return_0;

	region->bounds->nr_bins = nr_bins;

	log_debug("Added region histogram spec with %d entries.", nr_bins);
	return 1;

badchar:
	log_error("Invalid character in histogram: '%c' (0x%x)", *c, *c);
bad:
	dm_pool_abandon_object(mem);
	return 0;
}

static int _stats_parse_list_region(struct dm_stats *dms,
				    struct dm_stats_region *region, char *line)
{
	char *p = NULL, string_data[4096]; /* FIXME: add dm_sscanf with %ms? */
	const char *program_id, *aux_data, *stats_args;
	const char *empty_string = "";
	int r;

	memset(string_data, 0, sizeof(string_data));

	/*
	 * Parse fixed fields, line format:
	 *
	 * <region_id>: <start_sector>+<length> <step> <string data>
	 *
	 * Maximum string data size is 4096 - 1 bytes.
	 */
	r = sscanf(line, FMTu64 ": " FMTu64 "+" FMTu64 " " FMTu64 " %4095c",
		   &region->region_id, &region->start, &region->len,
		   &region->step, string_data);

	if (r != 5)
		return_0;

	/* program_id is guaranteed to be first. */
	program_id = string_data;

	/*
	 * FIXME: support embedded '\ ' in string data:
	 *   s/strchr/_find_unescaped_space()/
	 */
	if ((p = strchr(string_data, ' '))) {
		/* terminate program_id string. */
		*p = '\0';
		if (!strcmp(program_id, "-"))
			program_id = empty_string;
		aux_data = p + 1;
		if ((p = strchr(aux_data, ' '))) {
			/* terminate aux_data string. */
			*p = '\0';
			if (!strcmp(aux_data, "-"))
				aux_data = empty_string;
			stats_args = p + 1;
		} else
			stats_args = empty_string;
	} else
		aux_data = stats_args = empty_string;

	if (strstr(stats_args, PRECISE_ARG))
		region->timescale = 1;
	else
		region->timescale = NSEC_PER_MSEC;

	if ((p = strstr(stats_args, HISTOGRAM_ARG))) {
		if (!_stats_parse_histogram_spec(dms, region, p))
			return_0;
	} else
		region->bounds = NULL;

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
	uint64_t max_region = 0, nr_regions = 0;
	struct dm_stats_region cur, fill;
	struct dm_pool *mem = dms->mem;
	FILE *list_rows;
	/* FIXME: use correct maximum line length for kernel format */
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
		goto_bad;

	while(fgets(line, sizeof(line), list_rows)) {

		if (!_stats_parse_list_region(dms, &cur, line))
			goto_bad;

		/* handle holes in the list of region_ids */
		if (cur.region_id > max_region) {
			memset(&fill, 0, sizeof(fill));
			fill.region_id = DM_STATS_REGION_NOT_PRESENT;
			do {
				if (!dm_pool_grow_object(mem, &fill, sizeof(fill)))
					goto_bad;
			} while (max_region++ < (cur.region_id - 1));
		}

		if (!dm_pool_grow_object(mem, &cur, sizeof(cur)))
			goto_bad;

		max_region++;
		nr_regions++;
	}

	dms->nr_regions = nr_regions;
	dms->max_region = max_region - 1;
	dms->regions = dm_pool_end_object(mem);

	if (fclose(list_rows))
		stack;

	return 1;

bad:
	if (fclose(list_rows))
		stack;
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
		return_0;

	if (!_stats_parse_list(dms, dm_task_get_message_response(dmt))) {
		log_error("Could not parse @stats_list response.");
		goto bad;
	}

	dm_task_destroy(dmt);
	return 1;

bad:
	dm_task_destroy(dmt);
	return 0;
}

/*
 * Parse histogram data returned from a @stats_print operation.
 */
static int _stats_parse_histogram(struct dm_pool *mem, char *hist_str,
				  struct dm_histogram **histogram,
				  struct dm_stats_region *region)
{
	struct dm_histogram hist, *bounds = region->bounds;
	static const char *_valid_chars = "0123456789:";
	int nr_bins = region->bounds->nr_bins;
	const char *c, *v, *val_start;
	struct dm_histogram_bin cur;
	uint64_t sum = 0, this_val;
	char *endptr = NULL;
	int bin = 0;

	c = hist_str;

	if (!dm_pool_begin_object(mem, sizeof(cur)))
		return_0;

	hist.nr_bins = nr_bins;

	if (!dm_pool_grow_object(mem, &hist, sizeof(hist)))
		goto_bad;

	do {
		memset(&cur, 0, sizeof(cur));
		for (v = _valid_chars; *v; v++)
			if (*c == *v)
				break;
		if (!*v)
			goto badchar;

		if (*c == ',')
			goto badchar;
		else {
			val_start = c;
			endptr = NULL;

			this_val = strtoull(val_start, &endptr, 10);
			if (!endptr) {
				log_error("Could not parse histogram value.");
				goto bad;
			}
			c = endptr; /* Advance to colon, or end. */

			if (*c == ':')
				c++;
			else if (*c & (*c != '\n'))
				/* Expected ':', '\n', or NULL. */
				goto badchar;

			if (*c == ':')
				c++;

			cur.upper = bounds->bins[bin].upper;
			cur.count = this_val;
			sum += this_val;

			if (!dm_pool_grow_object(mem, &cur, sizeof(cur)))
				goto_bad;

			bin++;
		}
	} while (*c && (*c != '\n'));

	log_debug("Added region histogram data with %d entries.", nr_bins);

	*histogram = dm_pool_end_object(mem);
	(*histogram)->sum = sum;

	return 1;

badchar:
	log_error("Invalid character in histogram data: '%c' (0x%x)", *c, *c);
bad:
	dm_pool_abandon_object(mem);
	return 0;
}

static int _stats_parse_region(struct dm_stats *dms, const char *resp,
			       struct dm_stats_region *region,
			       uint64_t timescale)
{
	struct dm_histogram *hist = NULL;
	struct dm_pool *mem = dms->mem;
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
		goto_bad;

	/*
	 * dm_task_get_message_response() returns a 'const char *' but
	 * since fmemopen also permits "w" it expects a 'char *'.
	 */
	stats_rows = fmemopen((char *)resp, strlen(resp), "r");
	if (!stats_rows)
		goto_bad;

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
			goto bad;
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

		if (region->bounds) {
			/* Find first histogram separator. */
			char *hist_str = strchr(row, ':');
			if (!hist_str) {
				log_error("Could not parse histogram value.");
				goto bad;
			}
			/* Find space preceding histogram. */
			while (hist_str && *(hist_str - 1) != ' ')
				hist_str--;

			/* Use a separate pool for histogram objects since we
			 * are growing the area table and each area's histogram
			 * table simultaneously.
			 */
			if (!_stats_parse_histogram(dms->hist_mem, hist_str,
						    &hist, region))
				goto_bad;
			hist->dms = dms;
			hist->region = region;
		}

		cur.histogram = hist;

		if (!dm_pool_grow_object(mem, &cur, sizeof(cur)))
			goto_bad;

		if (region->start == UINT64_MAX) {
			region->start = start;
			region->step = len; /* area size is always uniform. */
		}
	}

	region->len = (start + len) - region->start;
	region->timescale = timescale;
	region->counters = dm_pool_end_object(mem);

	if (fclose(stats_rows))
		stack;

	return 1;

bad:
	if (stats_rows)
		if (fclose(stats_rows))
			stack;
	dm_pool_abandon_object(mem);

	return 0;
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

int dm_stats_get_region_nr_histogram_bins(const struct dm_stats *dms,
					  uint64_t region_id)
{
	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;

	if (!dms->regions[region_id].bounds)
		return_0;

	return dms->regions[region_id].bounds->nr_bins;
}

static int _stats_create_region(struct dm_stats *dms, uint64_t *region_id,
				uint64_t start, uint64_t len, int64_t step,
				int precise, const char *hist_arg,
				const char *program_id,	const char *aux_data)
{
	const char *err_fmt = "Could not prepare @stats_create %s.";
	const char *precise_str = PRECISE_ARG;
	const char *resp, *opt_args = NULL;
	char msg[1024], range[64], *endptr = NULL;
	struct dm_task *dmt = NULL;
	int r = 0, nr_opt = 0;

	if (!_stats_bound(dms))
		return_0;

	if (!program_id || !strlen(program_id))
		program_id = dms->program_id;

	if (start || len) {
		if (!dm_snprintf(range, sizeof(range), FMTu64 "+" FMTu64,
				 start, len)) {
			log_error(err_fmt, "range");
			return 0;
		}
	}

	if (precise < 0)
		precise = dms->precise;

	if (precise)
		nr_opt++;
	else
		precise_str = "";

	if (hist_arg)
		nr_opt++;
	else
		hist_arg = "";

	if (nr_opt) {
		if ((dm_asprintf((char **)&opt_args, "%d %s %s%s", nr_opt,
				 precise_str,
				 (strlen(hist_arg)) ? HISTOGRAM_ARG : "",
				 hist_arg)) < 0) {
			log_error(err_fmt, PRECISE_ARG " option.");
			return 0;
		}
	} else
		opt_args = dm_strdup("");

	if (!dm_snprintf(msg, sizeof(msg), "@stats_create %s %s" FMTu64
			 " %s %s %s", (start || len) ? range : "-",
			 (step < 0) ? "/" : "",
			 (uint64_t)llabs(step),
			 opt_args, program_id, aux_data)) {
		log_error(err_fmt, "message");
		dm_free((void *) opt_args);
		return 0;
	}

	if (!(dmt = _stats_send_message(dms, msg)))
		goto_out;

	resp = dm_task_get_message_response(dmt);
	if (!resp) {
		log_error("Could not parse empty @stats_create response.");
		goto out;
	}

	if (region_id) {
		*region_id = strtoull(resp, &endptr, 10);
		if (resp == endptr)
			goto_out;
	}

	r = 1;

out:
	if (dmt)
		dm_task_destroy(dmt);
	dm_free((void *) opt_args);

	return r;
}

int dm_stats_create_region(struct dm_stats *dms, uint64_t *region_id,
			   uint64_t start, uint64_t len, int64_t step,
			   int precise, struct dm_histogram *bounds,
			   const char *program_id, const char *aux_data)
{
	char *hist_arg = NULL;
	int r = 0;

	/* Nanosecond counters and histograms both need precise_timestamps. */
	if ((precise || bounds) && !_stats_check_precise_timestamps(dms))
		return_0;

	if (bounds) {
		/* _build_histogram_arg enables precise if vals < 1ms. */
		if (!(hist_arg = _build_histogram_arg(bounds, &precise)))
			goto_out;
	}

	r = _stats_create_region(dms, region_id, start, len, step,
				 precise, hist_arg, program_id, aux_data);
	dm_free(hist_arg);

out:
	return r;
}

int dm_stats_delete_region(struct dm_stats *dms, uint64_t region_id)
{
	struct dm_task *dmt;
	char msg[1024];

	if (!_stats_bound(dms))
		return_0;

	if (!dm_snprintf(msg, sizeof(msg), "@stats_delete " FMTu64, region_id)) {
		log_error("Could not prepare @stats_delete message.");
		return 0;
	}

	dmt = _stats_send_message(dms, msg);
	if (!dmt)
		return_0;
	dm_task_destroy(dmt);

	return 1;
}

int dm_stats_clear_region(struct dm_stats *dms, uint64_t region_id)
{
	struct dm_task *dmt;
	char msg[1024];

	if (!_stats_bound(dms))
		return_0;

	if (!dm_snprintf(msg, sizeof(msg), "@stats_clear " FMTu64, region_id)) {
		log_error("Could not prepare @stats_clear message.");
		return 0;
	}

	dmt = _stats_send_message(dms, msg);

	if (!dmt)
		return_0;

	dm_task_destroy(dmt);

	return 1;
}

static struct dm_task *_stats_print_region(struct dm_stats *dms,
				    uint64_t region_id, unsigned start_line,
				    unsigned num_lines, unsigned clear)
{
	/* @stats_print[_clear] <region_id> [<start_line> <num_lines>] */
	const char *clear_str = "_clear", *lines_fmt = "%u %u";
	const char *msg_fmt = "@stats_print%s " FMTu64 " %s";
	const char *err_fmt = "Could not prepare @stats_print %s.";
	struct dm_task *dmt = NULL;
	char msg[1024], lines[64];

	if (start_line || num_lines)
		if (!dm_snprintf(lines, sizeof(lines),
				 lines_fmt, start_line, num_lines)) {
			log_error(err_fmt, "row specification");
			return NULL;
		}

	if (!dm_snprintf(msg, sizeof(msg), msg_fmt, (clear) ? clear_str : "",
			 region_id, (start_line || num_lines) ? lines : "")) {
		log_error(err_fmt, "message");
		return NULL;
	}

	if (!(dmt = _stats_send_message(dms, msg)))
		return_NULL;

	return dmt;
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
		return_0;

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
		return_0;
	return dms->nr_regions;
}

/**
 * Test whether region_id is present in this set of stats data
 */
int dm_stats_region_present(const struct dm_stats *dms, uint64_t region_id)
{
	if (!dms->regions)
		return_0;

	if (region_id > dms->max_region)
		return_0;

	return _stats_region_present(&dms->regions[region_id]);
}

static int _dm_stats_populate_region(struct dm_stats *dms, uint64_t region_id,
				     const char *resp)
{
	struct dm_stats_region *region = &dms->regions[region_id];

	if (!_stats_bound(dms))
		return_0;

	if (!_stats_parse_region(dms, resp, region, region->timescale)) {
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
	struct dm_task *dmt = NULL; /* @stats_print task */
	const char *resp;

	if (!_stats_bound(dms))
		return_0;

	/* allow zero-length program_id for populate */
	if (!program_id)
		program_id = dms->program_id;

	if (all_regions && !dm_stats_list(dms, program_id)) {
		log_error("Could not parse @stats_list response.");
		goto bad;
	}

	/* successful list but no regions registered */
	if (!dms->nr_regions)
		return_0;

	dm_stats_walk_start(dms);
	do {
		region_id = (all_regions)
			     ? dm_stats_get_current_region(dms) : region_id;

		/* obtain all lines and clear counter values */
		if (!(dmt = _stats_print_region(dms, region_id, 0, 0, 1)))
			goto_bad;

		resp = dm_task_get_message_response(dmt);
		if (!_dm_stats_populate_region(dms, region_id, resp)) {
			dm_task_destroy(dmt);
			goto_bad;
		}

		dm_task_destroy(dmt);
		dm_stats_walk_next_region(dms);

	} while (all_regions && !dm_stats_walk_end(dms));

	return 1;

bad:
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
	dm_pool_destroy(dms->hist_mem);
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
		return_0;

	if (!dm_stats_get_utilization(dms, &util, region_id, area_id))
		return_0;

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

int dm_stats_get_region_start(const struct dm_stats *dms, uint64_t *start,
			      uint64_t region_id)
{
	if (!dms || !dms->regions)
		return_0;
	*start = dms->regions[region_id].start;
	return 1;
}

int dm_stats_get_region_len(const struct dm_stats *dms, uint64_t *len,
			    uint64_t region_id)
{
	if (!dms || !dms->regions)
		return_0;
	*len = dms->regions[region_id].len;
	return 1;
}

int dm_stats_get_region_area_len(const struct dm_stats *dms, uint64_t *len,
				 uint64_t region_id)
{
	if (!dms || !dms->regions)
		return_0;
	*len = dms->regions[region_id].step;
	return 1;
}

int dm_stats_get_current_region_start(const struct dm_stats *dms,
				      uint64_t *start)
{
	return dm_stats_get_region_start(dms, start, dms->cur_region);
}

int dm_stats_get_current_region_len(const struct dm_stats *dms,
				    uint64_t *len)
{
	return dm_stats_get_region_len(dms, len, dms->cur_region);
}

int dm_stats_get_current_region_area_len(const struct dm_stats *dms,
					 uint64_t *step)
{
	return dm_stats_get_region_area_len(dms, step, dms->cur_region);
}

int dm_stats_get_area_start(const struct dm_stats *dms, uint64_t *start,
			    uint64_t region_id, uint64_t area_id)
{
	struct dm_stats_region *region;
	if (!dms || !dms->regions)
		return_0;
	region = &dms->regions[region_id];
	*start = region->start + region->step * area_id;
	return 1;
}

int dm_stats_get_area_offset(const struct dm_stats *dms, uint64_t *offset,
			     uint64_t region_id, uint64_t area_id)
{
	if (!dms || !dms->regions)
		return_0;
	*offset = dms->regions[region_id].step * area_id;
	return 1;
}

int dm_stats_get_current_area_start(const struct dm_stats *dms,
				    uint64_t *start)
{
	return dm_stats_get_area_start(dms, start,
				       dms->cur_region, dms->cur_area);
}

int dm_stats_get_current_area_offset(const struct dm_stats *dms,
					  uint64_t *offset)
{
	return dm_stats_get_area_offset(dms, offset,
				       dms->cur_region, dms->cur_area);
}

int dm_stats_get_current_area_len(const struct dm_stats *dms,
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

int dm_stats_get_region_precise_timestamps(const struct dm_stats *dms,
					   uint64_t region_id)
{
	struct dm_stats_region *region = &dms->regions[region_id];
	return region->timescale == 1;
}

int dm_stats_get_current_region_precise_timestamps(const struct dm_stats *dms)
{
	return dm_stats_get_region_precise_timestamps(dms, dms->cur_region);
}

/*
 * Histogram access methods.
 */

struct dm_histogram *dm_stats_get_histogram(const struct dm_stats *dms,
					    uint64_t region_id,
					    uint64_t area_id)
{
	region_id = (region_id == DM_STATS_REGION_CURRENT)
		     ? dms->cur_region : region_id ;
	area_id = (area_id == DM_STATS_AREA_CURRENT)
		     ? dms->cur_area : area_id ;

	if (!dms->regions[region_id].counters)
		return dms->regions[region_id].bounds;

	return dms->regions[region_id].counters[area_id].histogram;
}

int dm_histogram_get_nr_bins(const struct dm_histogram *dmh)
{
	return dmh->nr_bins;
}

uint64_t dm_histogram_get_bin_lower(const struct dm_histogram *dmh, int bin)
{
	return (!bin) ? 0 : dmh->bins[bin - 1].upper;
}

uint64_t dm_histogram_get_bin_upper(const struct dm_histogram *dmh, int bin)
{
	return dmh->bins[bin].upper;
}

uint64_t dm_histogram_get_bin_width(const struct dm_histogram *dmh, int bin)
{
	uint64_t upper, lower;
	upper = dm_histogram_get_bin_upper(dmh, bin);
	lower = dm_histogram_get_bin_lower(dmh, bin);
	return (upper - lower);
}

uint64_t dm_histogram_get_bin_count(const struct dm_histogram *dmh, int bin)
{
	return dmh->bins[bin].count;
}

uint64_t dm_histogram_get_sum(const struct dm_histogram *dmh)
{
	return dmh->sum;
}

dm_percent_t dm_histogram_get_bin_percent(const struct dm_histogram *dmh,
					  int bin)
{
	uint64_t value = dm_histogram_get_bin_count(dmh, bin);
	uint64_t width = dm_histogram_get_bin_width(dmh, bin);
	uint64_t total = dm_histogram_get_sum(dmh);

	double val = (double) value;

	if (!total || !value || !width)
		return DM_PERCENT_0;

	return dm_make_percent((uint64_t) val, total);
}

/*
 * Histogram string helper functions: used to construct histogram and
 * bin boundary strings from numeric data.
 */

/*
 * Allocate an unbound histogram object with nr_bins bins. Only used
 * for histograms used to hold bounds values as arguments for calls to
 * dm_stats_create_region().
 */
static struct dm_histogram *_alloc_dm_histogram(int nr_bins)
{
	struct dm_histogram *dmh = NULL;
	struct dm_histogram_bin *cur = NULL;
	/* Allocate space for dm_histogram + nr_entries. */
	size_t size = sizeof(*dmh) + (unsigned) nr_bins * sizeof(*cur);
	return dm_zalloc(size);
}

/*
 * Parse a histogram bounds string supplied by the user. The string
 * consists of a list of numbers, "n1,n2,n3,..." with optional 'ns',
 * 'us', 'ms', or 's' unit suffixes.
 *
 * The scale parameter indicates the timescale used for this region: one
 * for nanoscale resolution and NSEC_PER_MSEC for miliseconds.
 *
 * On return bounds contains a pointer to an array of uint64_t
 * histogram bounds values expressed in units of nanoseconds.
 */
struct dm_histogram *dm_histogram_bounds_from_string(const char *bounds_str)
{
	static const char *_valid_chars = "0123456789,muns";
	uint64_t this_val = 0, mult = 1;
	const char *c, *v, *val_start;
	struct dm_histogram_bin *cur;
	struct dm_histogram *dmh;
	int nr_entries = 1;
	char *endptr;

	c = bounds_str;

	/* Count number of bounds entries. */
	while(*c)
		if (*(c++) == ',')
			nr_entries++;

	c = bounds_str;

	if (!(dmh = _alloc_dm_histogram(nr_entries)))
		return_0;

	dmh->nr_bins = nr_entries;

	cur = dmh->bins;

	do {
		for (v = _valid_chars; *v; v++)
			if (*c == *v)
				break;

		if (!*v) {
			stack;
			goto badchar;
		}

		if (*c == ',') {
			log_error("Empty histogram bin not allowed: %s",
				  bounds_str);
			goto bad;
		} else {
			val_start = c;
			endptr = NULL;

			this_val = strtoull(val_start, &endptr, 10);
			if (!endptr) {
				log_error("Could not parse histogram bound.");
				goto bad;
			}
			c = endptr; /* Advance to units, comma, or end. */

			if (*c == 's') {
				mult = NSEC_PER_SEC;
				c++; /* Advance over 's'. */
			} else if (*(c + 1) == 's') {
				if (*c == 'm')
					mult = NSEC_PER_MSEC;
				else if (*c == 'u')
					mult = NSEC_PER_USEC;
				else if (*c == 'n')
					mult = 1;
				else {
					stack;
					goto badchar;
				}
				c += 2; /* Advance over 'ms', 'us', or 'ns'. */
			} else if (*c == ',')
				c++;
			else if (*c) { /* Expected ',' or NULL. */
				stack;
				goto badchar;
			}

			if (*c == ',')
				c++;
			this_val *= mult;
			(cur++)->upper = this_val;
		}
	} while (*c);

	/* Bounds histograms have no owner. */
	dmh->dms = NULL;
	dmh->region = NULL;

	return dmh;

badchar:
	log_error("Invalid character in histogram: %c", *c);
bad:
	dm_free(dmh);
	return NULL;
}

struct dm_histogram *dm_histogram_bounds_from_uint64(const uint64_t *bounds)
{
	const uint64_t *entry = bounds;
	struct dm_histogram_bin *cur;
	struct dm_histogram *dmh;
	int nr_entries = 1;

	if (!bounds || !bounds[0]) {
		log_error("Could not parse empty histogram bounds array");
		return 0;
	}

	/* Count number of bounds entries. */
	while(*entry)
		if (*(++entry))
			nr_entries++;

	entry = bounds;

	if (!(dmh = _alloc_dm_histogram(nr_entries)))
		return_0;

	dmh->nr_bins = nr_entries;

	cur = dmh->bins;

	while (*entry)
		(cur++)->upper = *(entry++);

	/* Bounds histograms have no owner. */
	dmh->dms = NULL;
	dmh->region = NULL;

	return dmh;
}

void dm_histogram_bounds_destroy(struct dm_histogram *bounds)
{
	if (!bounds)
		return;

	/* Bounds histograms are not bound to any handle or region. */
	if (bounds->dms || bounds->region) {
		log_error("Freeing invalid histogram bounds pointer %p.",
			  (void *) bounds);
		stack;
	}
	/* dm_free() expects a (void *). */
	dm_free((void *) bounds);
}

/*
 * Scale a bounds value down from nanoseconds to the largest possible
 * whole unit suffix.
 */
static void _scale_bound_value_to_suffix(uint64_t *bound, const char **suffix)
{
	*suffix = "ns";
	if (!(*bound % NSEC_PER_SEC)) {
		*bound /= NSEC_PER_SEC;
		*suffix = "s";
	} else if (!(*bound % NSEC_PER_MSEC)) {
		*bound /= NSEC_PER_MSEC;
		*suffix = "ms";
	} else if (!(*bound % NSEC_PER_USEC)) {
		*bound /= NSEC_PER_USEC;
		*suffix = "us";
	}
}

#define DM_HISTOGRAM_BOUNDS_MASK 0x30

static int _make_bounds_string(char *buf, size_t size, uint64_t lower,
			       uint64_t upper, int flags, int width)
{
	const char *l_suff = NULL;
	const char *u_suff = NULL;
	const char *sep = "";
	char bound_buf[32];
	int bounds = flags & DM_HISTOGRAM_BOUNDS_MASK;

	if (!bounds)
		return_0;

	*buf = '\0';

	if (flags & DM_HISTOGRAM_SUFFIX) {
		_scale_bound_value_to_suffix(&lower, &l_suff);
		_scale_bound_value_to_suffix(&upper, &u_suff);
	} else
		l_suff = u_suff = "";

	if (flags & DM_HISTOGRAM_VALUES)
		sep = ":";

	if (bounds > DM_HISTOGRAM_BOUNDS_LOWER) {
		/* Handle infinite uppermost bound. */
		if (upper == UINT64_MAX) {
			if (dm_snprintf(bound_buf, sizeof(bound_buf),
					 ">" FMTu64 "%s", lower, l_suff) < 0)
				goto_out;
			/* Only display an 'upper' string for final bin. */
			bounds = DM_HISTOGRAM_BOUNDS_UPPER;
		} else {
			if (dm_snprintf(bound_buf, sizeof(bound_buf),
					 FMTu64 "%s", upper, u_suff) < 0)
				goto_out;
		}
	} else if (bounds == DM_HISTOGRAM_BOUNDS_LOWER) {
		if ((dm_snprintf(bound_buf, sizeof(bound_buf), FMTu64 "%s",
				 lower, l_suff)) < 0)
			goto_out;
	}

	switch (bounds) {
	case DM_HISTOGRAM_BOUNDS_LOWER:
	case DM_HISTOGRAM_BOUNDS_UPPER:
		return dm_snprintf(buf, size, "%*s%s", width, bound_buf, sep);
	case DM_HISTOGRAM_BOUNDS_RANGE:
		return dm_snprintf(buf, size,  FMTu64 "%s-%s%s",
				   lower, l_suff, bound_buf, sep);
	}
out:
	return 0;
}

#define BOUND_WIDTH_NOSUFFIX 10 /* 999999999 nsecs */
#define BOUND_WIDTH 6 /* bounds string up to 9999xs */
#define COUNT_WIDTH 6 /* count string: up to 9999 */
#define PERCENT_WIDTH 6 /* percent string : 0.00-100.00% */
#define DM_HISTOGRAM_VALUES_MASK 0x06

const char *dm_histogram_to_string(const struct dm_histogram *dmh, int bin,
				   int width, int flags)
{
	int minwidth, bounds, values, start, last;
	uint64_t lower, upper, val_u64; /* bounds of the current bin. */
	/* Use the histogram pool for string building. */
	struct dm_pool *mem = dmh->dms->hist_mem;
	char buf[64], bounds_buf[64];
	const char *sep = "";
	int bounds_width;
	ssize_t len = 0;
	float val_flt;

	bounds = flags & DM_HISTOGRAM_BOUNDS_MASK;
	values = flags & DM_HISTOGRAM_VALUES;

	if (bin < 0) {
		start = 0;
		last = dmh->nr_bins - 1;
	} else
		start = last = bin;

	minwidth = width;

	if (width < 0 || !values)
		width = minwidth = 0; /* no padding */
	else if (flags & DM_HISTOGRAM_PERCENT)
		width = minwidth = (width) ? : PERCENT_WIDTH;
	else if (flags & DM_HISTOGRAM_VALUES)
		width = minwidth = (width) ? : COUNT_WIDTH;

	if (values && !width)
		sep = ":";

	/* Set bounds string to the empty string. */
	bounds_buf[0] = '\0';

	if (!dm_pool_begin_object(mem, 64))
		return_0;

	for (bin = start; bin <= last; bin++) {
		if (bounds) {
			/* Default bounds width depends on time suffixes. */
			bounds_width = (!(flags & DM_HISTOGRAM_SUFFIX))
					? BOUND_WIDTH_NOSUFFIX
					: BOUND_WIDTH ;

			bounds_width = (!width) ? width : bounds_width;

			lower = dm_histogram_get_bin_lower(dmh, bin);
			upper = dm_histogram_get_bin_upper(dmh, bin);

			len = sizeof(bounds_buf);
			len = _make_bounds_string(bounds_buf, len,
						  lower, upper, flags,
						  bounds_width);
			/*
			 * Comma separates "bounds: value" pairs unless
			 * --noheadings is used.
			 */
			sep = (width || !values) ? "," : ":";

			/* Adjust width by real bounds length if set. */
			width -= (width) ? (len - (bounds_width + 1)) : 0;

			/* -ve width indicates specified width was overrun. */
			width = (width > 0) ? width : 0;
		}

		if (bin == last)
			sep = "";

		if (flags & DM_HISTOGRAM_PERCENT) {
			dm_percent_t pr;
			pr = dm_histogram_get_bin_percent(dmh, bin);
			val_flt = dm_percent_to_float(pr);
			len = dm_snprintf(buf, sizeof(buf), "%s%*.2f%%%s",
					  bounds_buf, width, val_flt, sep);
		} else if (values) {
			val_u64 = dmh->bins[bin].count;
			len = dm_snprintf(buf, sizeof(buf), "%s%*"PRIu64"%s",
					  bounds_buf, width, val_u64, sep);
		} else if (bounds)
			len = dm_snprintf(buf, sizeof(buf), "%s%s", bounds_buf,
					  sep);

		if (len < 0)
			goto_bad;

		width = minwidth; /* re-set histogram column width. */
		if (!dm_pool_grow_object(mem, buf, (size_t) len))
			goto_bad;
	}

	if (!dm_pool_grow_object(mem, "\0", 1))
		goto_bad;

	return (const char *) dm_pool_end_object(mem);

bad:
	dm_pool_abandon_object(mem);
	return NULL;
}

/*
 * Backward compatible dm_stats_create_region() implementations.
 *
 * Keep these at the end of the file to avoid adding clutter around the
 * current dm_stats_create_region() version.
 */

#if defined(__GNUC__)
int dm_stats_create_region_v1_02_106(struct dm_stats *dms, uint64_t *region_id,
				     uint64_t start, uint64_t len, int64_t step,
				     int precise, const char *program_id,
				     const char *aux_data);
int dm_stats_create_region_v1_02_106(struct dm_stats *dms, uint64_t *region_id,
				     uint64_t start, uint64_t len, int64_t step,
				     int precise, const char *program_id,
				     const char *aux_data)
{
	/* 1.02.106 lacks histogram argument. */
	return _stats_create_region(dms, region_id, start, len, step, precise,
				    NULL, program_id, aux_data);
}
DM_EXPORT_SYMBOL(dm_stats_create_region, 1_02_106);

int dm_stats_create_region_v1_02_104(struct dm_stats *dms, uint64_t *region_id,
				     uint64_t start, uint64_t len, int64_t step,
				     const char *program_id, const char *aux_data);
int dm_stats_create_region_v1_02_104(struct dm_stats *dms, uint64_t *region_id,
				     uint64_t start, uint64_t len, int64_t step,
				     const char *program_id, const char *aux_data)
{
	/* 1.02.104 lacks histogram and precise arguments. */
	return _stats_create_region(dms, region_id, start, len, step, 0, NULL,
				    program_id, aux_data);
}
DM_EXPORT_SYMBOL(dm_stats_create_region, 1_02_104);
#endif
