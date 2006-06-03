#include <inc/error.h>
#include <lib/kdprintf.h>
#include <lib/vector.h>
#include <lib/string.h>
#include <lib/stdio.h>
#include <lib/assert.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/mpage.h>
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
static struct address_space_operations kfs_aops;
static struct dentry_operations kfs_dentry_ops;
static struct super_operations  kfs_superblock_ops;


// The current fdesc, to help kfs_aops.writepage()
static fdesc_t * kfsd_fdesc;


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
	r = kfsd_register_shutdown_module(kernel_serve_shutdown, NULL, SHUTDOWN_PREMODULES);
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

static CFS_t * sb2cfs(struct super_block * sb)
{
	return ((mount_desc_t *) sb->s_fs_info)->cfs;
}

static CFS_t * dentry2cfs(struct dentry * dentry)
{
	return sb2cfs(dentry->d_sb);
}

static fdesc_t * file2fdesc(struct file * filp)
{
	return (fdesc_t *) filp->private_data;
}

static int feature_supported(CFS_t * cfs, inode_t cfs_ino, int feature_id)
{
	const size_t num_features = CALL(cfs, get_num_features, cfs_ino);
	size_t i;

	for(i=0; i < num_features; i++)
		if(CALL(cfs, get_feature, cfs_ino, i)->id == feature_id)
			return 1;

	return 0;
}


struct kernel_metadata {
	int mode;
};
typedef struct kernel_metadata kernel_metadata_t;

static int kernel_get_metadata(void * arg, uint32_t id, size_t size, void * data)
{
	const kernel_metadata_t * kernelmd = (kernel_metadata_t *) arg;
	if (KFS_feature_uid.id == id)
	{
		if (size < sizeof(current->euid))
			return -E_NO_MEM;
		*(typeof(current->euid) *) data = current->euid;
		return sizeof(current->euid);
	}
	else if (KFS_feature_gid.id == id)
	{
		if (size < sizeof(current->egid))
			return -E_NO_MEM;
		*(typeof(current->egid) *) data = current->egid;
		return sizeof(current->egid);
	}
	else if (KFS_feature_unix_permissions.id == id)
	{
		if (size < sizeof(kernelmd->mode))
			return -E_NO_MEM;
		*(typeof(kernelmd->mode) *) data = kernelmd->mode;
		return sizeof(kernelmd->mode);
	}	
	return -E_NOT_FOUND;			
}


/* Looking at the NFS file system implementation was very helpful for some of these functions. */

static int serve_set_super(struct super_block * sb, void * data)
{
	sb->s_fs_info = data;
	return set_anon_super(sb, data);
}

static int serve_compare_super(struct super_block * sb, void * data)
{
	mount_desc_t * m = data;
	mount_desc_t * old = sb->s_fs_info;
	if(strcmp(old->path, m->path))
		return 0;
	if(old->cfs != m->cfs)
		return 0;
	return 1;
}

