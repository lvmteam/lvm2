/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

struct label
{
    uint32_t magic;
    uint32_t format_type;
    uint32_t checksum;
    uint16_t datalen;
    char    *data;
    void    *pool;             /* Pool that data is allocated from */
};



extern int label_write(struct device *dev, struct label *label);
extern int label_read(struct device *dev, struct label *label);
extern int is_labelled(struct device *dev);

extern struct dev_filter *label_filter_create();
extern struct dev_filter *label_format_filter_create(uint32_t format_type);
