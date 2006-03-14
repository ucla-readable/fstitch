#include <inc/error.h>
#include <lib/kdprintf.h>
#include <linux/module.h>
#include <linux/fs.h>

#include <kfs/kfsd.h>
#include <kfs/kernel_serve.h>

#define KERNEL_SERVE_DEBUG 0

#if KERNEL_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

static struct file_system_type  kfs_fs_type;
static struct inode_operations  kfs_reg_inode_ops;
static struct file_operations   kfs_reg_file_ops;
static struct inode_operations  kfs_dir_inode_ops;
static struct file_operations   kfs_dir_file_ops;
static struct dentry_operations kfs_dentry_ops;
static struct super_operations  kfs_superblock_ops;


int kernel_serve_add_mount(const char * path, CFS_t * cfs)
{
	Dprintf("%s(path = \"%s\", cfs = %s)\n", __FUNCTION__, path, modman_name_cfs(cfs));
	if (!path || !cfs)
		return -E_INVAL;
	return -E_UNSPECIFIED;
}

static void kernel_serve_shutdown(void * ignore)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r = unregister_filesystem(&kfs_fs_type);
	if (r < 0)
		kdprintf(STDERR_FILENO, "kernel_serve_shutdown(): unregister_filesystem: %d\n", r);
}

int kernel_serve_init(spinlock_t * kfsd_lock)
{
	int r = kfsd_register_shutdown_module(kernel_serve_shutdown, NULL);
	if (r < 0)
		return r;
	return register_filesystem(&kfs_fs_type);
}


//
// Linux VFS function implementations

static struct super_block *
serve_get_sb(struct file_system_type * fs_type, int flags, const char * dev_name, void *data)
{
	Dprintf("%s()\n", __FUNCTION__);
	return NULL;
}

static struct dentry *
serve_dir_lookup(struct inode * dir, struct dentry * dentry, struct nameidata * ignore)
{
	Dprintf("%s()\n", __FUNCTION__);
	return NULL;
}

static int
serve_notify_change(struct dentry * dentry, struct iattr * attr)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -1;
}

static ssize_t
serve_read(struct file * filp, char __user * buffer, size_t count, loff_t * f_pos)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -1;
}

static ssize_t
serve_write(struct file * filp, const char __user * buffer, size_t count, loff_t * f_pos)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -1;
}

static int
serve_unlink(struct inode * dir, struct dentry * dentry)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -1;
}

static int
serve_create(struct inode * dir, struct dentry * dentry, int mode, struct nameidata * nd)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -1;
}

static int
serve_dir_readdir(struct file * filp, void * dirent, filldir_t filldir)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -1;
}

static int
serve_delete_dentry(struct dentry * dentry)
{
	Dprintf("%s()\n", __FUNCTION__);
	return -1;
}


//
// Linux VFS struct definitions

static struct file_system_type kfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "kfs",
	.get_sb = serve_get_sb,
	.kill_sb = kill_anon_super
};

static struct inode_operations kfs_reg_inode_ops = {
	.setattr = serve_notify_change
};

static struct file_operations kfs_reg_file_ops = {
	.llseek = generic_file_llseek,
	.read = serve_read,
	.write = serve_write
};

static struct inode_operations kfs_dir_inode_ops = {
	.lookup	= serve_dir_lookup,
	.unlink	= serve_unlink,
	.create	= serve_create
};

static struct file_operations kfs_dir_file_ops = {
	.read = generic_read_dir,
	.readdir = serve_dir_readdir
};

static struct dentry_operations kfs_dentry_ops = {
	.d_delete = serve_delete_dentry
};

static struct super_operations kfs_superblock_ops = {
};