static void read_inode_withlock(struct inode * inode)
{
	CFS_t * cfs;
	uint32_t type;
	bool nlinks_supported, uid_supported, gid_supported, perms_supported, mtime_supported, atime_supported;
	int r;

	assert(kfsd_have_lock());

	cfs = sb2cfs(inode->i_sb);

	nlinks_supported = feature_supported(cfs, inode->i_ino, KFS_feature_nlinks.id);
	uid_supported = feature_supported(cfs, inode->i_ino, KFS_feature_uid.id);
	gid_supported = feature_supported(cfs, inode->i_ino, KFS_feature_gid.id);
	perms_supported = feature_supported(cfs, inode->i_ino, KFS_feature_unix_permissions.id);
	mtime_supported = feature_supported(cfs, inode->i_ino, KFS_feature_mtime.id);
	atime_supported = feature_supported(cfs, inode->i_ino, KFS_feature_atime.id);

	r = CALL(cfs, get_metadata, inode->i_ino, KFS_feature_filetype.id, sizeof(type), &type);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "%s: CALL(get_metadata, ino = %u) = %d\n", __FUNCTION__, inode->i_ino, r);
		return;
	}

	if (nlinks_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, KFS_feature_nlinks.id, sizeof(inode->i_nlink), &inode->i_nlink);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: get_metadata for nlinks failed, manually counting links for directories and assuming files have 1 link\n", __FUNCTION__);
		else
			assert(r == sizeof(inode->i_nlink));
	}

	if (uid_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, KFS_feature_uid.id, sizeof(inode->i_uid), &inode->i_uid);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed UID but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_uid));
	}
	else
		inode->i_uid = 0;

	if (gid_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, KFS_feature_gid.id, sizeof(inode->i_gid), &inode->i_gid);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed GID but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_gid));
	}
	else
		inode->i_gid = 0;

	if (perms_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, KFS_feature_unix_permissions.id, sizeof(inode->i_mode), &inode->i_mode);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed unix permissions but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_mode));
	}

	if (mtime_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, KFS_feature_mtime.id, sizeof(inode->i_mtime.tv_sec), &inode->i_mtime.tv_sec);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed mtime but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_mtime.tv_sec));
	}
	else
		inode->i_mtime = CURRENT_TIME;
	inode->i_ctime = inode->i_mtime;

	if (atime_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, KFS_feature_atime.id, sizeof(inode->i_atime.tv_sec), &inode->i_atime.tv_sec);
		if (r < 0)
			kdprintf(STDERR_FILENO, "%s: file system at \"%s\" claimed atime but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_atime.tv_sec));
	}
	else
		inode->i_atime = CURRENT_TIME;

	if (type == TYPE_DIR)
	{
		if (!nlinks_supported)
		{
			dirent_t dirent;
			uint32_t basep = 0;
			fdesc_t * fdesc;
			
			inode->i_nlink = 2;
			
			r = CALL(cfs, open, inode->i_ino, 0, &fdesc);
			assert(r >= 0);
			/* HACK: this does not have to be the correct value */
			fdesc->common->parent = inode->i_ino;
			
			while ((r = CALL(cfs, get_dirent, fdesc, &dirent, sizeof(dirent), &basep)) >= 0)
				if (dirent.d_type == TYPE_DIR)
					inode->i_nlink++;
			
			r = CALL(cfs, close, fdesc);
			assert(r >= 0);
		}
		if (!perms_supported)
			inode->i_mode = 0777; // default, in case permissions are not supported
		inode->i_mode |= S_IFDIR;
		inode->i_op = &kfs_dir_inode_ops;
		inode->i_fop = &kfs_dir_file_ops;
	}
	else if (type == TYPE_FILE || type == TYPE_DEVICE)
	{
		if (!nlinks_supported)
			inode->i_nlink = 1;
		if (!perms_supported)
			inode->i_mode = 0666; // default, in case permissions are not supported
		inode->i_mode |= S_IFREG;
		inode->i_op = &kfs_reg_inode_ops;
		inode->i_fop = &kfs_reg_file_ops;
		inode->i_mapping->a_ops = &kfs_aops;
	}
	else if (type == TYPE_INVAL)
	{
		kdprintf(STDERR_FILENO, "%s: inode %u has type invalid\n", __FUNCTION__, inode->i_ino);
		goto exit;
	}
	else
	{
		kdprintf(STDERR_FILENO, "%s: inode %u has unsupported type\n", __FUNCTION__, inode->i_ino);
		goto exit;
	}

	CALL(sb2cfs(inode->i_sb), get_metadata, inode->i_ino, KFS_feature_size.id, sizeof(inode->i_size), &inode->i_size);

  exit:
	return;
}

static void serve_read_inode(struct inode * inode)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, inode->i_ino);
	kfsd_enter();
	read_inode_withlock(inode);
	kfsd_leave(1);
}

