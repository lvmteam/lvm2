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
#include "libdm/libdevmapper.h"

#include <string.h>

//----------------------------------------------------------------

static const char *_test_stats_str =
	"{ version : 36, dataBlocksUsed : 560, overheadBlocksUsed : 1087370, "
	"logicalBlocksUsed : 16478, physicalBlocks : 62911488, "
	"logicalBlocks : 5243136, blockMapCacheSize : 134217728, "
	"blockSize : 4096, completeRecoveries : 0, readOnlyRecoveries : 0, "
	"mode : normal, inRecoveryMode : 0, recoveryPercentage : 100, "
	"packer : { compressedFragmentsWritten : 0, compressedBlocksWritten : 0, "
	"compressedFragmentsInPacker : 0, }, "
	"allocator : { slabCount : 118, slabsOpened : 2, slabsReopened : 0, }, "
	"journal : { diskFull : 0, slabJournalCommitsRequested : 0, "
	"entries : { started : 17136, written : 17136, committed : 17136, }, "
	"blocks : { started : 163, written : 163, committed : 163, }, }, "
	"slabJournal : { diskFullCount : 0, flushCount : 0, blockedCount : 0, "
	"blocksWritten : 0, tailBusyCount : 0, }, "
	"slabSummary : { blocksWritten : 0, }, "
	"refCounts : { blocksWritten : 0, }, "
	"blockMap : { dirtyPages : 25, cleanPages : 0, freePages : 32741, "
	"failedPages : 0, incomingPages : 0, outgoingPages : 0, "
	"cachePressure : 0, readCount : 17162, writeCount : 17042, "
	"failedReads : 0, failedWrites : 0, reclaimed : 0, readOutgoing : 0, "
	"foundInCache : 18949, discardRequired : 0, waitForPage : 15230, "
	"fetchRequired : 25, pagesLoaded : 25, pagesSaved : 0, flushCount : 0, }, "
	"hashLock : { dedupeAdviceValid : 6, dedupeAdviceStale : 0, "
	"concurrentDataMatches : 2, concurrentHashCollisions : 0, "
	"currDedupeQueries : 0, }, "
	"errors : { invalidAdvicePBNCount : 0, noSpaceErrorCount : 0, "
	"readOnlyErrorCount : 0, }, "
	"instance : 0, currentVIOsInProgress : 0, maxVIOs : 2048, "
	"dedupeAdviceTimeouts : 0, flushOut : 4, logicalBlockSize : 4096, "
	"biosIn : { read : 349, write : 17042, emptyFlush : 4, discard : 0, "
	"flush : 4, fua : 1, }, "
	"biosInPartial : { read : 0, write : 0, emptyFlush : 0, discard : 0, "
	"flush : 0, fua : 0, }, "
	"biosOut : { read : 93, write : 586, emptyFlush : 0, discard : 0, "
	"flush : 0, fua : 0, }, "
	"biosMeta : { read : 49, write : 165, emptyFlush : 0, discard : 0, "
	"flush : 164, fua : 164, }, "
	"biosJournal : { read : 0, write : 163, emptyFlush : 0, discard : 0, "
	"flush : 163, fua : 163, }, "
	"biosPageCache : { read : 25, write : 0, emptyFlush : 0, discard : 0, "
	"flush : 0, fua : 0, }, "
	"biosOutCompleted : { read : 93, write : 586, emptyFlush : 0, discard : 0, "
	"flush : 0, fua : 0, }, "
	"biosMetaCompleted : { read : 49, write : 165, emptyFlush : 0, discard : 0, "
	"flush : 164, fua : 164, }, "
	"biosJournalCompleted : { read : 0, write : 163, emptyFlush : 0, discard : 0, "
	"flush : 163, fua : 163, }, "
	"biosPageCacheCompleted : { read : 25, write : 0, emptyFlush : 0, discard : 0, "
	"flush : 0, fua : 0, }, "
	"biosAcknowledged : { read : 349, write : 17042, emptyFlush : 4, discard : 0, "
	"flush : 4, fua : 1, }, "
	"biosAcknowledgedPartial : { read : 0, write : 0, emptyFlush : 0, discard : 0, "
	"flush : 0, fua : 0, }, "
	"biosInProgress : { read : 0, write : 0, emptyFlush : 0, discard : 0, "
	"flush : 0, fua : 0, }, "
	"memoryUsage : { bytesUsed : 482540048, peakBytesUsed : 482540048, }, "
	"index : { entriesIndexed : 586, postsFound : 6, postsNotFound : 586, "
	"queriesFound : 0, queriesNotFound : 0, updatesFound : 0, "
	"updatesNotFound : 0, entriesDiscarded : 0, }, }";

