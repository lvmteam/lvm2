/*
 * Copyright (C) 2001 Sistina Software
 *
 * This file is released under the GPL.
 *
 * This is the in core representation of a volume group and it's
 * associated physical and logical volumes.
 */

#ifndef METADATA_H
#define METADATA_H

#define ID_LEN 32

struct id {
	uint8_t chars[ID_LEN];
};

struct logical_volume;

struct physical_volume {
        struct id *id;
	struct device *dev;
	char *vg_name;

        uint32_t status;
        uint64_t size;

        /* physical extents */
        uint64_t pe_size;
        uint64_t pe_start;
        uint32_t pe_count;
        uint32_t pe_allocated;
};

struct pe_specifier {
        struct physical_volume *pv;
        uint32_t pe;
};

struct logical_volume {
        /* disk */
	struct id *id;
        char *name;

        uint32_t access;
        uint32_t status;
        uint32_t open;

        uint64_t size;
        uint32_t le_count;

        /* le -> pe mapping array */
        struct pe_specifier *map;
};

struct volume_group {
	struct id *id;
	char *name;

        uint64_t extent_size;
        uint32_t extent_count;
        uint32_t free_count;

        /* physical volumes */
        uint32_t pv_count;
        struct physical_volume **pv;

        /* logical volumes */
        uint32_t lv_count;
        struct logical_volume **lv;
};

/* ownership of returned objects passes */
struct io_space {
	struct str_list *(*get_vgs)(struct io_space *is);
	struct dev_list *(*get_pvs)(struct io_space *is);

	struct physical_volume *read_pv(struct io_space *is,
					struct device *dev);
	int write_pv(struct io_space *is, struct physical_volume *pv);

	struct volume_group *(*read_vg)(struct io_space *is,
					const char *vg_name);
	int (*write_vg)(struct io_space *is, struct volume_group *vg);
	void (*destructor)(struct io_space *is);

	struct device_manager *mgr;
	void *private;
};

struct io_space *create_text_format(struct device_manager *mgr,
				    const char *text_file);
struct io_space *create_lvm1_format(struct device_manager *mgr);

inline struct volume_group *read_vg(struct io_space *f)
{
	struct dev_list *pvs = f->get_pvs();
	return f->read_vg(pvs);
}

inline int write_vg(struct io_object *f, struct volume_group *vg)
{
	return f->write_vg(vg);
}



inline int write_backup(struct io_format *orig, struct io_format *text)
{

}



int id_eq(struct id *op1, struct id *op2);

struct volume_group *create_vg();
int destroy_vg(struct volume_group *vg);

int add_pv(struct volume_group *vg, struct physical_volume *pv);
struct physical_volume *find_pv(struct volume_group *vg,
				struct physical_volume *pv);

int add_lv(struct volume_group *vg, struct logical_volume *lv);
struct logical_volume *find_lv(struct volume_group *vg,
			       struct logical_volume *lv);

struct io_handler {
	struct volume_group *read_vg();
	int write_vg(struct volume_group *vg);
};

#endif
