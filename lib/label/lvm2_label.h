/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

struct lvm2_label
{
    uint32_t magic;
    uint32_t crc;
    uint64_t label1_loc;
    uint64_t label2_loc;
    uint16_t datalen;

    char     disk_type[32];
    uint32_t version[3];

    char    *data;
};

#define VERSION_MATCH_EQUAL     1
#define VERSION_MATCH_LESSTHAN  2
#define VERSION_MATCH_LESSEQUAL 3
#define VERSION_MATCH_ANY       4

extern struct dev_filter *lvm2_label_filter_create();
extern struct dev_filter *lvm2_label_format_filter_create(char *disk_type, uint32_t version[3], int match_type);