static int serve_stat_fs(struct super_block * sb, struct kstatfs * st)
{
	mount_desc_t * m = (mount_desc_t *) sb->s_fs_info;
	Dprintf("%s(kfs:%s)\n", __FUNCTION__, m->path);
	CFS_t * cfs = m->cfs;
	int r;
	
	kfsd_enter();
	r = CALL(cfs, get_metadata, 0, KFS_feature_blocksize.id, sizeof(st->f_frsize), &st->f_frsize);
	if (r < 0)
		goto out;
	assert(sizeof(st->f_frsize) == r);
	st->f_bsize = st->f_frsize;
	
	r = CALL(cfs, get_metadata, 0, KFS_feature_devicesize.id, sizeof(st->f_blocks), &st->f_blocks);
	if (r < 0)
		goto out;
	assert(sizeof(st->f_blocks) == r);
	
	r = CALL(cfs, get_metadata, 0, KFS_feature_freespace.id, sizeof(st->f_bavail), &st->f_bavail);
	if (r < 0)
		goto out;
	assert(sizeof(st->f_bavail) == r);
	/* what is the difference between bfree and bavail? */
	st->f_bfree = st->f_bavail;
	
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

static int serve_fill_super(struct super_block * sb, mount_desc_t * m)
{
	inode_t cfs_root;
	struct inode * k_root;
	int r;
	
	assert(kfsd_have_lock());
	
	/* FIXME? */
	sb->s_blocksize = 4096;
	sb->s_blocksize_bits = 12;
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

static struct super_block * serve_get_sb(struct file_system_type * fs_type, int flags, const char * dev_name, void * data)
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

static void serve_kill_sb(struct super_block * sb)
{
	Dprintf("%s()\n", __FUNCTION__);
	mount_desc_t * m = sb->s_fs_info;
	modman_dec_cfs(m->cfs, sb->s_type);
	m->mounted = 0;
	kill_anon_super(sb);
}

static int serve_open(struct inode * inode, struct file * filp)
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

// A copy of 'mm/filemap.c:filemap_write_and_wait()' from 2.6.16.11;
// because it is not exported by the kernel
static int serve_filemap_write_and_wait(struct address_space * mapping)
{
	int retval = 0;

	if (mapping->nrpages) {
		retval = filemap_fdatawrite(mapping);
		if (retval == 0)
			retval = filemap_fdatawait(mapping);
	}
	return retval;
}

static int serve_release(struct inode * inode, struct file * filp)
{
	Dprintf("%s(filp = \"%s\", fdesc = %p)\n", __FUNCTION__, filp->f_dentry->d_name.name, file2fdesc(filp));
	int r;

	kfsd_enter();

	kfsd_fdesc = file2fdesc(filp);
	r = serve_filemap_write_and_wait(inode->i_mapping);
	kfsd_fdesc = NULL;
	if (r < 0)
		kdprintf(STDERR_FILENO, "%s(filp = \"%s\"): serve_filemap_write_and_wait() = %d\n", __FUNCTION__, filp->f_dentry->d_name.name, r);

	r = CALL(dentry2cfs(filp->f_dentry), close, file2fdesc(filp));

	kfsd_leave(1);
	return r;
}

static struct dentry * serve_dir_lookup(struct inode * dir, struct dentry * dentry, struct nameidata * ignore)
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

#ifndef ATTR_FILE
/* ATTR_FILE and struct iattr's field ia_file were introduced in 2.6.15:
 * http://www.ussg.iu.edu/hypermail/linux/kernel/0510.3/0102.html
 * Disable ATTR_FILE support when building for older kernels: */
#define ATTR_FILE 0
#endif

static int serve_setattr(struct dentry * dentry, struct iattr * attr)
{
	Dprintf("%s(\"%s\", attributes %u)\n", __FUNCTION__, dentry->d_name.name, attr->ia_valid);
	CFS_t * cfs;
	struct inode * inode = dentry->d_inode;
	unsigned int supported = ATTR_SIZE;
	fdesc_t * fdesc;
	struct timespec now = current_fs_time(inode->i_sb);
	int r;

	kfsd_enter();
	cfs = dentry2cfs(dentry);

	if(feature_supported(cfs, inode->i_ino, KFS_feature_mtime.id))
		supported |= ATTR_MTIME | ATTR_MTIME_SET;
	if(feature_supported(cfs, inode->i_ino, KFS_feature_atime.id))
		supported |= ATTR_ATIME | ATTR_ATIME_SET;
	if(feature_supported(cfs, inode->i_ino, KFS_feature_unix_permissions.id))
		supported |= ATTR_MODE;

	// always at least act as if we support, so we do not error
	supported |= ATTR_UID | ATTR_GID;

	// not actually supported, but we won't error on these "supported" flags
	supported |= ATTR_CTIME;

	if(attr->ia_valid & ~supported)
	{
		Dprintf("%s: attribute set %u not supported\n", __FUNCTION__, attr->ia_valid);
		kfsd_leave(0);
		return -E_NO_SYS;
	}

#if ATTR_FILE != 0
	if(attr->ia_valid & ATTR_FILE)
		fdesc = file2fdesc(attr->ia_file);
	else
#endif
	{
		/* it would be nice if we didn't have to open the file to change the permissions, etc. */
		r = CALL(cfs, open, inode->i_ino, O_RDWR, &fdesc);
		if(r < 0)
		{
			kfsd_leave(0);
			return r;
		}
	}

	/* check if the change is ok */
	r = inode_change_ok(inode, attr);
	if(r < 0)
		goto error;

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
	
	if((attr->ia_valid & ATTR_UID) && feature_supported(cfs, inode->i_ino, KFS_feature_uid.id))
	{
		if((r = CALL(cfs, set_metadata, inode->i_ino, KFS_feature_uid.id, sizeof(attr->ia_uid), &attr->ia_uid)) < 0)
			goto error;
	}
	if((attr->ia_valid & ATTR_GID) && feature_supported(cfs, inode->i_ino, KFS_feature_gid.id))
	{
		if((r = CALL(cfs, set_metadata, inode->i_ino, KFS_feature_gid.id, sizeof(attr->ia_gid), &attr->ia_gid)) < 0)
			goto error;
	}

	if(attr->ia_valid & ATTR_MODE)
	{
		uint32_t cfs_mode = attr->ia_mode;
		if((r = CALL(cfs, set_metadata, inode->i_ino, KFS_feature_unix_permissions.id, sizeof(cfs_mode), &cfs_mode)) < 0)
			goto error;
	}
	if(attr->ia_valid & (ATTR_MTIME | ATTR_MTIME_SET))
	{
		time_t mtime;
		if(attr->ia_valid & ATTR_MTIME_SET)
			mtime = now.tv_sec;
		else
			mtime = attr->ia_mtime.tv_sec;
		if((r = CALL(cfs, set_metadata, inode->i_ino, KFS_feature_mtime.id, sizeof(mtime), &mtime)) < 0)
			goto error;
	}
	if(attr->ia_valid & (ATTR_ATIME | ATTR_ATIME_SET))
	{
		time_t atime;
		if(attr->ia_valid & ATTR_ATIME_SET)
			atime = now.tv_sec;
		else
			atime = attr->ia_atime.tv_sec;
		if((r = CALL(cfs, set_metadata, inode->i_ino, KFS_feature_atime.id, sizeof(atime), &atime)) < 0)
			goto error;
	}

	/* import the change to the inode */
	r = inode_setattr(inode, attr);
	assert(r >= 0);
	
error:
	if(!(attr->ia_valid & ATTR_FILE))
		if(CALL(cfs, close, fdesc) < 0)
			kdprintf(STDERR_FILENO, "%s: unable to CALL(%s, close, %p)\n", __FUNCTION__, modman_name_cfs(cfs), fdesc);
	kfsd_leave(1);
	return r;
}

static ssize_t serve_read(struct file * filp, char __user * buffer, size_t count, loff_t * f_pos)
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

static int serve_link(struct dentry * src_dentry, struct inode * parent, struct dentry * target_dentry)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, src_dentry->d_name.name, target_dentry->d_name.name);
	int r;

	kfsd_enter();
	assert(dentry2cfs(src_dentry) == dentry2cfs(target_dentry));
	r = CALL(dentry2cfs(src_dentry), link, src_dentry->d_inode->i_ino, parent->i_ino, target_dentry->d_name.name);
	if (r >= 0)
	{
		struct inode * inode = new_inode(parent->i_sb);
		if (!inode)
		{
			r = -E_NO_MEM;
			goto out;
		}
		inode->i_ino = src_dentry->d_inode->i_ino;
		read_inode_withlock(inode);
		d_instantiate(target_dentry, inode);
	}

out:
	kfsd_leave(1);
	return r;
}

