#include <inc/error.h>
#include <lib/kdprintf.h>
#include <lib/vector.h>
#include <lib/string.h>
#include <lib/stdio.h>
#include <lib/assert.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/vmalloc.h>
#include <asm/uaccess.h>

#include <kfs/feature.h>
#include <kfs/kfsd.h>
#include <kfs/modman.h>
#include <kfs/sync.h>
#include <kfs/sched.h>
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

int kernel_serve_init(void)
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

static fdesc_t *
file2fdesc(struct file * filp)
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
inode_get_size(struct inode * inode)
{
	uint32_t filesize_size;
	union {
		int32_t * filesize;
		void * ptr;
	} filesize;
	uint32_t size;
	int r;

	assert(kfsd_have_lock());

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
read_inode_withlock(struct inode * inode)
{
	uint32_t type_size;
	union {
		uint32_t * type;
		void * ptr;
	} type;
	int r, nlinks_supported = 0, perms_supported = 0;
	
	assert(kfsd_have_lock());

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
		{
			char buf[1024];
			uint32_t basep = 0;
			fdesc_t * fdesc;
			
			inode->i_nlink = 2;
			
			r = CALL(sb2cfs(inode->i_sb), open, inode->i_ino, 0, &fdesc);
			assert(r >= 0);
			/* HACK: this does not have to be the correct value */
			fdesc->common->parent = inode->i_ino;
			
			while ((r = CALL(sb2cfs(inode->i_sb), getdirentries, fdesc, buf, sizeof(buf), &basep)) > 0)
			{
				char * cur = buf;
				while (cur < buf + r)
				{
					if (((dirent_t *) cur)->d_type == TYPE_DIR)
						inode->i_nlink++;
					cur += ((dirent_t *) cur)->d_reclen;
				}
			}
			
			r = CALL(sb2cfs(inode->i_sb), close, fdesc);
			assert(r >= 0);
		}
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

	inode->i_size = inode_get_size(inode);

	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_mtime = CURRENT_TIME;
	inode->i_atime = CURRENT_TIME;
	inode->i_ctime = CURRENT_TIME;

  exit:
	free(type.type);
	return;
}

static void
serve_read_inode(struct inode * inode)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, inode->i_ino);
	kfsd_enter();
	read_inode_withlock(inode);
	kfsd_leave(1);
}

static int
serve_stat_fs(struct super_block * sb, struct kstatfs * st)
{
	mount_desc_t * m = (mount_desc_t *) sb->s_fs_info;
	CFS_t * cfs = m->cfs;
	size_t size;
	void * data;
	int r;
	
	kfsd_enter();
	r = CALL(cfs, get_metadata, 0, KFS_feature_blocksize.id, &size, &data);
	if (r < 0)
		goto out;
	assert(sizeof(st->f_bsize) >= size);
	st->f_bsize = st->f_frsize = *(uint32_t *) data;
	free(data);
	assert(st->f_bsize != 0);
	
	r = CALL(cfs, get_metadata, 0, KFS_feature_devicesize.id, &size, &data);
	if (r < 0)
		goto out;
	assert(sizeof(st->f_blocks) >= size);
	st->f_blocks = *(uint32_t *) data;
	free(data);
	
	r = CALL(cfs, get_metadata, 0, KFS_feature_freespace.id, &size, &data);
	if (r < 0)
		goto out;
	assert(sizeof(st->f_bfree) >= size);
	/* what is the difference between bfree and bavail? */
	st->f_bfree = st->f_bavail = *(uint32_t *) data;
	free(data);
	
	// TODO - add lfs features for these guys
	st->f_files = 0;
	st->f_ffree = 0;
	/* 256 taken from linux/dirent.h */
	st->f_namelen = 256;
	r = 0;
        
out:
	kfsd_leave(1);
	return r;
}

static int
serve_fill_super(struct super_block * sb, mount_desc_t * m)
{
	inode_t cfs_root;
	struct inode * k_root;
	int r;
	
	assert(kfsd_have_lock());
	
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
	read_inode_withlock(k_root);

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
	
	kfsd_enter();
	size = vector_size(mounts);
	for (i = 0; i < size; i++)
	{
		mount_desc_t * m = vector_elt(mounts, i);
		if (!strcmp(m->path, &dev_name[4]))
		{
			struct super_block * sb;
			if (m->mounted)
			{
				kfsd_leave(1);
				return ERR_PTR(-E_BUSY);
			}
			if (modman_inc_cfs(m->cfs, fs_type, m->path) < 0)
			{
				kfsd_leave(1);
				return ERR_PTR(-E_NO_MEM);
			}
			sb = sget(fs_type, serve_compare_super, serve_set_super, m);
			if (IS_ERR(sb) || sb->s_root) /* sb->s_root means it is mounted already? */
			{
				modman_dec_cfs(m->cfs, fs_type);
				kfsd_leave(1);
				return sb;
			}
			sb->s_flags = flags;
			i = serve_fill_super(sb, m);
			if (i < 0)
			{
				modman_dec_cfs(m->cfs, fs_type);
				up_write(&sb->s_umount);
				deactivate_super(sb);
				kfsd_leave(1);
				return ERR_PTR(i);
			}
			m->mounted = 1;
			sb->s_flags |= MS_ACTIVE;
			kfsd_leave(1);
			printk("kkfsd: mounted \"kfs:%s\"\n", m->path);
			return sb;
		}
	}
	kfsd_leave(1);
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

	/* don't cache above KFS - we have our own caches */
	filp->f_mode |= O_SYNC;

	r = generic_file_open(inode, filp);
	if (r < 0)
		return r;

	kfsd_enter();
	r = CALL(dentry2cfs(filp->f_dentry), open, filp->f_dentry->d_inode->i_ino, 0, &fdesc);
	fdesc->common->parent = filp->f_dentry->d_parent->d_inode->i_ino;
	if (r < 0)
	{
		kfsd_leave(1);
		return r;
	}
	filp->private_data = fdesc;
	kfsd_leave(1);
	return 0;
}

