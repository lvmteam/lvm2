/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "units.h"
#include "lib/misc/lib.h"

#define process_event mirror_test_process_event
#define register_device mirror_test_register_device
#define unregister_device mirror_test_unregister_device
#include "daemons/dmeventd/libdevmapper-event.h"

/* Mirror uses a single-target dm_task mock; pool plugins share pool_transport. */
#define dmeventd_lvm2_init mirror_test_lvm2_init
#define dmeventd_lvm2_exit mirror_test_lvm2_exit
#define dmeventd_lvm2_lock mirror_test_lvm2_lock
#define dmeventd_lvm2_unlock mirror_test_lvm2_unlock
#define dmeventd_lvm2_command mirror_test_lvm2_command
#define dmeventd_lvm2_run mirror_test_lvm2_run
#include "daemons/dmeventd/plugins/lvm2/dmeventd_lvm.h"

/* Fake only the task transport; status parsing and policy decisions are real. */
struct mirror_target {
	const char *type;
	const char *status;
};

static struct mirror_target _targets[4];
static unsigned _target_count, _reads, _locks, _repairs;
static int _repair_result;

static const char *_task_name(const struct dm_task *dmt) { return "test-mirror"; }
static void *_next_target(struct dm_task *dmt, void *next, uint64_t *start,
			  uint64_t *length, char **type, char **params)
{
	unsigned i = next ? (unsigned) ((struct mirror_target *) next - _targets) : 0;

	T_ASSERT(i < _target_count);
	_reads++;
	*start = 0;
	*length = 1024;
	*type = (char *) _targets[i].type;
	*params = (char *) _targets[i].status;
	return (i + 1 < _target_count) ? (void *) &_targets[i + 1] : NULL;
}

#define dm_task_get_name _task_name
#define dm_get_next_target _next_target
#undef DM_EVENT_LOG_FN
#define DM_EVENT_LOG_FN(subsys)
#include "daemons/dmeventd/plugins/mirror/dmeventd_mirror.c"

#define DMEVENTD_UNIT_LVM2_PREFIX mirror_test
#include "dmeventd_unit_lvm2_lock.h"
int mirror_test_lvm2_command(struct dm_pool *mem, char *buffer, size_t size,
			     const char *cmd, const char *device)
{
	T_ASSERT(!strcmp(cmd, "lvconvert --repair --use-policies"));
	T_ASSERT(!strcmp(device, "test-mirror"));
	return dm_snprintf(buffer, size, "test-policy") >= 0;
}
int mirror_test_lvm2_run(const char *cmd)
{
	T_ASSERT_EQUAL(_locks, 1);
	T_ASSERT(!strcmp(cmd, "test-policy"));
	_repairs++;
	return _repair_result;
}

static void *_init(void)
{
	void *state = NULL;

	_reads = _locks = _repairs = 0;
	_repair_result = 1;
	_target_count = 0;
	T_ASSERT(mirror_test_register_device("test-mirror", "test-uuid", 0, 0, &state));
	return state;
}

static void _fixture_exit(void *fixture)
{
	T_ASSERT_EQUAL(_locks, 0);
	T_ASSERT(mirror_test_unregister_device("test-mirror", "test-uuid", 0, 0, &fixture));
}

static void _add_target(const char *type, const char *status)
{
	T_ASSERT(_target_count < DM_ARRAY_SIZE(_targets));
	_targets[_target_count].type = type;
	_targets[_target_count].status = status;
	_target_count++;
}

static void _event(void *fixture, unsigned repairs)
{
	mirror_test_process_event((struct dm_task *) fixture, DM_EVENT_TIMEOUT, &fixture);
	T_ASSERT_EQUAL(_repairs, repairs);
	T_ASSERT_EQUAL(_reads, _target_count);
	_reads = 0;
	_target_count = 0;
}

static void _mirror(void *fixture, const char *status, unsigned repairs)
{
	_add_target("mirror", status);
	_event(fixture, repairs);
}

/*
 * A healthy in-sync mirror still runs repair so missing PVs can be detected
 * when dm status reports all legs alive. */
static void _insync(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 A", 1);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 A", 2);
}

/*
 * Alive devices with incomplete in-sync regions are ignored until the mirror
 * finishes resyncing. */
static void _partial(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 399/400 1 AA 3 disk 253:0 A", 0);
	_mirror(fixture, "2 253:1 253:2 10/400 1 AA 3 disk 253:0 A", 0);
}

/*
 * A dead ('D') leg triggers the repair policy on each reported failure; the
 * kernel normally replaces the target before the next event. */
static void _dead(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 400/400 1 AD 3 disk 253:0 A", 1);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AD 3 disk 253:0 A", 2);
}

