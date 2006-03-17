#include <inc/error.h>
#include <lib/kdprintf.h>
#include <lib/vector.h>
#include <lib/string.h>
#include <lib/stdio.h>
#include <lib/assert.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>

#include <kfs/feature.h>
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
sb2cfs(struct super_block * sb)
{
	return ((mount_desc_t *) sb->s_fs_info)->cfs;
}

static CFS_t *
dentry2cfs(struct dentry * dentry)
{
	return sb2cfs(dentry->d_sb);
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

static uint32_t
data_size(struct inode * inode)
{
	
	uint32_t filesize_size;
	union {
		int32_t * filesize;
		void * ptr;
	} filesize;
	uint32_t size;
	int r;

	r = CALL(sb2cfs(inode->i_sb), get_metadata, inode->i_ino, KFS_feature_size.id, &filesize_size, &filesize.ptr);
	if (r < 0)
	{
		Dprintf("%s: CALL(get_metadata, cfs_ino = %lu) = %d\n", __FUNCTION__, inode->i_ino, r);
		return 0;
	}
	size = *filesize.filesize;
	free(filesize.filesize);
	return size;
}

static void
serve_read_inode(struct inode * inode)
{
	uint32_t type_size;
	union {
		uint32_t * type;
		void * ptr;
	} type;
	int nlinks_supported = 0, perms_supported = 0;
	int r;
	
	// TODO: it seems this function can be called from another kernel_serve function (which holds kfsd_lock)
	// or by the kernel (in which case this function needs to lock kfsd_lock).
	// This needs to be taken care of!

	r = CALL(sb2cfs(inode->i_sb), get_metadata, inode->i_ino, KFS_feature_filetype.id, &type_size, &type.ptr);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "%s: CALL(get_metadata, ino = %u) = %d\n", __FUNCTION__, inode->i_ino, r);
		return;
	}

	if (nlinks_supported)
	{
		// TODO
		kdprintf(STDERR_FILENO, "%s: add nlinks support\n", __FUNCTION__);
		nlinks_supported = 0;
	}

	if (perms_supported)
	{
		// TODO
		kdprintf(STDERR_FILENO, "%s: add permission support\n", __FUNCTION__);
		perms_supported = 0;
	}

	if (*type.type == TYPE_DIR)
	{
		if (!nlinks_supported)
			inode->i_nlink = 2;
		if (!perms_supported)
			inode->i_mode = 0777; // default, in case permissions are not supported
		inode->i_mode |= S_IFDIR;
		inode->i_op = &kfs_dir_inode_ops;
		inode->i_fop = &kfs_dir_file_ops;
	}
	else if (*type.type == TYPE_FILE || *type.type == TYPE_DEVICE)
	{
		if (!nlinks_supported)
			inode->i_nlink = 1;
		if (!perms_supported)
			inode->i_mode = 0666; // default, in case permissions are not supported
		inode->i_mode |= S_IFREG;
		inode->i_op = &kfs_reg_inode_ops;
		inode->i_fop = &kfs_reg_file_ops;
	}
	else if (*type.type == TYPE_INVAL)
	{
		kdprintf(STDERR_FILENO, "%s: inode %u has type invalid\n", __FUNCTION__, inode->i_ino);
		goto exit;
	}
	else
	{
		kdprintf(STDERR_FILENO, "%s: inode %u has unsupported type\n", __FUNCTION__, inode->i_ino);
		goto exit;
	}

	inode->i_size = data_size(inode);

	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_mtime = CURRENT_TIME;
	inode->i_atime = CURRENT_TIME;
	inode->i_ctime = CURRENT_TIME;

  exit:
	free(type.type);
	return;
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

	k_root = new_inode(sb);
	if (!k_root)
	{
		sb->s_dev = 0;
		return -E_NO_MEM;
	}
	k_root->i_ino = cfs_root;
	serve_read_inode(k_root);

	sb->s_root = d_alloc_root(k_root);
	if (!sb->s_root)
	{
		iput(k_root);
		sb->s_dev = 0;
		return -E_NO_MEM;
	}
	return 0;
}

