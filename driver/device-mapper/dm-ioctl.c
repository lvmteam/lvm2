/*
 * Copyright (C) 2001 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include <linux/fs.h>

#include "dm.h"
#include <linux/dm-ioctl.h>

static void free_params(struct dm_ioctl *p)
{
	vfree(p);
}

static int copy_params(struct dm_ioctl *user, struct dm_ioctl **result)
{
	struct dm_ioctl tmp, *dmi;

	if (copy_from_user(&tmp, user, sizeof(tmp)))
		return -EFAULT;

	if (!(dmi = vmalloc(tmp.data_size)))
		return -ENOMEM;

	if (copy_from_user(dmi, user, tmp.data_size))
		return -EFAULT;

	*result = dmi;
	return 0;
}

/*
 * check a string doesn't overrun the chunk of
 * memory we copied from userland.
 */
static int valid_str(char *str, void *end)
{
	while (((void *) str < end) && *str)
		str++;

	return *str ? 0 : 1;
}

static int first_target(struct dm_ioctl *a, void *end,
			struct dm_target_spec **spec, char **params)
{
	*spec = (struct dm_target_spec *) (a + 1);
	*params = (char *) (*spec + 1);

	return valid_str(*params, end);
}

static int next_target(struct dm_target_spec *last, void *end,
		       struct dm_target_spec **spec, char **params)
{
	*spec = (struct dm_target_spec *)
		(((unsigned char *) last) + last->next);
	*params = (char *) (*spec + 1);

	return valid_str(*params, end);
}

void err_fn(const char *message, void *private)
{
	printk(KERN_WARNING "%s\n", message);
}

/*
 * Checks to see if there's a gap in the table.
 * Returns true iff there is a gap.
 */
static int gap(struct dm_table *table, struct dm_target_spec *spec)
{
	if (!table->num_targets)
		return (spec->sector_start > 0) ? 1 : 0;

	if (spec->sector_start != table->highs[table->num_targets - 1] + 1)
		return 1;

	return 0;
}

static int populate_table(struct dm_table *table, struct dm_ioctl *args)
{
	int i = 0, r, first = 1;
	struct dm_target_spec *spec;
	char *params;
	struct target_type *ttype;
	void *context, *end;
	offset_t high = 0;

	if (!args->target_count) {
		WARN("No targets specified");
		return -EINVAL;
	}

	end = ((void *) args) + args->data_size;

#define PARSE_ERROR(msg) {err_fn(msg, NULL); return -EINVAL;}

	for (i = 0; i < args->target_count; i++) {

		r = first ? first_target(args, end, &spec, &params) :
			next_target(spec, end, &spec, &params);

		if (!r)
			PARSE_ERROR("unable to find target");

		/* lookup the target type */
		if (!(ttype = dm_get_target_type(spec->target_type)))
			PARSE_ERROR("unable to find target type");

		if (gap(table, spec))
			PARSE_ERROR("gap in target ranges");

		/* build the target */
		if (ttype->ctr(table, spec->sector_start, spec->length, params,
			       &context))
			PARSE_ERROR(context);

		/* add the target to the table */
		high = spec->sector_start + (spec->length - 1);
		if (dm_table_add_target(table, high, ttype, context))
			PARSE_ERROR("internal error adding target to table");

		first = 0;
	}

#undef PARSE_ERROR

	r = dm_table_complete(table);
	return r;
}

/*
 * Copies device info back to user space, used by
 * the create and info ioctls.
 */
static int info(const char *name, struct dm_ioctl *user)
{
	struct dm_ioctl param;
	struct mapped_device *md = dm_get(name);

	if (!md) {
		param.exists = 0;
		goto out;
	}

	param.data_size = 0;
	strncpy(param.name, md->name, sizeof(param.name));
	param.exists = 1;
	param.suspend = md->suspended;
	param.open_count = md->use_count;
	param.major = MAJOR(md->dev);
	param.minor = MINOR(md->dev);
	param.target_count = md->map->num_targets;

 out:
	return copy_to_user(user, &param, sizeof(param));
}

static int create(struct dm_ioctl *param, struct dm_ioctl *user)
{
	int r;
	struct mapped_device *md;
	struct dm_table *t;

	t = dm_table_create();
	r = PTR_ERR(t);
	if (IS_ERR(t))
		goto bad;

	if ((r = populate_table(t, param)))
		goto bad;

	md = dm_create(param->name, param->minor, t);
	r = PTR_ERR(md);
	if (IS_ERR(md))
		goto bad;

	if ((r = info(param->name, user))) {
		dm_destroy(md);
		goto bad;
	}

	return 0;

 bad:
	dm_table_destroy(t);
	return r;
}

static int remove(struct dm_ioctl *param)
{
	struct mapped_device *md = dm_get(param->name);

	if (!md)
		return -ENXIO;

	return dm_destroy(md);
}

static int suspend(struct dm_ioctl *param)
{
	struct mapped_device *md = dm_get(param->name);

	if (!md)
		return -ENXIO;

	return param->suspend ? dm_suspend(md) : dm_resume(md);
}

static int reload(struct dm_ioctl *param)
{
	int r;
	struct mapped_device *md = dm_get(param->name);
	struct dm_table *t;

	if (!md)
		return -ENXIO;

	t = dm_table_create();
	if (IS_ERR(t))
		return PTR_ERR(t);

	if ((r = populate_table(t, param))) {
		dm_table_destroy(t);
		return r;
	}

	if ((r = dm_swap_table(md, t))) {
		dm_table_destroy(t);
		return r;
	}

	return 0;
}

static int ctl_open(struct inode *inode, struct file *file)
{
	/* only root can open this */
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	return 0;
}

static int ctl_close(struct inode *inode, struct file *file)
{
	return 0;
}


static int ctl_ioctl(struct inode *inode, struct file *file,
		     uint command, ulong a)
{
	int r;
	struct dm_ioctl *p;

	if ((r = copy_params((struct dm_ioctl *) a, &p)))
		return r;

	switch (command) {
	case DM_CREATE:
		r = create(p, (struct dm_ioctl *) a);
		break;

	case DM_REMOVE:
		r = remove(p);
		break;

	case DM_SUSPEND:
		r = suspend(p);
		break;

	case DM_RELOAD:
		r = reload(p);
		break;

	case DM_INFO:
		r = info(p->name, (struct dm_ioctl *) a);
		break;

	default:
		WARN("dm_ctl_ioctl: unknown command 0x%x\n", command);
		r = -EINVAL;
	}

	free_params(p);
	return r;
}


static struct file_operations _ctl_fops = {
	open:		ctl_open,
	release:	ctl_close,
	ioctl:		ctl_ioctl,
	owner:		THIS_MODULE,
};


static devfs_handle_t _ctl_handle;

int dm_interface_init(void)
{
	int r;

	if ((r = devfs_register_chrdev(DM_CHAR_MAJOR, DM_DIR,
				       &_ctl_fops)) < 0) {
		WARN("devfs_register_chrdev failed for dm control dev");
		return -EIO;
	}

	_ctl_handle = devfs_register(0 , DM_DIR "/control", 0,
				     DM_CHAR_MAJOR, 0,
				     S_IFCHR | S_IRUSR | S_IWUSR | S_IRGRP,
				     &_ctl_fops, NULL);

	return r;
}

void dm_interface_exit(void)
{
	// FIXME: remove control device

	if (devfs_unregister_chrdev(DM_CHAR_MAJOR, DM_DIR) < 0)
		WARN("devfs_unregister_chrdev failed for dm control device");
}

