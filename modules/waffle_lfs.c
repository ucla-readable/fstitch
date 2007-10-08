/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/pool.h>

#include <fscore/bd.h>
#include <fscore/lfs.h>
#include <fscore/modman.h>
#include <fscore/debug.h>
#include <fscore/feature.h>

#include <modules/waffle.h>
#include <modules/waffle_lfs.h>

#define WAFFLE_LFS_DEBUG 1

#if WAFFLE_LFS_DEBUG
#define Dprintf(x...) printf("WAFFLEDEBUG: " x)
#else
#define Dprintf(x...)
#endif

/* values for the "purpose" parameter */
#define PURPOSE_FILEDATA 0
#define PURPOSE_DIRDATA 1
#define PURPOSE_INDIRECT 2
#define PURPOSE_DINDIRECT 3

struct waffle_fdesc {
	/* extend struct fdesc */
	struct fdesc_common * common;
	struct fdesc_common base;
	
	struct waffle_fdesc ** f_cache_pprev;
	struct waffle_fdesc * f_cache_next;
	uint32_t f_nopen;
	uint32_t f_age;
	
	inode_t f_inode;
	uint8_t f_type;
	bdesc_t * f_inode_cache;
	uint32_t f_inode_number;
	const struct waffle_inode * f_ip;
};

/* waffle LFS structure */
struct waffle_info {
	LFS_t lfs;
	
	BD_t * ubd;
	patch_t ** write_head;
	bdesc_t * super_cache;
	const struct waffle_super * super;
	struct {
		bdesc_t * bb_cache;
		uint32_t bb_number;
		/* block bitmap block index */
		uint32_t bb_index;
	} active, shapshot;
	uint32_t free_blocks;
	struct waffle_fdesc * filecache;
	int fdesc_count;
};

DECLARE_POOL(waffle_fdesc_pool, struct waffle_fdesc);
static int n_waffle_instances = 0;

static inline uint8_t waffle_to_fstitch_type(uint16_t type)
{
	switch(type & WAFFLE_S_IFMT)
	{
		case WAFFLE_S_IFDIR:
			return TYPE_DIR;
		case WAFFLE_S_IFREG:
			return TYPE_FILE;
		case WAFFLE_S_IFLNK:
			return TYPE_SYMLINK;	
		default:
			return TYPE_INVAL;
	}
}