static struct super_block *
serve_get_sb(struct file_system_type * fs_type, int flags, const char * dev_name, void * data)
{
	Dprintf("%s()\n", __FUNCTION__);
	int i, size;
	if (strncmp(dev_name, "kfs:", 4))
		return ERR_PTR(-E_INVAL);
	
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
				return ERR_PTR(-E_BUSY);
			}
			if (modman_inc_cfs(m->cfs, fs_type, m->path) < 0)
			{
				spin_unlock(kfsd_lock);
				return ERR_PTR(-E_NO_MEM);
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
	return ERR_PTR(-E_NO_DEV);
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

	spin_lock(kfsd_lock);
	r = CALL(dentry2cfs(filp->f_dentry), open, filp->f_dentry->d_inode->i_ino, 0, &fdesc);
	fdesc->common->parent = filp->f_dentry->d_parent->d_inode->i_ino;
	spin_unlock(kfsd_lock);
	if (r < 0)
		return r;
	filp->private_data = fdesc;
	return 0;
}

static int
serve_release(struct inode * inode, struct file * filp)
{
	Dprintf("%s(name = \"%s\", fdesc = %p)\n", __FUNCTION__, filp->f_dentry->d_name.name, file2fdesc(filp));
	int r;
	spin_lock(kfsd_lock);
	r = CALL(dentry2cfs(filp->f_dentry), close, file2fdesc(filp));
	spin_unlock(kfsd_lock);
	return r;
}

static struct dentry *
serve_dir_lookup(struct inode * dir, struct dentry * dentry, struct nameidata * ignore)
{
	inode_t cfs_ino;
	ino_t k_ino;
	struct inode * inode = NULL;
	int r;

	Dprintf("%s(dentry = \"%s\") (pid = %d)\n", __FUNCTION__, dentry->d_name.name, current->pid);

	spin_lock(kfsd_lock);
	assert(dentry2cfs(dentry));
	r = CALL(dentry2cfs(dentry), lookup, dir->i_ino, dentry->d_name.name, &cfs_ino);
	if (r == -E_NOT_FOUND)
		cfs_ino = 0;
	else if (r < 0)
	{
		spin_unlock(kfsd_lock);
		return ERR_PTR(r);
	}
	k_ino = cfs_ino;
	spin_unlock(kfsd_lock); // TODO: do we need to hold the lock for iget() et al, too?

	if (k_ino)
	{
		inode = iget(dir->i_sb, k_ino); 
		if (!inode)
			return ERR_PTR(-E_PERM);
	}
	if (inode)
	{
		struct dentry * d = d_splice_alias(inode, dentry);
		if (d)
			d->d_op = &kfs_dentry_ops;
		return d;
	}
	/* add a negative dentry */
	d_add(dentry, inode);
	return NULL;
}

static int
serve_notify_change(struct dentry * dentry, struct iattr * attr)
{
	Dprintf("%s(%s)\n", __FUNCTION__, dentry->d_name.name);
#if 0
	struct inode * inode = dentry->d_inode;
	if (attr->ia_valid & ATTR_SIZE)
	{
		if(is_directory)
			return -EPERM; /* operation not permitted */
		change_size(new_size);
		if(failed)
			return error;
	}
	inode_change_ok(inode, attr);
	if(failed)
		return error;
	inode_setattr(inode, attr);
	if(failed)
		return error;
	return 0;
#endif
	return -1;
}

