/*
 * Copyright (C) 2026 Red Hat, Inc. All rights reserved.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * Generate one or more streams of data blocks, and perform one of these
 * actions:
 *
 * - Write a single stream to a device or file.  This writing is not
 *   required to succeed, and we report the number of blocks written.
 *
 * - Verify that such a stream was written correctly.
 *
 * - Verify that when such several streams have been written to the same
 *   device, only the data which was written actually appears on the
 *   device.
 *
 * - Write an arbitrary number of streams to files in a directory.  This
 *   writing is required to succeed, and we report the number of blocks
 *   written.
 *
 * - Verify that such streams were written correctly.
 *
 */

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

// This is borrowed directly from the Linux kernel code
#define container_of(ptr, type, member)             \
  __extension__ ({                                  \
  __typeof__(((type *)0)->member) *__mptr = (ptr);  \
  (type *)((char *)__mptr - offsetof(type,member)); \
    })

// XXX - DeviceSlice should be renamed.  The name may have been appropriate
//       once, but now it basically is the options from the command line that
//       tell gen_data_blocks what to do.
typedef struct {
  const char *dirPath;     // Path of the directory
  const char *path;        // Path of the device or file
  size_t      blockSize;   // Block size (in bytes)
  int         blockCount;  // Block count (in blocks)
  int         fileCount;   // File count
  int         offset;      // Block offset to virtual block 0 of the slice
  bool        creat;       // Path should open for write with O_CREAT
  bool        direct;      // Path should open with O_DIRECT
  bool        sync;        // Path should open for write with O_SYNC
  bool        fsync;       // Path should be fsync'd once writing is complete
} DeviceSlice;

// Each data stream is identified by an 8 character tag
enum { TAG_SIZE = 8 };

// We do math mod 1000 to process the dedupe fraction
enum { DEDUPE_MODULUS = 1000 };

// Superclass of a generic data stream.  This is used directly for an
// all-zero stream, or is subclassed by a data stream.
typedef struct dataStream {
  // Return true if the data block might have been written by this data stream
  bool (*claim)(struct dataStream *pds, void *buf);
  // Generate the data for block N of this data stream
  void (*generate)(struct dataStream *pds, int n, void *buf, size_t bufSize);
  // Report the number of blocks transferred by this data stream
  void (*report)(struct dataStream *pds);
  unsigned long counter;    // Number of blocks transferred
  struct dataStream *next;  // Next data stream
} DataStream;

// Subclass of a tagged data stream
typedef struct {
  DataStream common;
  char tag[TAG_SIZE];
  double compress;
  long dedupe;
  int streamNumber;
} BlockStream;

// Layout of a data stream.
typedef struct {
  struct __attribute__((packed)) {
    char tag[TAG_SIZE];         // 8 character tag
    int streamNumber;           // stream number of the data
    unsigned long blockNumber;  // block number of the data (not of the block)
  } head;
  char data[];             // rest of variable length block
} DataBlockHeader;

/**********************************************************************/
static int shrinkForDedupe(int number, long dedupe)
{
  // Create deduplication.  Shrink the block or stream number based upon the
  // target dedupe fraction.
  while ((number > 0)
         && ((number * dedupe) % DEDUPE_MODULUS < dedupe)) {
    number >>= 1;
  }
  return number;
}

/**********************************************************************/
static void dumpBlock(void *buf, const char *label)
{
  // Print the tag+number to stderr, as if it was a data block
  DataBlockHeader *dbh = buf;
  char tag[TAG_SIZE + 1];
  memcpy(tag, dbh->head.tag, TAG_SIZE);
  tag[TAG_SIZE] = '\0';
  for (int i = 0; i < TAG_SIZE; i++) {
    if (tag[i] == '\0') {
      tag[i] = ' ';
    } else if (!isprint(tag[i])) {
      tag[i] = '.';
    }
  }
  fprintf(stderr, "%s '%s' %u %lu + data\n", label, tag,
          dbh->head.streamNumber, dbh->head.blockNumber);
}

/***********************************************************************/
static void fillRandomly(void *seedPtr, size_t seedLen, void *ptr, size_t len)
{
  if (seedLen < sizeof(unsigned int)) {
    errx(2, "Invalid header size");
  }
  unsigned int seed = 0;
  unsigned int *pSeed = seedPtr;
  while (seedLen >= sizeof(unsigned int)) {
    seedLen -= sizeof(unsigned int);
    seed |= *pSeed++;
  }
  srandom(seed);

  unsigned long randNum  = 0;
  unsigned long randMask = 0;
  const unsigned long multiplier = (unsigned long) RAND_MAX + 1;

  unsigned char *bp = ptr;
  for (size_t i = 0; i < len; i++) {
    if (randMask < 0xff) {
      randNum  = randNum * multiplier + random();
      randMask = randMask * multiplier + RAND_MAX;
    }
    bp[i] = randNum & 0xff;
    randNum >>= 8;
    randMask >>= 8;
  }
}

