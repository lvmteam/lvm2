/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

struct label
{
    uint32_t magic;
    uint32_t crc;
    uint64_t label1_loc;
    uint64_t label2_loc;
    uint16_t datalen;

    char     disk_type[32];
    uint32_t version[3];

    char    *data; /* Should be freed with label_free() */
};

#define VERSION_MATCH_EQUAL     1
#define VERSION_MATCH_LESSTHAN  2
#define VERSION_MATCH_LESSEQUAL 3
#define VERSION_MATCH_ANY       4

extern int  label_write(struct device *dev, struct label *label);
extern int  label_read(struct device *dev, struct label *label);
extern void label_free(struct label *label);
extern int  is_labelled(struct device *dev);
extern int  labels_match(struct device *dev);

extern struct dev_filter *label_filter_create();
extern struct dev_filter *label_format_filter_create(char *disk_type, uint32_t version[3], int match_type);
