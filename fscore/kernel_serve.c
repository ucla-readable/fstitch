/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/vector.h>

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/namei.h>
#include <linux/mpage.h>
#include <linux/statfs.h>
#include <linux/vmalloc.h>
#include <linux/version.h>
#include <asm/uaccess.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 18)
#include <linux/mount.h>
#endif

#include <fscore/feature.h>
#include <fscore/fstitchd.h>
#include <fscore/modman.h>
#include <fscore/sync.h>
#include <fscore/sched.h>
#include <fscore/kernel_serve.h>

/* 2.6.12 has only CONFIG_PREEMPT or nothing.
 * By 2.6.13.4 linux added voluntary preemption and changed the defines. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13)
# ifndef CONFIG_PREEMPT_NONE
#  error CONFIG_PREEMPT_NONE not enabled. kfstitchd does not lock everything it should.
# endif
#else
# ifdef CONFIG_PREEMPT
#  error CONFIG_PREEMPT enabled. kfstitchd does not lock everything it should.
# endif
#endif

#ifdef CONFIG_SMP
# error CONFIG_SMP enabled. kfstitchd does not lock everything it should.
#endif

#define KERNEL_SERVE_DEBUG 0

#if KERNEL_SERVE_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

#define FSTITCHDEVROOT "fstitch:"

#if MALLOC_ACCOUNT
unsigned long long malloc_total = 0;
unsigned long long malloc_blocks = 0;
#endif

static struct file_system_type  fstitch_fs_type;
static struct inode_operations  fstitch_reg_inode_ops;
static struct file_operations   fstitch_reg_file_ops;
static struct inode_operations  fstitch_lnk_inode_ops;
static struct inode_operations  fstitch_dir_inode_ops;
static struct file_operations   fstitch_dir_file_ops;
static struct address_space_operations fstitch_aops;
static struct dentry_operations fstitch_dentry_ops;
static struct super_operations  fstitch_superblock_ops;


// The current fdesc, to help fstitch_aops.writepage()
static fdesc_t * fstitchd_fdesc;


struct mount_desc {
	const char * path;
	CFS_t * cfs;
	int mounted;
};
typedef struct mount_desc mount_desc_t;

static vector_t * mounts = NULL;

static mount_desc_t * mount_desc_create(const char * path, CFS_t * cfs)
{
	mount_desc_t * m = malloc(sizeof(*m));
	if (!m)
		return NULL;
	m->path = strdup(path);
	if (!m->path)
	{
		free(m);
		return NULL;
	}
	m->cfs = cfs;
	m->mounted = 0;
	return m;
}

static void mount_desc_destroy(mount_desc_t * m)
{
	free(m->path);
	free(m);
}

int kernel_serve_add_mount(const char * path, CFS_t * cfs)
{
	Dprintf("%s(path = \"%s\", cfs = %s)\n", __FUNCTION__, path, modman_name_cfs(cfs));
	mount_desc_t * m;
	int r;
	if (!path || !cfs)
		return -EINVAL;
	/* TODO: make sure there is no mount at this path already */
	m = mount_desc_create(path, cfs);
	if (!m)
		return -ENOMEM;
	r = vector_push_back(mounts, m);
	if (r < 0)
	{
		mount_desc_destroy(m);
		return r;
	}
	printk("kfstitchd: made \"fstitch:%s\" available for mounting\n", path);
	return 0;
}

static void kernel_serve_shutdown(void * ignore)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r = unregister_filesystem(&fstitch_fs_type);
	if (r < 0)
		fprintf(stderr, "kernel_serve_shutdown(): unregister_filesystem: %d\n", r);
#if MALLOC_ACCOUNT
	printf("malloc_total = %llu\n", malloc_total);
	printf("malloc_blocks = %llu\n", malloc_blocks);
#endif
}