/* NOTE: both 0 and INVALID_BLOCK may be returned as errors from this function */
static uint32_t waffle_get_inode_block(struct waffle_info * info, const struct waffle_inode * inode, uint32_t offset)
{
	bdesc_t * indirect;
	bdesc_t * dindirect;
	if(inode->i_size <= WAFFLE_INLINE_SIZE)
		/* inode has no blocks; data is inline */
		return INVALID_BLOCK;
	offset /= WAFFLE_BLOCK_SIZE;
	if(offset >= (inode->i_size + WAFFLE_BLOCK_SIZE - 1) / WAFFLE_BLOCK_SIZE)
		/* asked for an offset past the last block */
		return INVALID_BLOCK;
	if(offset < WAFFLE_DIRECT_BLOCKS)
		return inode->i_direct[offset];
	if(offset < WAFFLE_INDIRECT_BLOCKS)
	{
		indirect = CALL(info->ubd, read_block, inode->i_indirect, 1, NULL);
		if(!indirect)
			return INVALID_BLOCK;
		return ((uint32_t *) bdesc_data(indirect))[offset - WAFFLE_DIRECT_BLOCKS];
	}
	dindirect = CALL(info->ubd, read_block, inode->i_dindirect, 1, NULL);
	if(!dindirect)
		return INVALID_BLOCK;
	indirect = CALL(info->ubd, read_block, ((uint32_t *) bdesc_data(dindirect))[(offset - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS], 1, NULL);
	if(!indirect)
		return INVALID_BLOCK;
	return ((uint32_t *) bdesc_data(indirect))[(offset - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS];
}

static inode_t waffle_get_inode(struct waffle_info * info, struct waffle_fdesc * fdesc)
{
	uint32_t offset, block;
	assert(fdesc);
	assert(fdesc->f_inode >= WAFFLE_ROOT_INODE && fdesc->f_inode <= info->super->s_inodes);
	assert(!fdesc->f_inode_cache);
	
	offset = fdesc->f_inode * sizeof(struct waffle_inode);
	block = waffle_get_inode_block(info, &info->super->s_active.sn_inode, offset);
	if(block == INVALID_BLOCK)
		return -1;
	fdesc->f_inode_cache = CALL(info->ubd, read_block, block, 1, NULL);
	if(!fdesc->f_inode_cache)
		return -1;
	/* TODO: make it so we can track this and change it later if we COW this block */
	bdesc_retain(fdesc->f_inode_cache);
	
	offset %= WAFFLE_BLOCK_SIZE;
	fdesc->f_ip = (struct waffle_inode *) (bdesc_data(fdesc->f_inode_cache) + offset);
	
	return fdesc->f_inode;
}

/* returns 0 on error (including not found), otherwise the inode number */
static inode_t waffle_directory_search(struct waffle_info * info, struct waffle_fdesc * fdesc, const char * name, bdesc_t ** block_p, uint32_t * number_p, uint16_t * offset_p)
{
	uint32_t index;
	for(index = 0; index < fdesc->f_ip->i_size; index += WAFFLE_BLOCK_SIZE)
	{
		bdesc_t * block;
		uint16_t offset;
		uint32_t number = waffle_get_inode_block(info, fdesc->f_ip, index);
		if(!number || number == INVALID_BLOCK)
			return 0;
		block = CALL(info->ubd, read_block, number, 1, NULL);
		if(!block)
			return 0;
		for(offset = 0; offset < WAFFLE_BLOCK_SIZE; offset += sizeof(struct waffle_dentry))
		{
			struct waffle_dentry * dirent = (struct waffle_dentry *) (bdesc_data(block) + offset);
			if(!dirent->d_inode)
				continue;
			if(strcmp(dirent->d_name, name))
				continue;
			if(block_p)
				*block_p = block;
			if(number_p)
				*number_p = number;
			if(offset_p)
				*offset_p = offset;
			return dirent->d_inode;
		}
	}
	return 0;
}

static int waffle_get_metadata(LFS_t * object, const struct waffle_fdesc * fd, uint32_t id, size_t size, void * data)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, fd, id);
	struct waffle_info * info = (struct waffle_info *) object;
	if(id == FSTITCH_FEATURE_SIZE)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = fd->f_ip->i_size;
	}
	else if(id == FSTITCH_FEATURE_FILETYPE)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = fd->f_type;
	}
	else if(id == FSTITCH_FEATURE_FREESPACE)
	{
		struct waffle_info * info = (struct waffle_info *) object;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = info->free_blocks;
	}
	else if(id == FSTITCH_FEATURE_FILE_LFS)
	{
		if(size < sizeof(object))
			return -ENOMEM;
		size = sizeof(object);
		*((typeof(object) *) data) = object;
	}
	else if(id == FSTITCH_FEATURE_BLOCKSIZE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = WAFFLE_BLOCK_SIZE;
	}
	else if(id == FSTITCH_FEATURE_DEVSIZE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = info->super->s_blocks;
	}
	else if(id == FSTITCH_FEATURE_NLINKS)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = (uint32_t) fd->f_ip->i_links;
	}
	else if(id == FSTITCH_FEATURE_UID)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = fd->f_ip->i_uid;
	}
	else if(id == FSTITCH_FEATURE_GID)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = fd->f_ip->i_gid;
	}
	else if(id == FSTITCH_FEATURE_UNIX_PERM)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint16_t))
			return -ENOMEM;
		size = sizeof(uint16_t);
		*((uint16_t *) data) = fd->f_ip->i_mode & ~WAFFLE_S_IFMT;
	}
	else if(id == FSTITCH_FEATURE_MTIME)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = fd->f_ip->i_mtime;
	}
	else if(id == FSTITCH_FEATURE_ATIME)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = fd->f_ip->i_atime;
	}
	else if(id == FSTITCH_FEATURE_SYMLINK)
	{
		struct waffle_info * info = (struct waffle_info *) object;
		if(!fd || fd->f_type != TYPE_SYMLINK)
			return -EINVAL;
		/* fd->f_ip->i_size includes the zero byte */
		if(size < fd->f_ip->i_size)
			return -ENOMEM;
		size = fd->f_ip->i_size;
		if(size <= WAFFLE_INLINE_SIZE)
			memcpy(data, fd->f_ip->i_inline, size);
		else
		{
			bdesc_t * symlink_block;
			symlink_block = CALL(info->ubd, read_block, fd->f_ip->i_direct[0], 1, NULL);
			if(!symlink_block)
				return -1;
			memcpy(data, bdesc_data(symlink_block), fd->f_ip->i_size);
		}
	}
	else
		return -EINVAL;
	
	return size;
}