static int serve_unlink(struct inode * dir, struct dentry * dentry)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = CALL(dentry2cfs(dentry), unlink, dir->i_ino, dentry->d_name.name);
	if (r >= 0 && dentry->d_inode->i_mode & S_IFDIR)
		dir->i_nlink--;
	kfsd_leave(1);
	return r;
}

static int create_withlock(struct inode * dir, struct dentry * dentry, int mode)
{
	kernel_metadata_t kernelmd = { .mode = mode };
	metadata_set_t initialmd = { .get = kernel_get_metadata, .arg = &kernelmd };
	inode_t cfs_ino;
	struct inode * inode;
	fdesc_t * fdesc;
	int r;

	assert(kfsd_have_lock());

	r = CALL(dentry2cfs(dentry), create, dir->i_ino, dentry->d_name.name, 0, &initialmd, &fdesc, &cfs_ino);
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
	if (dentry->d_inode->i_mode & S_IFDIR)
		dir->i_nlink++;

	return 0;
}

static int serve_create(struct inode * dir, struct dentry * dentry, int mode, struct nameidata * nd)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = create_withlock(dir, dentry, mode);
	kfsd_leave(1);

	return r;
}

static int serve_mknod(struct inode * dir, struct dentry * dentry, int mode, dev_t dev)
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