int kernel_serve_init(void)
{
	int r;
	mounts = vector_create();
	if (!mounts)
		return -ENOMEM;
	r = fstitchd_register_shutdown_module(kernel_serve_shutdown, NULL, SHUTDOWN_PREMODULES);
	if (r < 0)
	{
		vector_destroy(mounts);
		mounts = NULL;
		return r;
	}
	return register_filesystem(&fstitch_fs_type);
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

static int feature_supported(CFS_t * cfs, feature_id_t id)
{
	const size_t max_id = CALL(cfs, get_max_feature_id);
	const bool * id_array = CALL(cfs, get_feature_array);
	if(id > max_id)
		return 0;
	return id_array[id];
}


struct kernel_metadata {
	uint16_t mode;
	int type;
	union {
		struct {
			const char * link;
			unsigned link_len;
		} symlink;	
	} type_info;
};
typedef struct kernel_metadata kernel_metadata_t;

static int kernel_get_metadata(void * arg, feature_id_t id, size_t size, void * data)
{
	const kernel_metadata_t * kernelmd = (kernel_metadata_t *) arg;
	if (FSTITCH_FEATURE_UID == id)
	{
		if (size < sizeof(current->euid))
			return -ENOMEM;
		*(typeof(current->euid) *) data = current->euid;
		return sizeof(current->euid);
	}
	else if (FSTITCH_FEATURE_GID == id)
	{
		if (size < sizeof(current->egid))
			return -ENOMEM;
		*(typeof(current->egid) *) data = current->egid;
		return sizeof(current->egid);
	}
	else if (FSTITCH_FEATURE_UNIX_PERM == id)
	{
		if (size < sizeof(kernelmd->mode))
			return -ENOMEM;
		*(uint16_t *) data = kernelmd->mode;
		return sizeof(kernelmd->mode);
	}
	else if (FSTITCH_FEATURE_FILETYPE == id)
	{
		if (size < sizeof(kernelmd->type))
			return -ENOMEM;
		*(int *) data = kernelmd->type;
		return sizeof(kernelmd->type);
	}
	else if (FSTITCH_FEATURE_SYMLINK == id && kernelmd->type == TYPE_SYMLINK)
	{
		if (size < kernelmd->type_info.symlink.link_len)
			return -ENOMEM;
		memcpy(data, kernelmd->type_info.symlink.link, kernelmd->type_info.symlink.link_len);
		return kernelmd->type_info.symlink.link_len;
	}
	return -ENOENT;
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

	assert(fstitchd_have_lock());

	cfs = sb2cfs(inode->i_sb);

	nlinks_supported = feature_supported(cfs, FSTITCH_FEATURE_NLINKS);
	uid_supported = feature_supported(cfs, FSTITCH_FEATURE_UID);
	gid_supported = feature_supported(cfs, FSTITCH_FEATURE_GID);
	perms_supported = feature_supported(cfs, FSTITCH_FEATURE_UNIX_PERM);
	mtime_supported = feature_supported(cfs, FSTITCH_FEATURE_MTIME);
	atime_supported = feature_supported(cfs, FSTITCH_FEATURE_ATIME);

	r = CALL(cfs, get_metadata, inode->i_ino, FSTITCH_FEATURE_FILETYPE, sizeof(type), &type);
	if (r < 0)
	{
		fprintf(stderr, "%s: CALL(get_metadata, ino = %lu) = %d\n", __FUNCTION__, inode->i_ino, r);
		return;
	}

	if (nlinks_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, FSTITCH_FEATURE_NLINKS, sizeof(inode->i_nlink), &inode->i_nlink);
		if (r < 0)
			fprintf(stderr, "%s: get_metadata for nlinks failed, manually counting links for directories and assuming files have 1 link\n", __FUNCTION__);
		else
			assert(r == sizeof(inode->i_nlink));
	}

	if (uid_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, FSTITCH_FEATURE_UID, sizeof(inode->i_uid), &inode->i_uid);
		if (r < 0)
			fprintf(stderr, "%s: file system at \"%s\" claimed UID but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_uid));
	}
	else
		inode->i_uid = 0;

	if (gid_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, FSTITCH_FEATURE_GID, sizeof(inode->i_gid), &inode->i_gid);
		if (r < 0)
			fprintf(stderr, "%s: file system at \"%s\" claimed GID but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_gid));
	}
	else
		inode->i_gid = 0;

	if (perms_supported)
	{
		uint16_t fstitch_mode;
		r = CALL(cfs, get_metadata, inode->i_ino, FSTITCH_FEATURE_UNIX_PERM, sizeof(fstitch_mode), &fstitch_mode);
		if (r < 0)
			fprintf(stderr, "%s: file system at \"%s\" claimed unix permissions but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
		{
			assert(r == sizeof(inode->i_mode));
			inode->i_mode = fstitch_mode;
		}
	}

	if (mtime_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, FSTITCH_FEATURE_MTIME, sizeof(inode->i_mtime.tv_sec), &inode->i_mtime.tv_sec);
		if (r < 0)
			fprintf(stderr, "%s: file system at \"%s\" claimed mtime but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
		else
			assert(r == sizeof(inode->i_mtime.tv_sec));
	}
	else
		inode->i_mtime = CURRENT_TIME;
	inode->i_ctime = inode->i_mtime;

	if (atime_supported)
	{
		r = CALL(cfs, get_metadata, inode->i_ino, FSTITCH_FEATURE_ATIME, sizeof(inode->i_atime.tv_sec), &inode->i_atime.tv_sec);
		if (r < 0)
			fprintf(stderr, "%s: file system at \"%s\" claimed atime but get_metadata returned %i\n", __FUNCTION__, modman_name_cfs(cfs), r);
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
		inode->i_op = &fstitch_dir_inode_ops;
		inode->i_fop = &fstitch_dir_file_ops;
	}
	else if (type == TYPE_FILE || type == TYPE_SYMLINK || type == TYPE_DEVICE)
	{
		if (!nlinks_supported)
			inode->i_nlink = 1;
		if (!perms_supported)
			inode->i_mode = 0666; // default, in case permissions are not supported
		if (type == TYPE_SYMLINK)
		{
			inode->i_mode |= S_IFLNK;
			inode->i_op = &fstitch_lnk_inode_ops;
		}
		else
		{
			inode->i_mode |= S_IFREG;
			inode->i_op = &fstitch_reg_inode_ops;
		}
		inode->i_fop = &fstitch_reg_file_ops;
		inode->i_mapping->a_ops = &fstitch_aops;
	}
	else if (type == TYPE_INVAL)
	{
		fprintf(stderr, "%s: inode %lu has type invalid\n", __FUNCTION__, inode->i_ino);
		goto exit;
	}
	else
	{
		fprintf(stderr, "%s: inode %lu has unsupported type\n", __FUNCTION__, inode->i_ino);
		goto exit;
	}

	CALL(sb2cfs(inode->i_sb), get_metadata, inode->i_ino, FSTITCH_FEATURE_SIZE, sizeof(inode->i_size), &inode->i_size);

  exit:
	return;
}

static void serve_read_inode(struct inode * inode)
{
	Dprintf("%s(ino = %lu)\n", __FUNCTION__, inode->i_ino);
	fstitchd_enter();
	read_inode_withlock(inode);
	fstitchd_leave(1);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static int serve_stat_fs(struct super_block * sb, struct kstatfs * st)
{
	mount_desc_t * m = (mount_desc_t *) sb->s_fs_info;
#else
static int serve_stat_fs(struct dentry * de, struct kstatfs * st)
{
	mount_desc_t * m = (mount_desc_t *) de->d_inode->i_sb->s_fs_info;
#endif
	Dprintf("%s(fstitch:%s)\n", __FUNCTION__, m->path);
	CFS_t * cfs = m->cfs;
	unsigned long temp;
	int r;
	
	fstitchd_enter();
	r = CALL(cfs, get_metadata, 0, FSTITCH_FEATURE_BLOCKSIZE, sizeof(st->f_frsize), &st->f_frsize);
	if (r < 0)
		goto out;
	assert(sizeof(st->f_frsize) == r);
	st->f_bsize = st->f_frsize;
	
	r = CALL(cfs, get_metadata, 0, FSTITCH_FEATURE_DEVSIZE, sizeof(temp), &temp);
	if (r < 0)
		goto out;
	assert(sizeof(temp) == r);
	st->f_blocks = temp;
	
	r = CALL(cfs, get_metadata, 0, FSTITCH_FEATURE_FREESPACE, sizeof(temp), &temp);
	if (r < 0)
		goto out;
	assert(sizeof(temp) == r);
	/* what is the difference between bfree and bavail? */
	st->f_bavail = temp;
	st->f_bfree = st->f_bavail;
	
	// TODO - add lfs features for these guys
	st->f_files = 0;
	st->f_ffree = 0;
	/* 256 taken from linux/dirent.h */
	st->f_namelen = 256;
	r = 0;
        
out:
	fstitchd_leave(1);
	return r;
}

static int serve_fill_super(struct super_block * sb, mount_desc_t * m)
{
	inode_t cfs_root;
	struct inode * k_root;
	int r;
	
	assert(fstitchd_have_lock());
	
	/* FIXME? */
	sb->s_blocksize = 4096;
	sb->s_blocksize_bits = 12;
	sb->s_magic = 0x88F50CF5;
	sb->s_op = &fstitch_superblock_ops;
	
	r = CALL(m->cfs, get_root, &cfs_root);
	assert(r >= 0);

	k_root = new_inode(sb);
	if (!k_root)
	{
		sb->s_dev = 0;
		return -ENOMEM;
	}
	/* is this next line really necessary? */
	k_root->i_sb = sb;
	k_root->i_ino = cfs_root;
	read_inode_withlock(k_root);

	sb->s_root = d_alloc_root(k_root);
	if (!sb->s_root)
	{
		iput(k_root);
		sb->s_dev = 0;
		return -ENOMEM;
	}
	return 0;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static struct super_block * serve_get_sb(struct file_system_type * fs_type, int flags, const char * dev_name, void * data)
#else
static int serve_get_sb(struct file_system_type * fs_type, int flags, const char * dev_name, void * data, struct vfsmount * vfs)
#endif
{
	Dprintf("%s()\n", __FUNCTION__);
	int i, size;
	if (strncmp(dev_name, FSTITCHDEVROOT, strlen(FSTITCHDEVROOT)))
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
		return ERR_PTR(-EINVAL);
#else
		return -EINVAL;
#endif
	
	fstitchd_enter();
	size = vector_size(mounts);
	for (i = 0; i < size; i++)
	{
		mount_desc_t * m = vector_elt(mounts, i);
		if (!strcmp(m->path, &dev_name[strlen(FSTITCHDEVROOT)]))
		{
			struct super_block * sb;
			if (m->mounted)
			{
				fstitchd_leave(1);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
				return ERR_PTR(-EBUSY);
#else
				return -EBUSY;
#endif
			}
			if (modman_inc_cfs(m->cfs, fs_type, m->path) < 0)
			{
				fstitchd_leave(1);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
				return ERR_PTR(-ENOMEM);
#else
				return -ENOMEM;
#endif
			}
			sb = sget(fs_type, serve_compare_super, serve_set_super, m);
			if (IS_ERR(sb) || sb->s_root) /* sb->s_root means it is mounted already? */
			{
				modman_dec_cfs(m->cfs, fs_type);
				fstitchd_leave(1);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
				return sb;
#else
				return simple_set_mnt(vfs, sb);
#endif
			}
			sb->s_flags = flags;
			i = serve_fill_super(sb, m);
			if (i < 0)
			{
				modman_dec_cfs(m->cfs, fs_type);
				up_write(&sb->s_umount);
				deactivate_super(sb);
				fstitchd_leave(1);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
				return ERR_PTR(i);
#else
				return i;
#endif
			}
			m->mounted = 1;
			sb->s_flags |= MS_ACTIVE;
			fstitchd_leave(1);
			printk("kfstitchd: mounted \"fstitch:%s\"\n", m->path);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
			return sb;
#else
			return simple_set_mnt(vfs, sb);
#endif
		}
	}
	fstitchd_leave(1);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
	return ERR_PTR(-ENOENT);
#else
	return -ENOENT;
#endif
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

	/* don't cache above featherstitch - we have our own caches */
	filp->f_mode |= O_SYNC;

	r = generic_file_open(inode, filp);
	if (r < 0)
		return r;

	fstitchd_enter();
	r = CALL(dentry2cfs(filp->f_dentry), open, filp->f_dentry->d_inode->i_ino, 0, &fdesc);
	fdesc->common->parent = filp->f_dentry->d_parent->d_inode->i_ino;
	if (r < 0)
	{
		fstitchd_leave(1);
		return r;
	}
	filp->private_data = fdesc;
	fstitchd_leave(1);
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

	fstitchd_enter();

	fstitchd_fdesc = file2fdesc(filp);
	r = serve_filemap_write_and_wait(inode->i_mapping);
	fstitchd_fdesc = NULL;
	if (r < 0)
		fprintf(stderr, "%s(filp = \"%s\"): serve_filemap_write_and_wait() = %d\n", __FUNCTION__, filp->f_dentry->d_name.name, r);

	r = CALL(dentry2cfs(filp->f_dentry), close, file2fdesc(filp));

	fstitchd_leave(1);
	return r;
}

static struct dentry * serve_dir_lookup(struct inode * dir, struct dentry * dentry, struct nameidata * ignore)
{
	inode_t cfs_ino;
	ino_t k_ino;
	struct inode * inode = NULL;
	int r;

	Dprintf("%s(dentry = \"%s\") (pid = %d)\n", __FUNCTION__, dentry->d_name.name, current->pid);

	fstitchd_enter();
	assert(dentry2cfs(dentry));
	r = CALL(dentry2cfs(dentry), lookup, dir->i_ino, dentry->d_name.name, &cfs_ino);
	if (r == -ENOENT)
		cfs_ino = 0;
	else if (r < 0)
	{
		fstitchd_leave(1);
		return ERR_PTR(r);
	}
	k_ino = cfs_ino;
	fstitchd_leave(1); // TODO: do we need to hold the lock for iget() et al, too?

	if (k_ino)
	{
		inode = iget(dir->i_sb, k_ino); 
		if (!inode)
			return ERR_PTR(-EPERM);
	}
	if (inode)
	{
		struct dentry * d = d_splice_alias(inode, dentry);
		if (d)
			d->d_op = &fstitch_dentry_ops;
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
	Dprintf("%s(\"%s\", attributes 0x%x)\n", __FUNCTION__, dentry->d_name.name, attr->ia_valid);
	CFS_t * cfs;
	struct inode * inode = dentry->d_inode;
	unsigned int supported = ATTR_SIZE;
	fdesc_t * fdesc;
	struct timespec now = current_fs_time(inode->i_sb);
	bool do_close = 0;
	int r;

	fstitchd_enter();
	cfs = dentry2cfs(dentry);

#if ATTR_FILE != 0
	supported |= ATTR_FILE;
#endif

	if(feature_supported(cfs, FSTITCH_FEATURE_MTIME))
		supported |= ATTR_MTIME | ATTR_MTIME_SET;
	if(feature_supported(cfs, FSTITCH_FEATURE_ATIME))
		supported |= ATTR_ATIME | ATTR_ATIME_SET;
	if(feature_supported(cfs, FSTITCH_FEATURE_UNIX_PERM))
		supported |= ATTR_MODE;

	// always at least act as if we support, so we do not error
	supported |= ATTR_UID | ATTR_GID;

	// not actually supported, but we won't error on these "supported" flags
	supported |= ATTR_CTIME;

	if(attr->ia_valid & ~supported)
	{
		Dprintf("%s: attribute set 0x%x (out of 0x%x) not supported\n", __FUNCTION__, attr->ia_valid & ~supported, attr->ia_valid);
		fstitchd_leave(0);
		return -ENOSYS;
	}

#if ATTR_FILE != 0
	if(attr->ia_valid & ATTR_FILE)
		fdesc = file2fdesc(attr->ia_file);
	else
#endif
	{
		/* it would be nice if we didn't have to open the file to change the permissions, etc. */
		r = CALL(cfs, open, inode->i_ino, O_RDONLY, &fdesc);
		if(r < 0)
		{
			fstitchd_leave(0);
			return r;
		}
		do_close = 1;
	}

	/* check if the change is ok */
	r = inode_change_ok(inode, attr);
	if(r < 0)
		goto error;

	if(attr->ia_valid & ATTR_SIZE)
	{
		if(inode->i_mode & S_IFDIR)
		{
			r = -EISDIR;
			goto error;
		}
		if((r = CALL(cfs, truncate, fdesc, attr->ia_size)) < 0)
			goto error;
	}

	fsmetadata_t fsm[5];
	int nfsm = 0;
	
	if((attr->ia_valid & ATTR_UID) && feature_supported(cfs, FSTITCH_FEATURE_UID))
	{
		fsm[nfsm].fsm_feature = FSTITCH_FEATURE_UID;
		fsm[nfsm].fsm_value.u = attr->ia_uid;
		nfsm++;
	}
	if((attr->ia_valid & ATTR_GID) && feature_supported(cfs, FSTITCH_FEATURE_GID))
	{
		fsm[nfsm].fsm_feature = FSTITCH_FEATURE_GID;
		fsm[nfsm].fsm_value.u = attr->ia_gid;
		nfsm++;
	}

	if(attr->ia_valid & ATTR_MODE)
	{
		fsm[nfsm].fsm_feature = FSTITCH_FEATURE_UNIX_PERM;
		fsm[nfsm].fsm_value.u = attr->ia_mode;
		nfsm++;
	}
	if(attr->ia_valid & (ATTR_MTIME | ATTR_MTIME_SET))
	{
		fsm[nfsm].fsm_feature = FSTITCH_FEATURE_MTIME;
		if(attr->ia_valid & ATTR_MTIME_SET)
			fsm[nfsm].fsm_value.u = now.tv_sec;
		else
			fsm[nfsm].fsm_value.u = attr->ia_mtime.tv_sec;
		nfsm++;
	}
	if(attr->ia_valid & (ATTR_ATIME | ATTR_ATIME_SET))
	{
		fsm[nfsm].fsm_feature = FSTITCH_FEATURE_ATIME;
		if(attr->ia_valid & ATTR_ATIME_SET)
			fsm[nfsm].fsm_value.u = now.tv_sec;
		else
			fsm[nfsm].fsm_value.u = attr->ia_atime.tv_sec;
		nfsm++;
	}

	if(nfsm > 0)
		if((r = CALL(cfs, set_metadata2, inode->i_ino, fsm, nfsm)) < 0)
			goto error;

	/* import the change to the inode */
	r = inode_setattr(inode, attr);
	assert(r >= 0);
	
error:
	if(do_close)
		if(CALL(cfs, close, fdesc) < 0)
			fprintf(stderr, "%s: unable to CALL(%s, close, %p)\n", __FUNCTION__, modman_name_cfs(cfs), fdesc);
	fstitchd_leave(1);
	return r;
}

static int serve_link(struct dentry * src_dentry, struct inode * parent, struct dentry * target_dentry)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, src_dentry->d_name.name, target_dentry->d_name.name);
	int r;

	fstitchd_enter();
	assert(dentry2cfs(src_dentry) == dentry2cfs(target_dentry));
	r = CALL(dentry2cfs(src_dentry), link, src_dentry->d_inode->i_ino, parent->i_ino, target_dentry->d_name.name);
	if (r >= 0)
	{
		struct inode * inode = src_dentry->d_inode;
		inode->i_mtime = inode->i_ctime = CURRENT_TIME_SEC;
		inc_nlink(inode);
		atomic_inc(&inode->i_count);
		d_instantiate(target_dentry, inode);
	}

	fstitchd_leave(1);
	return r;
}

static int serve_unlink(struct inode * dir, struct dentry * dentry)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	fstitchd_enter();
	r = CALL(dentry2cfs(dentry), unlink, dir->i_ino, dentry->d_name.name);
	if (r >= 0)
	{
		if (dentry->d_inode->i_mode & S_IFDIR)
			dir->i_nlink--;
		else
			dentry->d_inode->i_nlink--;
	}
	fstitchd_leave(1);
	return r;
}

static int create_withlock(struct inode * dir, struct dentry * dentry, uint16_t mode, kernel_metadata_t * kernelmd)
{
	metadata_set_t initialmd = { .get = kernel_get_metadata, .arg = kernelmd };
	CFS_t * cfs;
	inode_t cfs_ino;
	fdesc_t * fdesc;
	struct inode * inode;
	int r;

	assert(fstitchd_have_lock());

	cfs = dentry2cfs(dentry);

	r = CALL(cfs, create, dir->i_ino, dentry->d_name.name, 0, &initialmd, &fdesc, &cfs_ino);
	if (r < 0)
		return r;
	assert(cfs_ino != INODE_NONE);
	fdesc->common->parent = dir->i_ino;
	// TODO: recent 2.6s support lookup_instantiate_filp() for atomic create+open.
	// Are there other approaches to do this that work with older 2.6s?
	// To work with knoppix's 2.6.12 we do not currently support atomic create+open.
	r = CALL(cfs, close, fdesc);
	if (r < 0)
		fprintf(stderr, "%s(%s): unable to close created fdesc\n", __FUNCTION__, dentry->d_name.name);

	inode = new_inode(dir->i_sb);
	if (!inode)
		return -ENOMEM;
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
	kernel_metadata_t kernelmd = { .mode = mode, .type = TYPE_FILE };
	int r;

	fstitchd_enter();
	r = create_withlock(dir, dentry, mode, &kernelmd);
	fstitchd_leave(1);

	return r;
}

static int serve_mknod(struct inode * dir, struct dentry * dentry, int mode, dev_t dev)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	kernel_metadata_t kernelmd = { .mode = mode, .type = TYPE_FILE };
	int r;

	if (!(mode & S_IFREG))
		return -EPERM;

	fstitchd_enter();
	r = create_withlock(dir, dentry, mode, &kernelmd);
	fstitchd_leave(1);
	return r;
}

static int serve_symlink(struct inode * dir, struct dentry * dentry, const char * link)
{
	Dprintf("%s(\"%s\" -> \"%s\")\n", __FUNCTION__, dentry->d_name.name, link);
	int mode = S_IFLNK | S_IRWXUGO;
	kernel_metadata_t kernelmd = { .mode = mode, .type = TYPE_SYMLINK, .type_info.symlink = { .link = link, .link_len = strlen(link) } };
	int r;

	fstitchd_enter();

	if (!feature_supported(dentry2cfs(dentry), FSTITCH_FEATURE_SYMLINK))
	{
		fstitchd_leave(1);
		return -ENOSYS;
	}

	r = create_withlock(dir, dentry, mode, &kernelmd);

	fstitchd_leave(1);

	return r;
}

static int serve_mkdir(struct inode * dir, struct dentry * dentry, int mode)
{
	Dprintf("%s(%s)\n", __FUNCTION__, dentry->d_name.name);
	kernel_metadata_t kernelmd = { .mode = mode, .type = TYPE_DIR };
	metadata_set_t initialmd = { .get = kernel_get_metadata, .arg = &kernelmd };
	inode_t cfs_ino;
	struct inode * inode;
	int r;

	fstitchd_enter();

	r = CALL(dentry2cfs(dentry), mkdir, dir->i_ino, dentry->d_name.name, &initialmd, &cfs_ino);
	if (r < 0)
	{
		fstitchd_leave(1);
		return r;
	}

	inode = new_inode(dir->i_sb);
	if (!inode)
	{
		fstitchd_leave(1);
		return -ENOMEM;
	}
	inode->i_ino = cfs_ino;
	read_inode_withlock(inode);
	d_instantiate(dentry, inode);
	dir->i_nlink++;

	fstitchd_leave(1);

	return 0;
}

static int serve_rmdir(struct inode * dir, struct dentry * dentry)
{
	Dprintf("%s(%s)\n", __FUNCTION__, dentry->d_name.name);
	int r;

	fstitchd_enter();
	r = CALL(dentry2cfs(dentry), rmdir, dir->i_ino, dentry->d_name.name);
	if (r >= 0)
		dir->i_nlink--;
	fstitchd_leave(1);
	return r;
}

static int serve_rename(struct inode * old_dir, struct dentry * old_dentry, struct inode * new_dir, struct dentry * new_dentry)
{
	Dprintf("%s(old = %lu, oldn = \"%s\", newd = %lu, newn = \"%s\")\n", __FUNCTION__, old_dir->i_ino, old_dentry->d_name.name, new_dir->i_ino, new_dentry->d_name.name);
	struct inode * replace;
	CFS_t * cfs;
	int r;

	fstitchd_enter();
	cfs = dentry2cfs(old_dentry);
	if (cfs != dentry2cfs(new_dentry))
	{
		fstitchd_leave(1);
		return -EPERM;
	}
	replace = new_dentry->d_inode;
	r = CALL(cfs, rename, old_dir->i_ino, old_dentry->d_name.name, new_dir->i_ino, new_dentry->d_name.name);
	/* link counts of parent directories may have changed */
	if (r >= 0 && old_dentry->d_inode->i_mode & S_IFDIR)
	{
		old_dir->i_nlink--;
		new_dir->i_nlink++;
	}
	/* as well as that of the replaced file */
	if (replace)
		/* XXX: do we need to do anything special if i_nlink reaches 0 here? */
		replace->i_nlink--;
	fstitchd_leave(1);
	return r;
}

#define RECLEN_MIN_SIZE (sizeof(((dirent_t *) NULL)->d_reclen) + (int) &((dirent_t *) NULL)->d_reclen)

static int serve_dir_readdir(struct file * filp, void * k_dirent, filldir_t filldir)
{
	Dprintf("%s()\n", __FUNCTION__);
	int r;

	fstitchd_enter();
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
	fstitchd_leave(1);

	if (r == -1)
		return 1;
	return 0;
}

static int serve_fsync(struct file * filp, struct dentry * dentry, int datasync)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int r;

	fstitchd_enter();
	r = fstitch_sync();
	fstitchd_leave(1);
	return r;
}