/* LFS functions start here */

static int waffle_get_root(LFS_t * object, inode_t * inode)
{
	*inode = WAFFLE_ROOT_INODE;
	return 0;
}

static uint32_t waffle_allocate_block(LFS_t * object, fdesc_t * file, int purpose, patch_t ** tail)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	/* FIXME */
	return INVALID_BLOCK;
}

static bdesc_t * waffle_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("%s %u\n", __FUNCTION__, number);
	struct waffle_info * info = (struct waffle_info *) object;
	return CALL(info->ubd, read_block, number, 1, page);
}

static bdesc_t * waffle_synthetic_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("%s %u\n", __FUNCTION__, number);
	struct waffle_info * info = (struct waffle_info *) object;
	return CALL(info->ubd, synthetic_read_block, number, 1, page);
}

static void waffle_free_fdesc(LFS_t * object, fdesc_t * fdesc);

static fdesc_t * waffle_lookup_inode(LFS_t * object, inode_t inode)
{
	Dprintf("%s %u\n", __FUNCTION__, inode);
	struct waffle_info * info = (struct waffle_info *) object;
	struct waffle_fdesc * fd = NULL;
	struct waffle_fdesc * oldest_fd = NULL;
	static uint32_t age = 0;
	int r, nincache = 0;
	
	if(inode <= 0)
		return NULL;
	
	if(!++age)
		++age;
	
	for(fd = info->filecache; fd; fd = fd->f_cache_next)
		if(fd->f_inode == inode)
		{
			fd->f_nopen += (fd->f_age ? 1 : 2);
			fd->f_age = age;
			return (fdesc_t *) fd;
		}
		else if(fd->f_age)
		{
			++nincache;
			if(!oldest_fd || (int32_t) (oldest_fd->f_age - fd->f_age) > 0)
				oldest_fd = fd;
		}
	
	fd = waffle_fdesc_pool_alloc();
	if(!fd)
		goto waffle_lookup_inode_exit;
	
	fd->common = &fd->base;
	fd->base.parent = INODE_NONE;
	fd->f_nopen = 1;
	fd->f_age = age;
	fd->f_inode = inode;
	fd->f_inode_cache = NULL;
	fd->f_ip = NULL;
	
	r = waffle_get_inode(info, fd);
	if(r < 0)
		goto waffle_lookup_inode_exit;
	fd->f_type = waffle_to_fstitch_type(fd->f_ip->i_mode);
	
	/* stick in cache */
	if(oldest_fd && nincache >= 4)
	{
		oldest_fd->f_age = 0;
		waffle_free_fdesc(object, (fdesc_t *) oldest_fd);
	}
	fd->f_cache_pprev = &info->filecache;
	fd->f_cache_next = info->filecache;
	info->filecache = fd;
	if(fd->f_cache_next)
		fd->f_cache_next->f_cache_pprev = &fd->f_cache_next;
	
	return (fdesc_t *) fd;
	
  waffle_lookup_inode_exit:
	waffle_fdesc_pool_free(fd);
	return NULL;
}

static int waffle_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * inode)
{
	Dprintf("%s %u:%s\n", __FUNCTION__, parent, name);
	struct waffle_fdesc * fd = (struct waffle_fdesc *) waffle_lookup_inode(object, parent);
	if(!fd)
		return -EINVAL;
	*inode = waffle_directory_search((struct waffle_info *) object, fd, name, NULL, NULL, NULL);
	waffle_free_fdesc(object, (fdesc_t *) fd);
	return *inode ? 0 : -ENOENT;
}

static void __waffle_free_fdesc(struct waffle_fdesc * fdesc)
{
	assert(fdesc && !fdesc->f_nopen);
	if(fdesc->f_inode_cache)
		bdesc_release(&fdesc->f_inode_cache);
	if((*fdesc->f_cache_pprev = fdesc->f_cache_next))
		fdesc->f_cache_next->f_cache_pprev = fdesc->f_cache_pprev;
	waffle_fdesc_pool_free(fdesc);
}

static void waffle_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("%s %p\n", __FUNCTION__, fdesc);
	struct waffle_fdesc * fd = (struct waffle_fdesc *) fdesc;
	if(fd && !--fd->f_nopen)
		__waffle_free_fdesc(fd);
}