/*
 * Minimal stats strings for edge-case tests.  Only the fields that
 * affect N/A logic need to be present; missing fields stay at their
 * memset(0) defaults.
 */
static const char *_recovery_stats_str =
	"{ dataBlocksUsed : 560, overheadBlocksUsed : 1087370, "
	"logicalBlocksUsed : 16478, physicalBlocks : 62911488, "
	"logicalBlocks : 5243136, blockSize : 4096, "
	"mode : recovering, inRecoveryMode : 1, recoveryPercentage : 42, "
	"logicalBlockSize : 4096, "
	"biosIn : { write : 17042, }, "
	"biosOut : { write : 586, }, "
	"biosMeta : { write : 165, }, }";

static const char *_readonly_stats_str =
	"{ dataBlocksUsed : 560, overheadBlocksUsed : 1087370, "
	"logicalBlocksUsed : 16478, physicalBlocks : 62911488, "
	"logicalBlocks : 5243136, blockSize : 4096, "
	"mode : read-only, inRecoveryMode : 0, recoveryPercentage : 100, "
	"logicalBlockSize : 4096, "
	"biosIn : { write : 17042, }, "
	"biosOut : { write : 586, }, "
	"biosMeta : { write : 165, }, }";

static const char *_zero_logical_stats_str =
	"{ dataBlocksUsed : 0, overheadBlocksUsed : 1087370, "
	"logicalBlocksUsed : 0, physicalBlocks : 62911488, "
	"logicalBlocks : 5243136, blockSize : 4096, "
	"mode : normal, inRecoveryMode : 0, recoveryPercentage : 100, "
	"logicalBlockSize : 4096, "
	"biosIn : { write : 0, }, "
	"biosOut : { write : 0, }, "
	"biosMeta : { write : 0, }, }";

//----------------------------------------------------------------

static void _test_parse_raw_fields(void *fixture)
{
	struct dm_vdo_stats_full *full;
	struct dm_vdo_stats *s;

	full = dm_vdo_stats_parse(NULL, _test_stats_str, DM_VDO_STATS_BASIC);
	T_ASSERT(full);
	s = full->stats;
	T_ASSERT(s);
	T_ASSERT_EQUAL(s->data_blocks_used, 560ULL);
	T_ASSERT_EQUAL(s->overhead_blocks_used, 1087370ULL);
	T_ASSERT_EQUAL(s->logical_blocks_used, 16478ULL);
	T_ASSERT_EQUAL(s->physical_blocks, 62911488ULL);
	T_ASSERT_EQUAL(s->logical_blocks, 5243136ULL);
	T_ASSERT_EQUAL(s->bytes_per_physical_block, 4096ULL);
	T_ASSERT_EQUAL(s->bytes_per_logical_block, 4096ULL);
	T_ASSERT_EQUAL(s->bios_in, 17042ULL);
	T_ASSERT_EQUAL(s->bios_out, 586ULL);
	T_ASSERT_EQUAL(s->bios_meta, 165ULL);
	T_ASSERT_EQUAL(s->operating_mode, DM_VDO_MODE_NORMAL);
	dm_free(full);
}

static void _test_parse_null_input(void *fixture)
{
	T_ASSERT(!dm_vdo_stats_parse(NULL, NULL, DM_VDO_STATS_BASIC));
}

static void _test_parse_full_null_input(void *fixture)
{
	T_ASSERT(!dm_vdo_stats_parse(NULL, NULL, DM_VDO_STATS_FULL));
}