/*
 * A flush failure ('F') is treated as a failure and repaired. */
static void _flush_failed(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 400/400 1 AF 3 disk 253:0 A", 1);
}

/*
 * 'S' and 'R' on mirror legs are only logged (no ME_FAILURE).  When fully
 * in-sync that still yields ME_INSYNC and runs repair; incomplete regions are
 * ignored without repair. */
static void _sync_read_failed(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 400/400 1 AS 3 disk 253:0 A", 1);
	_mirror(fixture, "2 253:1 253:2 399/400 1 AS 3 disk 253:0 A", 1);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AR 3 disk 253:0 A", 2);
	_mirror(fixture, "2 253:1 253:2 399/400 1 AR 3 disk 253:0 A", 2);
}

/*
 * Unknown health characters and 'U' are treated as failures. */
static void _unclassified(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 400/400 1 AX 3 disk 253:0 A", 1);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AU 3 disk 253:0 A", 2);
}

/*
 * Failures on an external disk log ('D', 'F') trigger repair, while 'S' and
 * 'R' on the log are only logged. */
static void _log_failure(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 A", 1);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 D", 2);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 F", 3);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 S", 4);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 R", 5);
}

/*
 * Core and cluster logs carry no log devices, so only device health matters. */
static void _core_log(void *fixture)
{
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 1 core", 1);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 cluster 253:0 A", 2);
	_mirror(fixture, "4 253:1 253:2 253:3 253:4 400/400 1 ADFF 1 core", 3);
}

/*
 * A failed lvconvert command is reported; the next event may retry, and a
 * subsequent healthy in-sync mirror runs repair again. */
static void _repair_failure(void *fixture)
{
	_repair_result = 0;
	_mirror(fixture, "2 253:1 253:2 400/400 1 AD 3 disk 253:0 A", 1);
	_mirror(fixture, "2 253:1 253:2 400/400 1 AD 3 disk 253:0 A", 2);
	_repair_result = 1;
	_mirror(fixture, "2 253:1 253:2 400/400 1 AA 3 disk 253:0 A", 3);
}

/*
 * Malformed status (including too many devices) is ignored without disabling
 * monitoring. */
static void _invalid(void *fixture)
{
	static const char *bad[] = {
		"",
		"garbage",
		"1 253:1 400/400 1 A",
		"9 253:1 253:2 253:3 253:4 253:5 253:6 253:7 253:8 400/400 1 AAAAAAAAA 1 core"
	};
	unsigned i;

	for (i = 0; i < DM_ARRAY_SIZE(bad); ++i)
		_mirror(fixture, bad[i], 0);
}

/*
 * An unmirrored portion followed by a failing mirror target is repaired
 * once; a lost mapping with no status does nothing; each failing mirror
 * target in one event is repaired. */
static void _multiple_targets(void *fixture)
{
	/* An unmirrored portion may precede the mirror target. */
	_add_target("linear", "0 1024 linear");
	_add_target("mirror", "2 253:1 253:2 400/400 1 AD 3 disk 253:0 A");
	_event(fixture, 1);

	/* A lost mapping delivers no status at all. */
	_add_target(NULL, NULL);
	_event(fixture, 1);

	/* Each failing mirror target in the same event is repaired. */
	_add_target("mirror", "2 253:1 253:2 400/400 1 AD 3 disk 253:0 A");
	_add_target("mirror", "2 253:1 253:2 400/400 1 AD 3 disk 253:0 A");
	_event(fixture, 3);
}

void dmeventd_mirror_tests(struct dm_list *all_tests)
{
	struct test_suite *ts = test_suite_create(_init, _fixture_exit);

	T_ASSERT(ts);
#define TEST(name, desc, fn) register_test(ts, "/dmeventd/mirror/" name, desc, fn)
	TEST("insync", "healthy in-sync mirror runs repair policy", _insync);
	TEST("partial", "ignore incomplete in-sync regions", _partial);
	TEST("dead", "dead leg triggers repair", _dead);
	TEST("flush-failed", "flush failure triggers repair", _flush_failed);
	TEST("sync-read-failed", "leg sync/read logged; in-sync still repairs", _sync_read_failed);
	TEST("unclassified", "unknown health is a failure", _unclassified);
	TEST("log-failure", "log device failures trigger repair", _log_failure);
	TEST("core-log", "core and cluster logs have no log devices", _core_log);
	TEST("repair-failure", "handle failed repair command", _repair_failure);
	TEST("invalid", "recover from malformed status", _invalid);
	TEST("targets", "handle multiple and missing targets", _multiple_targets);
#undef TEST
	dm_list_add(all_tests, &ts->list);
}