static uint32_t waffle_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	struct waffle_fdesc * fdesc = (struct waffle_fdesc *) file;
	if(fdesc->f_type == TYPE_SYMLINK || fdesc->f_ip->i_size <= WAFFLE_INLINE_SIZE)
		return 0;
	return (fdesc->f_ip->i_size + WAFFLE_BLOCK_SIZE - 1) / WAFFLE_BLOCK_SIZE;
}

/* XXX: we need to do some fancy stuff to support writing to the blocks returned by this */
static uint32_t waffle_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, offset);
	struct waffle_fdesc * fdesc = (struct waffle_fdesc *) file;
	return waffle_get_inode_block((struct waffle_info *) object, fdesc->f_ip, offset);
}

static int waffle_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, basep, *basep);
	struct waffle_info * info = (struct waffle_info *) object;
	struct waffle_fdesc * fd = (struct waffle_fdesc *) file;
	uint32_t index = *basep * sizeof(struct waffle_dentry);
	uint16_t offset = index % WAFFLE_BLOCK_SIZE;
	if(fd->f_type != TYPE_DIR)
		return -ENOTDIR;
	for(index -= offset; index < fd->f_ip->i_size; index += WAFFLE_BLOCK_SIZE)
	{
		bdesc_t * block;
		uint32_t number = waffle_get_inode_block(info, fd->f_ip, offset);
		if(!number || number == INVALID_BLOCK)
			return -ENOENT;
		block = CALL(info->ubd, read_block, number, 1, NULL);
		if(!block)
			return -ENOENT;
		for(; offset < WAFFLE_BLOCK_SIZE; offset += sizeof(struct waffle_dentry))
		{
			struct waffle_dentry * dirent = (struct waffle_dentry *) (bdesc_data(block) + offset);
			uint16_t namelen, reclen;
			if(!dirent->d_inode)
			{
				++*basep;
				continue;
			}
			namelen = strlen(dirent->d_name);
			reclen = sizeof(*entry) - sizeof(entry->d_name) + namelen + 1;
			if(size < reclen)
				return -ENOSPC;
			entry->d_fileno = dirent->d_inode;
			entry->d_reclen = reclen;
			entry->d_type = waffle_to_fstitch_type(dirent->d_type);
			entry->d_namelen = namelen;
			strcpy(entry->d_name, dirent->d_name);
			++*basep;
			return 0;
		}
		offset = 0;
	}
	/* end of directory */
	return -1;
}

static int waffle_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, block);
	/* FIXME */
	return -ENOSYS;
}

static fdesc_t * waffle_allocate_name(LFS_t * object, inode_t parent_inode, const char * name,
                                      uint8_t type, fdesc_t * link, const metadata_set_t * initialmd,
                                      inode_t * new_inode, patch_t ** head)
{
	Dprintf("%s %u:%s\n", __FUNCTION__, parent_inode, name);
	/* FIXME */
	return NULL;
}

static int waffle_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, patch_t ** head)
{
	Dprintf("%s %u:%s -> %u:%s\n", __FUNCTION__, oldparent, oldname, newparent, newname);
	/* FIXME */
	return -ENOSYS;
}

static uint32_t waffle_truncate_file_block(LFS_t * object, fdesc_t * file, patch_t ** head)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	/* FIXME */
	return INVALID_BLOCK;
}

static int waffle_free_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, block);
	/* FIXME */
	return -ENOSYS;
}

static int waffle_remove_name(LFS_t * object, inode_t parent, const char * name, patch_t ** head)
{
	Dprintf("%s %u:%s\n", __FUNCTION__, parent, name);
	/* FIXME */
	return -ENOSYS;
}

static int waffle_write_block(LFS_t * object, bdesc_t * block, uint32_t number, patch_t ** head)
{
	Dprintf("%s %u\n", __FUNCTION__, number);
	struct waffle_info * info = (struct waffle_info *) object;
	assert(head);
	
	/* XXX: we must do COW here! */
	return CALL(info->ubd, write_block, block, number);
}

static patch_t ** waffle_get_write_head(LFS_t * object)
{
	struct waffle_info * info = (struct waffle_info *) object;
	return info->write_head;
}

static int32_t waffle_get_block_space(LFS_t * object)
{
	struct waffle_info * info = (struct waffle_info *) object;
	return CALL(info->ubd, get_block_space);
}