static int read_link(struct dentry * dentry, char * buffer, int buflen)
{
	CFS_t * cfs;
	inode_t cfs_ino;
	int link_len;

	cfs = dentry2cfs(dentry);
	cfs_ino = dentry->d_inode->i_ino;

	if (!feature_supported(cfs, FSTITCH_FEATURE_SYMLINK))
		return -ENOSYS;

	link_len = CALL(cfs, get_metadata, cfs_ino, FSTITCH_FEATURE_SYMLINK, buflen - 1, buffer);
	if (link_len < 0)
	{
		if (link_len == -ENOMEM)
			return -ENAMETOOLONG;
		return link_len;
	}
	buffer[link_len++] = '\0';
	return link_len;
}

static char link_name[PATH_MAX];

static int serve_readlink(struct dentry * dentry, char __user * buffer, int buflen)
{
	// We could implement this fn using generic_readlink(), but it would
	// call serve_follow_link(), which uses dynamic memory allocation

	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int link_len;
	int r;

	// there should never be a link longer than sizeof(link_name),
	// but users may create buffers that are larger:
	if (buflen > sizeof(link_name))
		buflen = sizeof(link_name);

	fstitchd_enter();

	link_len = read_link(dentry, link_name, buflen);
	if (link_len < 0)
	{
		fstitchd_leave(1);
		return link_len;
	}

	// do we need to NULL-terminate buffer? (read_link() does)
	r = copy_to_user(buffer, link_name, link_len);
	if (r > 0)
	{
		fstitchd_leave(1);
		return -EFAULT;
	}

	fstitchd_leave(1);

	return link_len;
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13)
// 2.6.13 introduced a follow_link/put_link cookie (the void*) to make
// error recovery easier for some classes of filesystems:
// Subj: "Kernel bug: Bad page state: related to generic symlink code and mmap"
// http://www.gatago.com/linux/kernel/14688503.html
#define FOLLOW_LINK_RET_TYPE void*
#define FOLLOW_LINK_RET_VAL(x) ERR_PTR(x)
#else
#define FOLLOW_LINK_RET_TYPE int
#define FOLLOW_LINK_RET_VAL(x) x
#endif

