#include <inc/error.h>
#include <lib/kdprintf.h>
#include <lib/vector.h>
#include <lib/string.h>
#include <lib/stdio.h>
#include <lib/assert.h>
#include <linux/module.h>
#include <linux/fs.h>

#include <kfs/kfsd.h>
#include <kfs/modman.h>
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

static spinlock_t * kfsd_lock = NULL;

struct mount_desc {
	const char * path;
	CFS_t * cfs;
	int mounted;
};
typedef struct mount_desc mount_desc_t;

static vector_t * mounts = NULL;

static mount_desc_t * mount_desc_create(const char * path, CFS_t * cfs)
{
	mount_desc_t * m = kmalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return NULL;
	m->path = strdup(path);
	if (!m->path)
	{
		kfree(m);
		return NULL;
	}
	m->cfs = cfs;
	m->mounted = 0;
	return m;
}

static void mount_desc_destroy(mount_desc_t * m)
{
	free(m->path);
	kfree(m);
}

int kernel_serve_add_mount(const char * path, CFS_t * cfs)
{
	Dprintf("%s(path = \"%s\", cfs = %s)\n", __FUNCTION__, path, modman_name_cfs(cfs));
	mount_desc_t * m;
	int r;
	if (!path || !cfs)
		return -E_INVAL;
	/* TODO: make sure there is no mount at this path already */
	m = mount_desc_create(path, cfs);
	if (!m)
		return -E_NO_MEM;
	r = vector_push_back(mounts, m);
	if (r < 0)
	{
		mount_desc_destroy(m);
		return r;
	}
	printk("kkfsd: made \"kfs:%s\" available for mounting\n", path);
	return 0;
}

static void kernel_serve_shutdown(void * ignore)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r = unregister_filesystem(&kfs_fs_type);
	if (r < 0)
		kdprintf(STDERR_FILENO, "kernel_serve_shutdown(): unregister_filesystem: %d\n", r);
}

int kernel_serve_init(spinlock_t * lock)
{
	int r;
	mounts = vector_create();
	if (!mounts)
		return -E_NO_MEM;
	r = kfsd_register_shutdown_module(kernel_serve_shutdown, NULL);
	if (r < 0)
	{
		vector_destroy(mounts);
		mounts = NULL;
		return r;
	}
	kfsd_lock = lock;
	return register_filesystem(&kfs_fs_type);
}


//
// Linux VFS function implementations

static CFS_t *
dentry2cfs(struct dentry * dentry)
{
	return ((mount_desc_t *) dentry->d_sb->s_fs_info)->cfs;
}

static fdesc_t * file2fdesc(struct file * filp)
{
	return (fdesc_t *) filp->private_data;
}


/* Looking at the NFS file system implementation was very helpful for some of these functions. */

static int
serve_set_super(struct super_block * sb, void * data)
{
	sb->s_fs_info = data;
	return set_anon_super(sb, data);
}

static int
serve_compare_super(struct super_block * sb, void * data)
{
	mount_desc_t * m = data;
	mount_desc_t * old = sb->s_fs_info;
	if(strcmp(old->path, m->path))
		return 0;
	if(old->cfs != m->cfs)
		return 0;
	return 1;
}

static struct inode *
serve_mk_linux_inode(struct super_block * sb, inode_t i)
{
	struct inode * inode;
	/* FIXME */
	printk("holy crap this is half-assed! please fix me!\n");
	inode = new_inode(sb);
	if (!inode)
		return NULL;
	inode->i_ino = i;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_size = 4096;
	inode->i_mode = 0755 | S_IFDIR;
	inode->i_op = &kfs_dir_inode_ops;
	inode->i_fop = &kfs_dir_file_ops;
	inode->i_nlink = 2;
	inode->i_mtime = CURRENT_TIME;
	inode->i_atime = CURRENT_TIME;
	inode->i_ctime = CURRENT_TIME;
	return inode;
}

static int
serve_fill_super(struct super_block * sb, mount_desc_t * m)
{
	inode_t cfs_root;
	struct inode * k_root;
	int r;
	
	/* FIXME? */
	sb->s_blocksize = 0;
	sb->s_blocksize_bits = 0;
	sb->s_magic = 0x88F50CF5;
	sb->s_op = &kfs_superblock_ops;
	
	r = CALL(m->cfs, get_root, &cfs_root);
	assert(r >= 0);
	k_root = serve_mk_linux_inode(sb, cfs_root);
	if (!k_root)
	{
		sb->s_dev = 0;
		return -ENOMEM;
	}
	sb->s_root = d_alloc_root(k_root);
	if (!sb->s_root)
	{
		iput(k_root);
		sb->s_dev = 0;
		return -ENOMEM;
	}
	return 0;
}