/**********************************************************************/
static int intConvert(const char *arg)
{
  char *leftover;
  long value = strtol(arg, &leftover, 0);
  if (leftover[0] != '\0') {
    if (leftover[1] != '\0') {
      errx(2, "Invalid number");
    }
    if (value != (int) value) {
      errx(2, "Numeric value too large");
    }
    switch (leftover[0]) {
    default:
      errx(2, "Invalid number");
    case 'G':
    case 'g':
      value *= 1024;
      // fall thru
    case 'M':
    case 'm':
      value *= 1024;
      // fall thru
    case 'K':
    case 'k':
      value *= 1024;
      break;
    }
  }
  if (value != (int) value) {
    errx(2, "Numeric value too large");
  }
  return (int) value;
}

/**********************************************************************/
/* Code for a tagged data stream */
/**********************************************************************/

/**********************************************************************/
static bool claimBS(DataStream *pds, void *buf)
{
  const BlockStream *pbs = container_of(pds, BlockStream, common);
  DataBlockHeader *dbh = buf;
  return memcmp(dbh->head.tag, pbs->tag, TAG_SIZE) == 0;
}

/**********************************************************************/
static void generateBS(DataStream *pds, int n, void *buf, size_t bufSize)
{
  const BlockStream *pbs = container_of(pds, BlockStream, common);
  // Create deduplication.  Shrink the block number based upon the target
  // dedupe fraction.
  int number = shrinkForDedupe(n, pbs->dedupe);
  // Create compression.  Set nData to allow the target compression fraction.
  int nData = bufSize - (int) (bufSize * pbs->compress);
  // Now populate the data block.  The data are completely determined by
  // the values of the "number", the "tag" and the value of "nData".
  DataBlockHeader *dbh = buf;
  memset(buf, 0, bufSize);
  memcpy(dbh->head.tag, pbs->tag, TAG_SIZE);
  dbh->head.blockNumber = number;
  dbh->head.streamNumber = pbs->streamNumber;
  fillRandomly(&dbh->head, sizeof(dbh->head),
               dbh->data, nData - sizeof(dbh->head));
}

/**********************************************************************/
static void reportBS(DataStream *pds)
{
  const BlockStream *pbs = container_of(pds, BlockStream, common);
  printf("%.*s:%ld\n", TAG_SIZE, pbs->tag, pds->counter);
}

/**********************************************************************/
static DataStream *makeBlockStream(char *arg)
{
  BlockStream *pbs = malloc(sizeof(BlockStream));
  memset(pbs, 0, sizeof(BlockStream));
  pbs->common.claim    = claimBS;
  pbs->common.generate = generateBS;
  pbs->common.report   = reportBS;
  double dedupe = 0.0;
  // arg can have the form "tag", or "tag,dedupe" or "tag,dedupe,compress"
  char *endTag = strchr(arg, ',');
  if (endTag != NULL) {
    char *endCompress, *endDedupe;
    dedupe = strtod(&endTag[1], &endDedupe);
    pbs->dedupe = rint(DEDUPE_MODULUS * dedupe);
    switch (*endDedupe) {
    default:
      errx(2, "Invalid data: %s", arg);
    case '\0':
      break;
    case ',':
      pbs->compress = strtod(&endDedupe[1], &endCompress);
      if (*endCompress != '\0') {
        errx(2, "Invalid data: %s", arg);
      }
      break;
    }
    *endTag = '\0';
  }
  if (strlen(arg) >= TAG_SIZE) {
    errx(2, "the tag string '%s' is too long", arg);
  }
  strncpy(pbs->tag, arg, TAG_SIZE);
  if ((pbs->dedupe < 0) || (pbs->dedupe > DEDUPE_MODULUS)) {
    errx(2, "the dedupe fraction (%f) is invalid", dedupe);
  }
  // We need a header on each data block, so 100% compression doesn't
  // really work.  Any compression higher than 93% is not effective for
  // VDO.
  if ((pbs->compress < 0.0) || (pbs->compress > 0.96)) {
    errx(2, "the compression fraction (%f) is invalid", pbs->compress);
  }
  return &pbs->common;
}