static int serve_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
	Dprintf("%s(%s)\n", __FUNCTION__, dentry->d_name.name);
	kernel_metadata_t kernelmd = { .mode = mode };
	metadata_set_t initialmd = { .get = kernel_get_metadata, .arg = &kernelmd };
	inode_t cfs_ino;
	struct inode * inode;
	int r;

	kfsd_enter();

	r = CALL(dentry2cfs(dentry), mkdir, dir->i_ino, dentry->d_name.name, &initialmd, &cfs_ino);
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
	dir->i_nlink++;

	kfsd_leave(1);

	return 0;
}

static int serve_rmdir(struct inode * dir, struct dentry * dentry)
{
	Dprintf("%s(%s)\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = CALL(dentry2cfs(dentry), rmdir, dir->i_ino, dentry->d_name.name);
	if (r >= 0)
		dir->i_nlink--;
	kfsd_leave(1);
	return r;
}

static int serve_rename(struct inode * old_dir, struct dentry * old_dentry, struct inode * new_dir, struct dentry * new_dentry)
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

static int serve_dir_readdir(struct file * filp, void * k_dirent, filldir_t filldir)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r;

	kfsd_enter();
	while (1)
	{
		uint32_t cfs_fpos = filp->f_pos;
		dirent_t dirent;

		r = CALL(dentry2cfs(filp->f_dentry), get_dirent, file2fdesc(filp), &dirent, sizeof(dirent), &cfs_fpos);
		if (r < 0)
			break;

		r = filldir(k_dirent, dirent.d_name, dirent.d_namelen, 0, dirent.d_fileno, dirent.d_type);
		if (r < 0)
			break;
		filp->f_pos = cfs_fpos;
	}
	kfsd_leave(1);

	if (r == -E_UNSPECIFIED)
		return 1;
	return 0;
}

static int serve_fsync(struct file * filp, struct dentry * dentry, int datasync)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	kfsd_enter();
	r = kfs_sync();
	kfsd_leave(1);
	return r;
}


//
// address space operations
// fs/smbfs/file.c served as a good reference for implementing these operations

// TODOs:
// - should we use generic_file_read() since we now have pagecache support?
// - should we use the generic vector and sendfile functions?
// - linux's pagecache and kkfsd's caches do duplicate cacheing, can we
//   improve this situation?

static int serve_readpage(struct file * filp, struct page * page)
{
	char * buffer = kmap(page);
	loff_t offset = (loff_t) page->index << PAGE_CACHE_SHIFT;
	int count = PAGE_SIZE;
	struct inode * inode = filp->f_dentry->d_inode;
	CFS_t * cfs;
	fdesc_t * fdesc;
	int r;

	Dprintf("%s(filp = \"%s\", offset = %ld)\n", __FUNCTION__, filp->f_dentry->d_name.name, offset);

	kfsd_enter();
	page_cache_get(page);
	cfs = dentry2cfs(filp->f_dentry);
	fdesc = file2fdesc(filp);

	do {
		r = CALL(cfs, read, fdesc, buffer, offset, count);
		if (r == -E_EOF)
			r = 0;
		else if (r < 0)
			goto out;

		count -= r;
		offset += r;
		buffer += r;

		inode->i_atime = current_fs_time(inode->i_sb);
	} while (count && r > 0);

	memset(buffer, 0, count);
	flush_dcache_page(page);
	SetPageUptodate(page);
	r = 0;

  out:
	page_cache_release(page);
	kfsd_leave(1);
	kunmap(page);
	unlock_page(page);
	return r;
}