static void _test_parse_full(void *fixture)
{
	struct dm_vdo_stats_full *full;

	full = dm_vdo_stats_parse(NULL, _test_stats_str, DM_VDO_STATS_FULL);
	T_ASSERT(full);
	T_ASSERT(full->field_count > 0);
	T_ASSERT(full->stats);
	T_ASSERT_EQUAL(full->stats->data_blocks_used, 560ULL);
	T_ASSERT_EQUAL(full->stats->bios_in, 17042ULL);
	dm_free(full);
}

static void _test_recovery_mode(void *fixture)
{
	struct dm_vdo_stats_full *full;

	full = dm_vdo_stats_parse(NULL, _recovery_stats_str, DM_VDO_STATS_BASIC);
	T_ASSERT(full);
	T_ASSERT_EQUAL(full->stats->operating_mode, DM_VDO_MODE_RECOVERING);
	T_ASSERT_EQUAL(full->stats->bios_in, 17042ULL);
	dm_free(full);
}

static void _test_readonly_mode(void *fixture)
{
	struct dm_vdo_stats_full *full;

	full = dm_vdo_stats_parse(NULL, _readonly_stats_str, DM_VDO_STATS_BASIC);
	T_ASSERT(full);
	T_ASSERT_EQUAL(full->stats->operating_mode, DM_VDO_MODE_READ_ONLY);
	dm_free(full);
}

static void _test_zero_logical_blocks(void *fixture)
{
	struct dm_vdo_stats_full *full;

	full = dm_vdo_stats_parse(NULL, _zero_logical_stats_str, DM_VDO_STATS_BASIC);
	T_ASSERT(full);
	T_ASSERT_EQUAL(full->stats->logical_blocks_used, 0ULL);
	T_ASSERT_EQUAL(full->stats->bios_in, 0ULL);
	dm_free(full);
}

/*
 * Tripwire: assert that all fields we depend on are present in the
 * kernel stats string.  If the kernel drops or renames a field this
 * test fails and we know we need to revisit the struct.
 */
static const char *_expected_labels[] = {
	"data blocks used",    "overhead blocks used",
	"logical blocks used", "physical blocks",
	"logical blocks",      "block size",
	"logical block size",  "mode",
	"bios in write",       "bios out write",
	"bios meta write",
};

#define NUM_EXPECTED_LABELS \
	(sizeof(_expected_labels) / sizeof(_expected_labels[0]))

static void _test_all_expected_fields_present(void *fixture)
{
	struct dm_vdo_stats_full *full;
	int found[NUM_EXPECTED_LABELS];
	unsigned i;
	int j;

	memset(found, 0, sizeof(found));
	full = dm_vdo_stats_parse(NULL, _test_stats_str, DM_VDO_STATS_FULL);
	T_ASSERT(full);

	for (j = 0; j < full->field_count; j++)
		for (i = 0; i < NUM_EXPECTED_LABELS; i++)
			if (!strcmp(full->fields[j].label, _expected_labels[i]))
				found[i] = 1;

	for (i = 0; i < NUM_EXPECTED_LABELS; i++)
		T_ASSERT(found[i]);

	dm_free(full);
}

//----------------------------------------------------------------

#define T(path, desc, fn) \
	register_test(ts, "/device-mapper/vdo/stats/" path, desc, fn)

static struct test_suite *_tests(void)
{
	struct test_suite *ts = test_suite_create(NULL, NULL);
	if (!ts) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	};

	T("parse-raw",
	  "parse raw fields from stats string",
	  _test_parse_raw_fields);
	T("parse-null", "parse handles null input", _test_parse_null_input);
	T("parse-full",
	  "parse_full returns stats and label/value arrays",
	  _test_parse_full);
	T("parse-full-null",
	  "parse_full handles null stats string",
	  _test_parse_full_null_input);
	T("recovery-mode",
	  "recovering mode sets operating_mode correctly",
	  _test_recovery_mode);
	T("readonly-mode",
	  "read-only mode sets operating_mode correctly",
	  _test_readonly_mode);
	T("zero-logical",
	  "zero logical blocks parses cleanly",
	  _test_zero_logical_blocks);
	T("fields-present",
	  "all expected labels present in stats blob",
	  _test_all_expected_fields_present);

	return ts;
}

void vdo_stats_tests(struct dm_list *all_tests)
{
	dm_list_add(all_tests, &_tests()->list);
}

//----------------------------------------------------------------