/**********************************************************************/
/* Code for a zero data stream.  Used for a new or trimmed VDO device */
/**********************************************************************/

/**********************************************************************/
static bool claimZS(DataStream *pds __attribute__((unused)), void *buf)
{
  unsigned long *data = buf;
  return (data[0] == 0) && (data[1] == 0);
}

/**********************************************************************/
static void generateZS(DataStream *pds  __attribute__((unused)),
                       int         n    __attribute__((unused)),
                       void       *buf,
                       size_t      bufSize)
{
  memset(buf, 0, bufSize);
}

/**********************************************************************/
static void reportZS(DataStream *pds)
{
  printf("ZERO:%ld\n", pds->counter);
}

/**********************************************************************/
static DataStream *makeZeroStream(void)
{
  DataStream *pzs = malloc(sizeof(DataStream));
  memset(pzs, 0, sizeof(DataStream));
  pzs->claim    = claimZS;
  pzs->generate = generateZS;
  pzs->report   = reportZS;
  return pzs;
}

/**********************************************************************/
/* Code for generic data streams */
/**********************************************************************/

/**********************************************************************/
static int countDataStreams(DataStream *pds)
{
  int n = 0;
  for (; pds != NULL; pds = pds->next) {
    n++;
  }
  return n;
}

/**********************************************************************/
static void freeDataStreams(DataStream *pds)
{
  while (pds != NULL) {
    DataStream *t = pds;
    pds = t->next;
    free(t);
  }
}

/**********************************************************************/
static void generateDataStream(DataStream *pds,
                               int         n,
                               void       *buf,
                               size_t      bufSize)
{
  pds->generate(pds, n, buf, bufSize);
}

/**********************************************************************/
static void pushDataStream(DataStream **ppds, DataStream *pds)
{
  pds->next = *ppds;
  *ppds = pds;
}

/**********************************************************************/
static void reportDataStreams(DataStream *pds)
{
  for (; pds != NULL; pds = pds->next) {
    if (pds->counter > 0) {
      pds->report(pds);
    }
  }
}

/**********************************************************************/
static int verifyDataStream(DataStream *pds, int n, void *buf, size_t bufSize)
{
  while (pds != NULL) {
    if (pds->claim(pds, buf)) {
      unsigned char *block = malloc(bufSize);
      if (block == NULL)
        errx(3, "memory allocation failure");
      generateDataStream(pds, n, block, bufSize);
      if (memcmp(buf, block, bufSize) == 0) {
        pds->counter++;
        free(block);
        return 0;
      }
      fprintf(stderr, "block %d compare failure\n", n);
      dumpBlock(buf,   "read    ");
      dumpBlock(block, "expected");
      unsigned char *buffer = buf;
      for (size_t i = 0; i < bufSize; i++) {
        if (buffer[i] != block[i]) {
          fprintf(stderr, "byte %4zu got %02X expected %02X\n", i, buffer[i],
                  block[i]);
        }
      }
      free(block);
      return 1;
    }
    pds = pds->next;
  }
  fprintf(stderr, "block %d unrecognized\n", n);
  dumpBlock(buf, "read");
  return 1;
}