static
FOLLOW_LINK_RET_TYPE
serve_follow_link(struct dentry * dentry, struct nameidata * nd)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	int link_len;
	char * nd_link_name;

	fstitchd_enter();

	link_len = read_link(dentry, link_name, sizeof(link_name));
	if (link_len < 0)
	{
		fstitchd_leave(1);
		return FOLLOW_LINK_RET_VAL(link_len);
	}

	nd_link_name = malloc(link_len);
	if (!nd_link_name)
	{
		fstitchd_leave(1);
		return FOLLOW_LINK_RET_VAL(-ENOMEM);
	}
	memcpy(nd_link_name, link_name, link_len);
	nd_set_link(nd, nd_link_name);

	fstitchd_leave(1);

	return FOLLOW_LINK_RET_VAL(0);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 13)
static void serve_put_link(struct dentry * dentry, struct nameidata * nd, void * cookie)
{
	(void) cookie;
#else
static void serve_put_link(struct dentry * dentry, struct nameidata * nd)
{
#endif
	Dprintf("%s(\"%s\")\n", __FUNCTION__, dentry->d_name.name);
	char * s = nd_get_link(nd);
	if (!IS_ERR(s))
		free(s);
}


//
// address space operations
// fs/smbfs/file.c served as a good reference for implementing these operations

// TODOs:
// - should we use the generic vector and sendfile functions?

static int serve_readpage(struct file * filp, struct page * page)
{
	char * buffer;
	loff_t offset = (loff_t) page->index << PAGE_CACHE_SHIFT;
	struct inode * inode = filp->f_dentry->d_inode;
	CFS_t * cfs;
	fdesc_t * fdesc;
	int r;

	Dprintf("%s(filp = \"%s\", offset = %lld)\n", __FUNCTION__, filp->f_dentry->d_name.name, offset);

	fstitchd_enter();
	assert(!PageHighMem(page));
	buffer = lowmem_page_address(page);
	cfs = dentry2cfs(filp->f_dentry);
	fdesc = file2fdesc(filp);

	r = CALL(cfs, read, fdesc, page, buffer, offset, PAGE_SIZE);
	/* CFS gives us an "error" when we hit EOF */
	if (r == -1)
		r = 0;
	else if (r < 0)
		goto out;

	if(r < PAGE_SIZE)
		memset(buffer + r, 0, PAGE_SIZE - r);
	flush_dcache_page(page);
	SetPageUptodate(page);

	inode->i_atime = current_fs_time(inode->i_sb);
	r = 0;

  out:
	fstitchd_leave(1);
	unlock_page(page);
	return r;
}


static ssize_t serve_write_page(struct file * filp, loff_t pos, struct page * page,
                                const char __user * buf, size_t len)
{
	struct address_space * mapping = page->mapping;
	struct inode * inode = mapping->host;
	CFS_t * cfs;
	fdesc_t * fdesc;
	ssize_t written;

	Dprintf("%s(file = \"%s\", pos = %lu, len = %u)\n", __FUNCTION__, filp->f_dentry->d_name.name, (unsigned long) pos, len);

	fstitchd_enter();

	if(!access_ok(VERIFY_READ, buf, len))
		return -EFAULT;

	cfs = sb2cfs(inode->i_sb);
	fdesc = file2fdesc(filp);
	assert(!PageHighMem(page));
	written = CALL(cfs, write, fdesc, page, buf, pos, len);
	if (written >= 0)
	{
		inode->i_mtime = inode->i_atime = current_fs_time(inode->i_sb);
		pos += written;
		if (pos > inode->i_size)
			inode->i_size = pos;

		assert(written == len);
	}

	fstitchd_leave(1);
	return written;
}

#include <linux/pagevec.h>
#include <linux/swap.h>

// Copy of 2.6.20.1 mm/filemap.c:__grab_cache_page() since it is not exported
static inline struct page *
__grab_cache_page(struct address_space *mapping, unsigned long index,
            struct page **cached_page, struct pagevec *lru_pvec)
{
	int err;
	struct page *page;
  repeat:
	page = find_lock_page(mapping, index);
	if (!page) {
		if (!*cached_page) {
			*cached_page = page_cache_alloc(mapping);
			if (!*cached_page)
				return NULL;
		}
		err = add_to_page_cache(*cached_page, mapping,
								index, GFP_KERNEL);
		if (err == -EEXIST)
			goto repeat;
		if (err == 0) {
			page = *cached_page;
			page_cache_get(page);
			if (!pagevec_add(lru_pvec, page))
				__pagevec_lru_add(lru_pvec);
			*cached_page = NULL;
		}
	}
	return page;
}

// Reimplementation of 2.6.20.1 generic_file_buffered_write() to work with
// integrated linux-fsttich cache
static ssize_t serve_generic_file_buffered_write(struct file * filp,
                                                 loff_t * ppos,
                                                 const char __user * buf,
                                                 size_t count)
{
	struct address_space * mapping = filp->f_mapping;
	struct inode * inode = mapping->host;
	long status = 0;
	struct page * cached_page = NULL;
	size_t bytes;
	ssize_t written = 0;
	struct pagevec lru_pvec;
	loff_t pos = *ppos;

	pagevec_init(&lru_pvec, 0);

	do
	{
		unsigned long index = pos >> PAGE_CACHE_SHIFT;
		unsigned long offset = pos & (PAGE_CACHE_SIZE - 1);
		struct page * page;
		size_t copied;

		bytes = PAGE_CACHE_SIZE - offset;
		/* Limit the size of the copy to the caller's write size */
		bytes = MIN(bytes, count);
		/* We do not use io vectors so we need not sorry about segments */

		/* Bring in the user page that we will copy from _first_.
		 * Otherwise there's a nasty deadlock on copying from the
		 * same page as we're writing to, without it being marked
		 * up-to-date. */
		fault_in_pages_readable(buf, bytes);

		page = __grab_cache_page(mapping, index, &cached_page, &lru_pvec);
		if (!page) {
			status = -ENOMEM;
			break;
		}

		if (unlikely(bytes == 0)) {
			status = 0;
			copied = 0;
			goto zero_length_segment;
		}

		/* serve_write_page() does work of prepare_write(),
		 * filemap_copy_from_user(), and commit_write() */
		copied = serve_write_page(filp, pos, page, buf, bytes);
		flush_dcache_page(page);
		if (unlikely(copied < 0))
		{
			loff_t isize = i_size_read(inode);
			status = copied;
			unlock_page(page);
			page_cache_release(page);
			/* serve_write_page() may have instantiated a few blocks
			 * outside i_size. Trim these off again. */
			if (pos + bytes > isize)
				vmtruncate(inode, isize);
			break;
		}

	  zero_length_segment:
		written += copied;
		count -= copied;
		pos += copied;
		buf += copied;
		if (unlikely(copied != bytes))
			status = -EFAULT;
		unlock_page(page);
		mark_page_accessed(page);
		page_cache_release(page);
		balance_dirty_pages_ratelimited(mapping);
		cond_resched();
	} while(count);
	*ppos = pos;

	if (cached_page)
		page_cache_release(cached_page);

	/* OK to ignore O_SYNC since serve_write_page() does its work */

	assert(!(filp->f_flags & O_DIRECT));

	pagevec_lru_add(&lru_pvec);
	return (written >= 0) ? written : status;
}

// Reimplementation of 2.6.20.1 __generic_file_aio_write_nolock() to call
// kernel_serve's generic_file_buffered_write()
static ssize_t serve__generic_file_aio_write_nolock(struct file * filp,
                                                    loff_t * ppos,
                                                    const char __user * buf,
                                                    size_t len)
{
	struct address_space * mapping = filp->f_mapping;
	struct inode * inode = mapping->host;
	size_t count = len;
	ssize_t written = 0;
	ssize_t err;

	if (!access_ok(VERIFY_READ, buf, len))
		return -EFAULT;

	vfs_check_frozen(inode->i_sb, SB_FREEZE_WRITE);

	/* We can write back this queue in page reclaim */
	current->backing_dev_info = mapping->backing_dev_info;

	err = generic_write_checks(filp, ppos, &count, S_ISBLK(inode->i_mode));
	if (err)
		goto out;

	if (count == 0)
		goto out;

	err = remove_suid(filp->f_path.dentry);
	if (err)
		goto out;

	file_update_time(filp);

	assert(!(filp->f_flags & O_DIRECT));

	written = serve_generic_file_buffered_write(filp, ppos, buf, count);

  out:
	current->backing_dev_info = NULL;
	return written ? written : err;
}

// Reimplementation of 2.6.20.1 generic_file_aio_write() to call kernel_serve's
// __generic_file_aio_write_nolock()
static ssize_t serve_generic_file_aio_write(struct file * filp,
                                            loff_t * ppos,
                                            const char __user * buf,
                                            size_t len)
{
	struct address_space * mapping = filp->f_mapping;
	struct inode * inode = mapping->host;
	ssize_t ret;

	mutex_lock(&inode->i_mutex);
	ret = serve__generic_file_aio_write_nolock(filp, ppos, buf, len);
	mutex_unlock(&inode->i_mutex);

	/* No need to handle O_SYNC or IS_SYNC() because the above call does the
	 * work */
	return ret;
}

// Reimplementation of 2.6.20.1 do_sync_write() to just call kernel_serve's
// generic_file_aio_write()
static ssize_t serve_do_sync_write(struct file * filp,
                                   const char __user * buf,
                                   size_t len, loff_t * ppos)
{
	/* Call what kernel_serve would expose as filp->f_op->aio_write() */
	return serve_generic_file_aio_write(filp, ppos, buf, len);
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

static struct file_system_type fstitch_fs_type = {
	.owner = THIS_MODULE,
	.name = "fstitch",
	.get_sb = serve_get_sb,
	.kill_sb = serve_kill_sb
};

static struct inode_operations fstitch_reg_inode_ops = {
	//.truncate =  // TODO: add? (what happens now?)
	.setattr = serve_setattr
};

static struct inode_operations fstitch_lnk_inode_ops = {
	.setattr = serve_setattr,
	.readlink = serve_readlink,
	.follow_link = serve_follow_link,
	.put_link = serve_put_link
};

static struct file_operations fstitch_reg_file_ops = {
	.open = serve_open,
	.release = serve_release,
	.llseek = generic_file_llseek,
	.read = do_sync_read,
	.aio_read = generic_file_aio_read,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
# error Check that serve_do_sync_write works with this kernel version
	//.write = serve_do_sync_write,
#else
	.write = serve_do_sync_write,
#endif
	.mmap = generic_file_readonly_mmap,
	.fsync = serve_fsync
};

static struct inode_operations fstitch_dir_inode_ops = {
	.setattr = serve_setattr,
	.lookup	= serve_dir_lookup,
	.link = serve_link,
	.unlink	= serve_unlink,
	.create	= serve_create,
	.mknod = serve_mknod,
	.symlink = serve_symlink,
	.mkdir = serve_mkdir,
	.rmdir = serve_rmdir,
	.rename = serve_rename
};

static struct file_operations fstitch_dir_file_ops = {
	.open = serve_open,
	.release = serve_release,
	.read = generic_read_dir,
	.readdir = serve_dir_readdir,
	.fsync = serve_fsync
};

static struct address_space_operations fstitch_aops = {
	.readpage = serve_readpage,
};

static struct dentry_operations fstitch_dentry_ops = {
	.d_delete = serve_delete_dentry
};

static struct super_operations fstitch_superblock_ops = {
	.read_inode = serve_read_inode,
	.statfs = serve_stat_fs
};