static int serve_writepage_sync(struct inode * inode, fdesc_t * fdesc,
                                struct page * page, unsigned long pageoffset,
                                unsigned int count)
{
	loff_t offset = ((loff_t) page->index << PAGE_CACHE_SHIFT) + pageoffset;
	char * buffer = kmap(page) + pageoffset;
	CFS_t * cfs = sb2cfs(inode->i_sb);
	int r = 0;

	Dprintf("%s(ino = %u, offset = %ld, count = %ld)\n", __FUNCTION__, inode->i_ino, offset, count);

	assert(kfsd_have_lock());

	do {
		r = CALL(cfs, write, fdesc, buffer, offset, count);
		if (r < 0)
			break;

		count -= r;
		offset += r;
		buffer += r;

		inode->i_mtime = inode->i_atime = current_fs_time(inode->i_sb);
		if (offset > inode->i_size)
			inode->i_size = offset;
	} while (count);

	kunmap(page);
	return r;
}

static int serve_writepage(struct page * page, struct writeback_control * wbc)
{
	struct address_space * mapping = page->mapping;
	struct inode * inode;
	unsigned long end_index;
	unsigned offset = PAGE_CACHE_SIZE;
	CFS_t * cfs;
	fdesc_t * fdesc;
	int r;

	assert(mapping);
	inode = mapping->host;
	assert(inode);

	Dprintf("%s(ino = %u, index = %lu)\n", __FUNCTION__, inode->i_ino, page->index);

	end_index = inode->i_size >> PAGE_CACHE_SHIFT;

	if (page->index >= end_index)
	{
		offset = inode->i_size & (PAGE_CACHE_SIZE-1);
		if (page->index >= end_index + 1 || !offset)
			return 0;
	}

	assert(kfsd_have_lock());

	cfs = sb2cfs(inode->i_sb);

	// HACK: CFS can not write files without an fdesc, but writepage()
	// has only an inode. Two workarounds:
	if (kfsd_fdesc)
	{
		// We were called by kernel_serve code that knows to set kfsd_fdesc
		fdesc = kfsd_fdesc;
	}
	else
	{
		// We were not called by kernel_serve code that knows about kfsd_fdesc
		printf("%s: Please set kfsd_fdesc for this trace:\n", __FUNCTION__);
		dump_stack();

		r = CALL(cfs, open, inode->i_ino, 0, &fdesc);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "%s(ino = %u): open() = %d\n", __FUNCTION__, inode->i_ino, r);
			unlock_page(page);
			return r;
		}
	}
		
	page_cache_get(page);
	r = serve_writepage_sync(inode, fdesc, page, 0, offset);
	SetPageUptodate(page);
	unlock_page(page);
	page_cache_release(page);

	if (!kfsd_fdesc)
		if ((r = CALL(cfs, close, fdesc)) < 0)
			kdprintf(STDERR_FILENO, "%s(ino = %u): close() = %d\n", __FUNCTION__, inode->i_ino, r);

	return r;
}

static int serve_prepare_write(struct file * filp, struct page * page,
                               unsigned from, unsigned to)
{
	return 0;
}

static int serve_commit_write(struct file * filp, struct page * page,
                              unsigned offset, unsigned to)
{
	Dprintf("%s(filp = \"%s\", index = %lu)\n", __FUNCTION__, filp->f_dentry->d_name.name, page->index);
	unsigned count = to - offset;
	int r;

	kfsd_enter();
	r = serve_writepage_sync(filp->f_dentry->d_inode, file2fdesc(filp), page, offset, count);
	kfsd_leave(1);
	return r;
}


//
// dentry operations

static int serve_delete_dentry(struct dentry * dentry)
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
	.write = generic_file_write, // kfs_aops requires going thru the pagecache
	.mmap = generic_file_mmap,
	.fsync = serve_fsync
};

static struct inode_operations kfs_dir_inode_ops = {
	.lookup	= serve_dir_lookup,
	.link = serve_link,
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

static struct address_space_operations kfs_aops = {
	.readpage = serve_readpage,
	.writepage = serve_writepage,
	.prepare_write = serve_prepare_write,
	.commit_write = serve_commit_write
};

static struct dentry_operations kfs_dentry_ops = {
	.d_delete = serve_delete_dentry
};

static struct super_operations kfs_superblock_ops = {
	.read_inode = serve_read_inode,
	.statfs = serve_stat_fs
};