/**********************************************************************/
static void usage(int verbose)
{
  fprintf(stderr,
          "Usage:  gen_data_blocks [--blockCount=N] [--blockSize=N] [--direct]\n"
          "                      [--fileCount=N] [--fsync] [--sync]\n"
          "                      --data=string[,dedupe[,compress]]\n"
          "                      --dir=path writeFiles\n"
          "\n"
          "Usage:  gen_data_blocks [--blockCount=N] [--blockSize=N] [--direct]\n"
          "                      [--fileCount=N] [--sync]\n"
          "                      --data=string[,dedupe[,compress]]\n"
          "                      --dir=path verifyFiles\n"
          "\n"
          "Usage:  gen_data_blocks [--blockCount=N] [--blockSize=N] [--direct]\n"
          "                      [--fsync] [--offset=N] [--sync]\n"
          "                      (--device=path | --file=path)\n"
          "                      --data=string[,dedupe[,compress]]\n"
          "                      writeSlice\n"
          "\n"
          "   or:  gen_data_blocks [--blockCount=N] [--blockSize=N] [--direct]\n"
          "                      [--offset=N] [--sync] [--zero]\n"
          "                      (--device=path | --file=path)\n"
          "                      --data=string[,dedupe[,compress]]...\n"
          "                      verifySlice\n"
          "\n"
          "   or:  gen_data_blocks --help\n");
  if (verbose) {
    fprintf(stderr,
            "\n"
            "\t--blockCount=N  sets the block count to N (default 1)\n"
            "\n"
            "\t--blockSize=N  sets the blocksize to N (default 4K)\n"
            "\n"
            "\t--data=T        sets a data stream with tag T, no dedupe, and\n"
            "\t                no compression\n"
            "\n"
            "\t--data=T,D      sets a data stream with tag T, the dedupe\n"
            "\t                fraction of D (e.g. 0.0 for no dedupe, 0.5\n"
            "\t                for 50%% dedupe), and no compression\n"
            "\n"
            "\t--data=T,D,C    sets a data stream with tag T, the dedupe\n"
            "\t                fraction of D, and the compression fraction\n"
            "\t                of C (e.g. 0.0 for no compression, 0.6 for\n"
            "\t                60%% compression)\n"
            "\n"
            "\t--device=path   sets the device path\n"
            "\n"
            "\t--dir=path      sets the directory path to write files in\n"
            "\n"
            "\t--direct        opens the file with O_DIRECT\n"
            "\n"
            "\t--file=path     sets the file path\n"
            "\n"
            "\t--fileCount=N   sets the file count to N (default 1)\n"
            "\n"
            "\t--fsync         calls fsync on the file when writing\n"
            "\t                completes\n"
            "\n"
            "\t--help          prints this help\n"
            "\n"
            "\t--offset=N      starts I/O at an offset of N (default 0)\n"
            "\n"
            "\t--sync          opens the file with O_SYNC\n"
            "\n"
            "\t--zero          sets a data stream of zero blocks\n"
            "\n"
            "\tverifyFiles  verifies the data is as expected in a directory\n"
            "\t             of files\n"
            "\n"
            "\tverifySlice  verifies the data is as expected on a block\n"
            "\t             stream on a device or a file\n"
            "\n"
            "\twriteFiles   writes the data to a directory of files\n"
            "\n"
            "\twriteSlice   writes the data to a block stream on a device or\n"
            "\t             a file\n"
      );
  }
  exit(verbose ? 0 : 1);
}

/**********************************************************************/
static void *allocateBufferForSlice(const DeviceSlice *ds)
{
  void *buffer;
  if (posix_memalign(&buffer, ds->blockSize, ds->blockSize) != 0) {
    errx(3, "memory allocation failure");
  }
  return buffer;
}

/**********************************************************************/
static void lseekSlice(const DeviceSlice *ds, int fd)
{
  off_t offset = (off_t) ds->blockSize * (off_t) ds->offset;
  if (lseek(fd, offset, SEEK_SET) == (off_t) -1) {
    err(3, "lseek(%s, %#jx) failure", ds->path, (uintmax_t)offset);
  }
}

/**********************************************************************/
static int verifySlice(const DeviceSlice *ds, DataStream *pds)
{
  if (ds->path == NULL) {
    errx(2, "the device path must be provided");
  }

  int status = 0;
  int flags = O_RDONLY | (ds->direct ? O_DIRECT : 0) | (ds->sync ? O_SYNC : 0);
  int fd = open(ds->path, flags);
  if (fd < 0) {
    err(3, "open(%s) failure", ds->path);
  }
  lseekSlice(ds, fd);

  void *buffer = allocateBufferForSlice(ds);
  for (int n = 0; n < ds->blockCount; n++) {
    ssize_t nRead = read(fd, buffer, ds->blockSize);
    if (nRead == -1) {
      warn("read failure on %s, block %d", ds->path, n);
      return 3;
    } else if (nRead != (ssize_t) ds->blockSize) {
      warnx("incomplete read on %s, block %d", ds->path, n);
      return 3;
    }
    if (verifyDataStream(pds, n, buffer, ds->blockSize) != 0) {
      status = 1;
    }
  }
  free(buffer);

  if (close(fd) != 0) {
    err(3, "close failure on %s", ds->path);
  }
  return status;
}