static int
serve_release(struct inode * inode, struct file * filp)
{
	Dprintf("%s(name = \"%s\", fdesc = %p)\n", __FUNCTION__, filp->f_dentry->d_name.name, file2fdesc(filp));
	int r;
	kfsd_enter();
	r = CALL(dentry2cfs(filp->f_dentry), close, file2fdesc(filp));
	kfsd_leave(1);
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

	kfsd_enter();
	assert(dentry2cfs(dentry));
	r = CALL(dentry2cfs(dentry), lookup, dir->i_ino, dentry->d_name.name, &cfs_ino);
	if (r == -E_NOT_FOUND)
		cfs_ino = 0;
	else if (r < 0)
	{
		kfsd_leave(1);
		return ERR_PTR(r);
	}
	k_ino = cfs_ino;
	kfsd_leave(1); // TODO: do we need to hold the lock for iget() et al, too?

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
serve_setattr(struct dentry * dentry, struct iattr * attr)
{
	Dprintf("%s(\"%s\", attributes %u)\n", __FUNCTION__, dentry->d_name.name, attr->ia_valid);
	CFS_t * cfs;
	struct inode * inode = dentry->d_inode;
	fdesc_t * fdesc;
	int r;

	if (attr->ia_valid & ~(ATTR_SIZE | ATTR_CTIME))
		return -E_PERM;

	kfsd_enter();
	cfs = dentry2cfs(dentry);

	/* it would be nice if we didn't have to open the file to change the size, permissions, etc. */
	r = CALL(cfs, open, inode->i_ino, O_RDWR, &fdesc);
	if(r < 0)
	{
		kfsd_leave(1);
		return r;
	}

	/* check if the change is ok */
	r = inode_change_ok(inode, attr);
	if(r < 0)
		goto error;

	/* allow, but ignore */
	if(attr->ia_valid & ATTR_CTIME);

	if(attr->ia_valid & ATTR_SIZE)
	{
		if(inode->i_mode & S_IFDIR)
		{
			r = -E_PERM; /* operation not permitted */
			goto error;
		}

		if((r = CALL(cfs, truncate, fdesc, attr->ia_size)) < 0)
			goto error;
	}

	/* import the change to the inode */
	r = inode_setattr(inode, attr);
	assert(r >= 0);
	
error:
	if(CALL(cfs, close, fdesc) < 0)
		kdprintf(STDERR_FILENO, "%s: unable to CALL(%s, close, %p)\n", __FUNCTION__, modman_name_cfs(cfs), fdesc);
	kfsd_leave(1);
	return r;
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
	
	kfsd_enter();
	r = CALL(cfs, read, fdesc, data, offset, data_size);
	kfsd_leave(1);
	
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
	struct inode * inode = filp->f_dentry->d_inode;
	/* pick a reasonably big, but not too big, maximum size we will allocate
	 * on behalf of a requesting user process... TODO: use it repeatedly? */
	size_t data_size = (count > 65536) ? 65536 : count;
	char * data = vmalloc(data_size);
	uint32_t offset = *f_pos;
	ssize_t r = 0;
	unsigned long bytes;
	
	if (!data)
		return -E_NO_MEM;
	
	kfsd_enter();
	if (filp->f_flags & O_APPEND)
		offset = *f_pos = inode_get_size(inode);
	
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
	
	r = CALL(cfs, write, fdesc, data, offset, data_size);
	
	if (r < 0)
		goto out;
	
	*f_pos += r;
	inode->i_size = inode_get_size(inode);
	
out:
	kfsd_leave(1);
	vfree(data);
	return r;
}

static int
serve_unlink(struct inode * dir, struct dentry * dentry)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = CALL(dentry2cfs(dentry), unlink, dir->i_ino, dentry->d_name.name);
	kfsd_leave(1);
	return r;
}

