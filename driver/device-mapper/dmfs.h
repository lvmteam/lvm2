#ifndef LINUX_DMFS_H
#define LINUX_DMFS_H

struct dmfs_i {
        struct semaphore sem;
        struct dm_table *table;
        struct mapped_device *md;
        struct dentry *dentry;
        struct list_head errors;
};

#define DMFS_I(inode) ((struct dmfs_i *)(inode)->u.generic_ip)


extern int dmfs_init(void) __init;
extern int dmfs_exit(void) __exit;

struct inode *dmfs_new_inode(struct super_block *sb, int mode);
struct inode *dmfs_new_private_inode(struct super_block *sb, int mode);

void dmfs_add_error(struct inode *inode, unsigned num, char *str);
void dmfs_zap_errors(struct inode *inode);



#endif /* LINUX_DMFS_H */