/**********************************************************************/
static int writeSlice(const DeviceSlice *ds, DataStream *pds)
{
  if (ds->path == NULL) {
    errx(2, "the device path must be provided");
  }
  if (countDataStreams(pds) != 1) {
    errx(2, "more than one data stream was provided");
  }

  int flags = (O_WRONLY
               | (ds->creat  ? O_CREAT|O_TRUNC : 0)
               | (ds->direct ? O_DIRECT        : 0)
               | (ds->sync   ? O_SYNC          : 0));
  int fd = open(ds->path, flags, 0666);
  if (fd < 0) {
    err(3, "open(%s) failure", ds->path);
  }
  lseekSlice(ds, fd);

  void *block = allocateBufferForSlice(ds);
  for (int n = 0; n < ds->blockCount; n++) {
    generateDataStream(pds, n, block, ds->blockSize);
    ssize_t nWrite = write(fd, block, ds->blockSize);
    if (nWrite == -1) {
      warn("write failure on %s, block %d", ds->path, n);
      return 3;
    } else if (nWrite != (ssize_t) ds->blockSize) {
      warnx("incomplete write on %s, block %d", ds->path, n);
      return 3;
    }
    pds->counter++;
  }
  free(block);

  if (ds->fsync && (fsync(fd) != 0)) {
    err(3, "fsync(%s) failure", ds->path);
  }
  if (close(fd) != 0) {
    err(3, "close(%s) failure", ds->path);
  }
  return 0;
}

static char filePath[48];

/**********************************************************************/
static int iterateOverFiles(DeviceSlice *ds, DataStream *pds, bool writing)
{
  enum { FILES_PER_DIRECTORY = 200 };
  BlockStream *pbs = container_of(pds, BlockStream, common);
  int status = 0;
  ds->path = filePath;
  int dedupe = pbs->dedupe;
  pbs->dedupe = 0;
  int dirNumber = 0;
  for (int n = 0; n < ds->fileCount; n++) {
    pbs->streamNumber = shrinkForDedupe(n, dedupe);
    if (ds->fileCount <= FILES_PER_DIRECTORY) {
      snprintf(filePath, sizeof(filePath), "%d.%d", n, pbs->streamNumber);
    } else {
      if (n % FILES_PER_DIRECTORY == 0) {
        dirNumber = n / FILES_PER_DIRECTORY;
        if (writing) {
          snprintf(filePath, sizeof(filePath), "D%d", dirNumber);
          if (mkdir(filePath, 0777) == -1) {
            err(3, "mkdir(%s) failure", filePath);
          }
        }
      }
      snprintf(filePath, sizeof(filePath), "D%d/%d.%d", dirNumber, n,
               pbs->streamNumber);
    }
    status = writing ? writeSlice(ds, pds) : verifySlice(ds, pds);
    if (status != 0) {
      break;
    }
  }
  pbs->dedupe = dedupe;
  return status;
}

/**********************************************************************/
static int verifyFiles(DeviceSlice *ds, DataStream *pds)
{
  if (ds->dirPath == NULL) {
    errx(2, "the directory path must be provided");
  }
  if (countDataStreams(pds) != 1) {
    errx(2, "more than one data stream was provided");
  }

  if (chdir(ds->dirPath) == -1) {
    err(3, "chdir(%s) failure", ds->dirPath);
  }
  return iterateOverFiles(ds, pds, false);
}

/**********************************************************************/
static int writeFiles(DeviceSlice *ds, DataStream *pds)
{
  if (ds->dirPath == NULL) {
    errx(2, "the directory path must be provided");
  }
  if (countDataStreams(pds) != 1) {
    errx(2, "more than one data stream was provided");
  }

  if (chdir(ds->dirPath) == -1) {
    if (errno != ENOENT) {
      err(3, "chdir(%s) failure", ds->dirPath);
    }
    if (mkdir(ds->dirPath, 0777) == -1) {
      err(3, "mkdir(%s) failure", ds->dirPath);
    }
    if (chdir(ds->dirPath) == -1) {
      err(3, "chdir(%s) failure", ds->dirPath);
    }
  }

  ds->creat = true;
  return iterateOverFiles(ds, pds, true);
}