static int create_withlock(struct inode * dir, struct dentry * dentry, int mode)
{
	inode_t cfs_ino;
	struct inode * inode;
	fdesc_t * fdesc;
	int r;

	assert(kfsd_have_lock());

	// TODO: support mode
	r = CALL(dentry2cfs(dentry), create, dir->i_ino, dentry->d_name.name, 0, &fdesc, &cfs_ino);
	if (r < 0)
		return r;
	assert(cfs_ino != INODE_NONE);
	fdesc->common->parent = dir->i_ino;
	// TODO: recent 2.6s support lookup_instantiate_filp() for atomic create+open.
	// Are there other approaches to do this that work with older 2.6s?
	// To work with knoppix's 2.6.12 we do not currently support atomic create+open.
	r = CALL(dentry2cfs(dentry), close, fdesc);
	if (r < 0)
		kdprintf(STDERR_FILENO, "%s(%s): unable to close created fdesc\n", __FUNCTION__, dentry->d_name.name);

	inode = new_inode(dir->i_sb);
	if (!inode)
		return -E_NO_MEM;
	inode->i_ino = cfs_ino;
	read_inode_withlock(inode);	
	d_instantiate(dentry, inode);

	return 0;
}

static int
serve_create(struct inode * dir, struct dentry * dentry, int mode, struct nameidata * nd)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = create_withlock(dir, dentry, mode);
	kfsd_leave(1);

	return r;
}

static int
serve_mknod(struct inode * dir, struct dentry * dentry, int mode, dev_t dev)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	if (!(mode & S_IFREG))
		return -E_PERM;

	kfsd_enter();
	r = create_withlock(dir, dentry, mode);
	kfsd_leave(1);
	return r;
}

static int
serve_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
	Dprintf("%s(%s)\n", __FUNCTION__, dentry->d_name.name);
	inode_t cfs_ino;
	struct inode * inode;
	int r;

	kfsd_enter();

	r = CALL(dentry2cfs(dentry), mkdir, dir->i_ino, dentry->d_name.name, &cfs_ino);
	if (r < 0)
	{
		kfsd_leave(1);
		return r;
	}

	inode = new_inode(dir->i_sb);
	if (!inode)
	{
		kfsd_leave(1);
		return -E_NO_MEM;
	}
	inode->i_ino = cfs_ino;
	read_inode_withlock(inode);	
	d_instantiate(dentry, inode);

	kfsd_leave(1);

	return 0;
}

static int
serve_rmdir(struct inode * dir, struct dentry * dentry)
{
	Dprintf("%s(%s)\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = CALL(dentry2cfs(dentry), rmdir, dir->i_ino, dentry->d_name.name);
	kfsd_leave(1);
	return r;
}

static int
serve_rename(struct inode * old_dir, struct dentry * old_dentry, struct inode * new_dir, struct dentry * new_dentry)
{
	Dprintf("%s(old = %lu, oldn = \"%s\", newd = %lu, newn = \"%s\")\n", __FUNCTION__, old_dir, old_dentry->d_name.name, new_dir, new_dentry->d_name.name);
	int r;

	kfsd_enter();
	if (dentry2cfs(old_dentry) != dentry2cfs(new_dentry))
	{
		kfsd_leave(1);
		return -E_PERM;
	}
	r = CALL(dentry2cfs(old_dentry), rename, new_dir->i_ino, old_dentry->d_name.name, new_dir->i_ino, new_dentry->d_name.name);
	kfsd_leave(1);
	return r;
}

#define RECLEN_MIN_SIZE (sizeof(((dirent_t *) NULL)->d_reclen) + (int) &((dirent_t *) NULL)->d_reclen)

static int
serve_dir_readdir(struct file * filp, void * k_dirent, filldir_t filldir)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r;

	kfsd_enter();
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
	kfsd_leave(1);

	if (r == -E_UNSPECIFIED)
		return 1;
	return 0;
}

static int
serve_fsync(struct file * filp, struct dentry * dentry, int datasync)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = kfs_sync();
	kfsd_leave(1);
	return r;
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
	.setattr = serve_setattr
};

static struct file_operations kfs_reg_file_ops = {
	.open = serve_open,
	.release = serve_release,
	.llseek = generic_file_llseek,
	.read = serve_read,
	.write = serve_write,
	.fsync = serve_fsync
};

static struct inode_operations kfs_dir_inode_ops = {
	.lookup	= serve_dir_lookup,
	.unlink	= serve_unlink,
	.create	= serve_create,
	.mknod = serve_mknod,
	.mkdir = serve_mkdir,
	.rmdir = serve_rmdir,
	.rename = serve_rename
};

static struct file_operations kfs_dir_file_ops = {
	.open = serve_open,
	.release = serve_release,
	.read = generic_read_dir,
	.readdir = serve_dir_readdir,
	.fsync = serve_fsync
};

static struct dentry_operations kfs_dentry_ops = {
	.d_delete = serve_delete_dentry
};

static struct super_operations kfs_superblock_ops = {
	.read_inode = serve_read_inode,
	.statfs = serve_stat_fs
};