static const bool waffle_features[] = {[FSTITCH_FEATURE_SIZE] = 1, [FSTITCH_FEATURE_FILETYPE] = 1, [FSTITCH_FEATURE_FREESPACE] = 1, [FSTITCH_FEATURE_FILE_LFS] = 1, [FSTITCH_FEATURE_BLOCKSIZE] = 1, [FSTITCH_FEATURE_DEVSIZE] = 1, [FSTITCH_FEATURE_MTIME] = 1, [FSTITCH_FEATURE_ATIME] = 1, [FSTITCH_FEATURE_GID] = 1, [FSTITCH_FEATURE_UID] = 1, [FSTITCH_FEATURE_UNIX_PERM] = 1, [FSTITCH_FEATURE_NLINKS] = 1, [FSTITCH_FEATURE_SYMLINK] = 1, [FSTITCH_FEATURE_DELETE] = 1};

static size_t waffle_get_max_feature_id(LFS_t * object)
{
	return sizeof(waffle_features) / sizeof(waffle_features[0]) - 1;
}

static const bool * waffle_get_feature_array(LFS_t * object)
{
	return waffle_features;
}

static int waffle_get_metadata_inode(LFS_t * object, inode_t inode, uint32_t id, size_t size, void * data)
{
	Dprintf("%s %u, %u\n", __FUNCTION__, inode, id);
	const struct waffle_fdesc * fd = (struct waffle_fdesc *) waffle_lookup_inode(object, inode);
	int r = waffle_get_metadata(object, fd, id, size, data);
	if(fd)
		waffle_free_fdesc(object, (fdesc_t *) fd);
	return r;
}

static int waffle_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, id);
	const struct waffle_fdesc * fd = (struct waffle_fdesc *) file;
	return waffle_get_metadata(object, fd, id, size, data);
}

static int waffle_set_metadata2_inode(LFS_t * object, inode_t inode, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	Dprintf("%s %u\n", __FUNCTION__, inode);
	/* FIXME */
	return -ENOSYS;
}

static int waffle_set_metadata2_fdesc(LFS_t * object, fdesc_t * file, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	/* FIXME */
	return -ENOSYS;
}

static int waffle_destroy(LFS_t * lfs)
{
	struct waffle_info * info = (struct waffle_info *) lfs;
	struct waffle_fdesc * fd;
	int r;
	
	if(info->fdesc_count)
		return -EBUSY;
	
	r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);
	
	if(info->super_cache)
		bdesc_release(&info->super_cache);
	for(fd = info->filecache; fd; fd = fd->f_cache_next)
		assert(fd->f_nopen == 1 && fd->f_age != 0);
	while(info->filecache)
		waffle_free_fdesc(lfs, (fdesc_t *) info->filecache);
	
	if(!--n_waffle_instances)
		waffle_fdesc_pool_free_all();
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	return 0;
}

LFS_t * waffle_lfs(BD_t * block_device)
{
	Dprintf("WAFFLEDEBUG: %s\n", __FUNCTION__);
	struct waffle_info * info;
	LFS_t * lfs;
	
	if(!block_device)
		return NULL;
	if(block_device->blocksize != WAFFLE_BLOCK_SIZE)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	
	lfs = &info->lfs;
	LFS_INIT(lfs, waffle);
	OBJMAGIC(lfs) = WAFFLE_FS_MAGIC;
	
	info->ubd = lfs->blockdev = block_device;
	info->write_head = CALL(block_device, get_write_head);
	info->active.bb_cache = NULL;
	info->active.bb_number = INVALID_BLOCK;
	info->active.bb_index = INVALID_BLOCK;
	info->shapshot = info->active;
	info->free_blocks = 0;
	info->filecache = NULL;
	info->fdesc_count = 0;
	
	/* superblock */
	info->super_cache = CALL(info->ubd, read_block, WAFFLE_SUPER_BLOCK, 1, NULL);
	if(!info->super_cache)
	{
		free(info);
		return NULL;
	}
	bdesc_retain(info->super_cache);
	info->super = (struct waffle_super *) bdesc_data(info->super_cache);
	
	/* FIXME: recover from unclean shutdown? */
	/* FIXME: index blocks to compare active image to snapshot? */
	/* FIXME: count the free blocks */
	
	n_waffle_instances++;
	
	if(modman_add_anon_lfs(lfs, __FUNCTION__))
	{
		DESTROY(lfs);
		return NULL;
	}
	if(modman_inc_bd(block_device, lfs, NULL) < 0)
	{
		modman_rem_lfs(lfs);
		DESTROY(lfs);
		return NULL;
	}
	
	return lfs;
}