/**********************************************************************/
int main(int argc, char **argv)
{
  DataStream *pds = NULL;
  DeviceSlice ds = {
    .dirPath    = NULL,
    .path       = NULL,
    .blockSize  = 4096,
    .blockCount = 1,
    .fileCount  = 1,
    .offset     = 0,
    .creat      = false,
    .direct     = false,
    .sync       = false,
    .fsync      = false,
  };

  enum {
    ACTION_VERIFY_FILES = 1,
    ACTION_VERIFY_SLICE = 2,
    ACTION_WRITE_FILES  = 4,
    ACTION_WRITE_SLICE  = 8,
    ACTIONS_FILES  = ACTION_VERIFY_FILES | ACTION_WRITE_FILES,
    ACTIONS_SLICE  = ACTION_VERIFY_SLICE | ACTION_WRITE_SLICE,
    ACTIONS_ALL    = ACTIONS_FILES | ACTIONS_SLICE,
  };
  int allowedActions = ACTIONS_ALL;

  enum { OPT_BC = 'A',
         OPT_BS, OPT_DATA, OPT_DEVICE, OPT_DIR, OPT_DIRECT, OPT_FC,
         OPT_FILE, OPT_FSYNC, OPT_HELP, OPT_OFFSET, OPT_SYNC, OPT_ZERO };
  struct option options[] = {
    { "blockCount", required_argument, NULL, OPT_BC },
    { "blockSize",  required_argument, NULL, OPT_BS },
    { "data",       required_argument, NULL, OPT_DATA },
    { "device",     required_argument, NULL, OPT_DEVICE },
    { "dir",        required_argument, NULL, OPT_DIR },
    { "direct",     no_argument,       NULL, OPT_DIRECT },
    { "file",       required_argument, NULL, OPT_FILE },
    { "fileCount",  required_argument, NULL, OPT_FC },
    { "fsync",      no_argument,       NULL, OPT_FSYNC },
    { "help",       no_argument,       NULL, OPT_HELP },
    { "offset",     required_argument, NULL, OPT_OFFSET },
    { "sync",       no_argument,       NULL, OPT_SYNC },
    { "zero",       no_argument,       NULL, OPT_ZERO },
    { NULL,         0,                 NULL, 0 },
  };
  int opt;
  while ((opt = getopt_long_only(argc, argv, "", options, NULL)) != -1) {
    switch (opt) {
    case OPT_BC:     // --blockCount=<count>
      ds.blockCount = intConvert(optarg);
      break;
    case OPT_BS:     // --blockSize=<block_size>
      ds.blockSize = intConvert(optarg);
      break;
    case OPT_DATA :  // --data=<tag>,<fraction>,<fraction>
      pushDataStream(&pds, makeBlockStream(optarg));
      break;
    case OPT_DEVICE: // --device=<path>
      ds.path = optarg;
      allowedActions &= ACTIONS_SLICE;
      break;
    case OPT_DIR:    // --dir=<path>
      ds.dirPath = optarg;
      allowedActions &= ACTIONS_FILES;
      break;
    case OPT_DIRECT: // --direct
      ds.direct = true;
      break;
    case OPT_FC:     // --fileCount=<count>
      ds.fileCount = intConvert(optarg);
      allowedActions &= ACTIONS_FILES;
      break;
    case OPT_FILE:   // --file=<path>
      ds.path  = optarg;
      ds.creat = true;
      allowedActions &= ACTIONS_SLICE;
      break;
    case OPT_FSYNC:  // --fsync
      ds.fsync = true;
      break;
    case OPT_HELP:   // --help
      usage(1);
      break;
    case OPT_OFFSET: // --offset=<offset>
      ds.offset = intConvert(optarg);
      if (ds.offset < 0) {
        errx(2, "the offset (%d) is invalid", ds.offset);
      }
      allowedActions &= ACTIONS_SLICE;
      break;
    case OPT_SYNC:   // --sync
      ds.sync = true;
      break;
    case OPT_ZERO:   // --zero
      pushDataStream(&pds, makeZeroStream());
      allowedActions &= ACTION_VERIFY_SLICE;
      break;
    default:
      usage(0);
      break;
    }
  }

  if (pds == NULL) {
    errx(2, "a data stream must be provided");
  }

  int status = 0;
  bool done = false;
  for (; optind < argc; optind++) {
    const char *arg = argv[optind];
    if ((allowedActions & ACTION_VERIFY_FILES)
        && (strcmp(arg, "verifyFiles") == 0)) {
      status = verifyFiles(&ds, pds);
      done = true;
    } else if ((allowedActions & ACTION_VERIFY_SLICE)
               && (strcmp(arg, "verifySlice") == 0)) {
      status = verifySlice(&ds, pds);
      done = true;
    } else if ((allowedActions & ACTION_WRITE_FILES)
               && (strcmp(arg, "writeFiles") == 0)) {
      status = writeFiles(&ds, pds);
      done = true;
    } else if ((allowedActions & ACTION_WRITE_SLICE)
               && (strcmp(arg, "writeSlice") == 0)) {
      status = writeSlice(&ds, pds);
      done = true;
    }
  }
  if (!done) {
    errx(2, "No action specified");
  }
  reportDataStreams(pds);
  freeDataStreams(pds);
  return status;
}