static ssize_t
serve_read(struct file * filp, char __user * buffer, size_t count, loff_t * f_pos)
{
	Dprintf("%s(%s, %d, %d)\n", __FUNCTION__, filp->f_dentry->d_name.name, count, (int) *f_pos);
	fdesc_t * fdesc = file2fdesc(filp);
	CFS_t * cfs = dentry2cfs(filp->f_dentry);
	/* pick a reasonably big, but not too big, maximum size we will allocate
	 * on behalf of a requesting user process... TODO: use it repeatedly? */
	size_t data_size = (count > 65536) ? 65536 : count;
	char * data = vmalloc(data_size);
	uint32_t offset = *f_pos;
	ssize_t r = 0;
	unsigned long bytes;
	
	if (!data)
		return -E_NO_MEM;
	
	spin_lock(kfsd_lock);
	r = CALL(cfs, read, fdesc, data, offset, data_size);
	spin_unlock(kfsd_lock);
	
	/* CFS gives us an "error" when we hit EOF */
	if (r == -E_EOF)
		r = 0;
	else if (r < 0)
		goto out;
	
	bytes = copy_to_user(buffer, data, r);
	if (bytes)
	{
		if (r == bytes)
		{
			r = -E_FAULT;
			goto out;
		}
		r -= bytes;
	}
	
	*f_pos += r;
	
out:
	vfree(data);
	return r;
}

static ssize_t
serve_write(struct file * filp, const char __user * buffer, size_t count, loff_t * f_pos)
{
	Dprintf("%s(%s, %d, %d)\n", __FUNCTION__, filp->f_dentry->d_name.name, count, (int) *f_pos);
	fdesc_t * fdesc = file2fdesc(filp);
	CFS_t * cfs = dentry2cfs(filp->f_dentry);
	/* pick a reasonably big, but not too big, maximum size we will allocate
	 * on behalf of a requesting user process... TODO: use it repeatedly? */
	size_t data_size = (count > 65536) ? 65536 : count;
	char * data = vmalloc(data_size);
	uint32_t offset = *f_pos;
	ssize_t r = 0;
	unsigned long bytes;
	
	if (!data)
		return -E_NO_MEM;
	
	bytes = copy_from_user(data, buffer, data_size);
	if (bytes)
	{
		if (data_size == bytes)
		{
			r = -E_FAULT;
			goto out;
		}
		data_size -= bytes;
	}
	
	spin_lock(kfsd_lock);
	r = CALL(cfs, write, fdesc, data, offset, data_size);
	spin_unlock(kfsd_lock);
	
	if (r < 0)
		goto out;
	
	*f_pos += r;
	
out:
	vfree(data);
	return r;
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

#define RECLEN_MIN_SIZE (sizeof(((dirent_t *) NULL)->d_reclen) + (int) &((dirent_t *) NULL)->d_reclen)

static int
serve_dir_readdir(struct file * filp, void * k_dirent, filldir_t filldir)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r;

	spin_lock(kfsd_lock);
	while (1)
	{
		uint32_t cfs_fpos = filp->f_pos;
		char buf[sizeof(dirent_t)];
		char * cur = buf;
		uint32_t k_nbytes = 0;

		r = CALL(dentry2cfs(filp->f_dentry), getdirentries, file2fdesc(filp), buf, sizeof(buf), &cfs_fpos);
		if (r < 0)
			break;

		/* make sure there is a reclen to read, and make sure it doesn't say to go to far  */
		while ((cur - buf) + RECLEN_MIN_SIZE <= r && (cur - buf) + ((dirent_t *) cur)->d_reclen <= r)
		{
			dirent_t * cfs_dirent = (dirent_t *) cur;
			int s;
			Dprintf("%s: \"%s\"\n", __FUNCTION__, cfs_dirent->d_name);
			// FIXME?: must fpos be a real file position?
			s = filldir(k_dirent, cfs_dirent->d_name, cfs_dirent->d_namelen, 0, cfs_dirent->d_fileno, cfs_dirent->d_type);
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
	spin_unlock(kfsd_lock);

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
	.open = serve_open,
	.release = serve_release,
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
	.read_inode = serve_read_inode
};