static struct super_block *
serve_get_sb(struct file_system_type * fs_type, int flags, const char * dev_name, void * data)
{
	Dprintf("%s()\n", __FUNCTION__);
	int i, size;
	if (strncmp(dev_name, "kfs:", 4))
		return ERR_PTR(-EINVAL);
	
	spin_lock(kfsd_lock);
	size = vector_size(mounts);
	for (i = 0; i < size; i++)
	{
		mount_desc_t * m = vector_elt(mounts, i);
		if (!strcmp(m->path, &dev_name[4]))
		{
			struct super_block * sb;
			if (m->mounted)
			{
				spin_unlock(kfsd_lock);
				return ERR_PTR(-EBUSY);
			}
			if (modman_inc_cfs(m->cfs, fs_type, m->path) < 0)
			{
				spin_unlock(kfsd_lock);
				return ERR_PTR(-ENOMEM);
			}
			sb = sget(fs_type, serve_compare_super, serve_set_super, m);
			if (IS_ERR(sb) || sb->s_root) /* sb->s_root means it is mounted already? */
			{
				modman_dec_cfs(m->cfs, fs_type);
				spin_unlock(kfsd_lock);
				return sb;
			}
			sb->s_flags = flags;
			i = serve_fill_super(sb, m);
			if (i < 0)
			{
				modman_dec_cfs(m->cfs, fs_type);
				up_write(&sb->s_umount);
				deactivate_super(sb);
				spin_unlock(kfsd_lock);
				return ERR_PTR(i);
			}
			m->mounted = 1;
			sb->s_flags |= MS_ACTIVE;
			spin_unlock(kfsd_lock);
			printk("kkfsd: mounted \"kfs:%s\"\n", m->path);
			return sb;
		}
	}
	spin_unlock(kfsd_lock);
	return ERR_PTR(-ENODEV);
}

static void
serve_kill_sb(struct super_block * sb)
{
	Dprintf("%s()\n", __FUNCTION__);
	mount_desc_t * m = sb->s_fs_info;
	modman_dec_cfs(m->cfs, sb->s_type);
	m->mounted = 0;
	kill_anon_super(sb);
}

static int
serve_open(struct inode * inode, struct file * filp)
{
	fdesc_t * fdesc;
	int r;
	Dprintf("%s(\"%s\")\n", __FUNCTION__, filp->f_dentry->d_name.name);

	r = generic_file_open(inode, filp);
	if (r < 0)
		return r;

	r = CALL(dentry2cfs(filp->f_dentry), open, filp->f_dentry->d_inode->i_ino, 0, &fdesc);
	if (r < 0)
		return -EPERM; // TODO: r could be other failures
	filp->private_data = fdesc;
	return 0;
}

static int
serve_release(struct inode * inode, struct file * filp)
{
	Dprintf("%s(name = \"%s\", fdesc = %p)\n", __FUNCTION__, filp->f_dentry->d_name.name, file2fdesc(filp));
	return CALL(dentry2cfs(filp->f_dentry), close, file2fdesc(filp));
}

static struct dentry *
serve_dir_lookup(struct inode * dir, struct dentry * dentry, struct nameidata * ignore)
{
	// Under knoppix there is some program that looks up "auto" with a
	// very short period, making debug output much more difficult to read.
	// Thus don't Dprintf() for "auto" lookups.
	if (strcmp(dentry->d_name.name, "auto"))
		Dprintf("%s(dentry = \"%s\")\n", __FUNCTION__, dentry->d_name.name);
	return ERR_PTR(-EPERM);
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
linux_filetype(uint8_t cfs_filetype)
{
	switch (cfs_filetype)
	{
		case TYPE_FILE:
			return DT_REG;
		case TYPE_DIR:
			return DT_DIR;
		case TYPE_SYMLINK:
			return DT_LNK;
		case TYPE_DEVICE:
			return DT_REG;
		default:
			assert(0);
			return DT_REG;
	}
}

static int
serve_dir_readdir(struct file * filp, void * k_dirent, filldir_t filldir)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r;

	while (1)
	{
		uint32_t cfs_fpos = filp->f_pos;
		char buf[sizeof(dirent_t)];
		char * cur = buf;
		uint32_t k_nbytes = 0;

		r = CALL(dentry2cfs(filp->f_dentry), getdirentries, file2fdesc(filp), buf, sizeof(buf), &cfs_fpos);
		if (r < 0)
			break;

		while ((cur - buf) + ((dirent_t *) cur)->d_reclen < r)
		{
			dirent_t * cfs_dirent = (dirent_t *) cur;
			int s;
			Dprintf("%s: \"%s\"\n", __FUNCTION__, cfs_dirent->d_name);
			// FIXME?: must fpos be a real file position?
			s = filldir(k_dirent, cfs_dirent->d_name, cfs_dirent->d_namelen, 0, cfs_dirent->d_fileno, linux_filetype(cfs_dirent->d_type));
			if (s < 0)
			{
				cfs_fpos = filp->f_pos;
				r = CALL(dentry2cfs(filp->f_dentry), getdirentries, file2fdesc(filp), buf, k_nbytes, &cfs_fpos);
				assert(r >= 0);
				break;
			}
			cur += cfs_dirent->d_reclen;
			k_nbytes += cfs_dirent->d_reclen;
		}
		filp->f_pos = cfs_fpos;
	}

	if (r == -E_UNSPECIFIED)
		return 1;
	return 0;
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
	.kill_sb = serve_kill_sb
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
	.open = serve_open,
	.release = serve_release,
	.read = generic_read_dir,
	.readdir = serve_dir_readdir
};

static struct dentry_operations kfs_dentry_ops = {
	.d_delete = serve_delete_dentry
};

static struct super_operations kfs_superblock_ops = {
};
