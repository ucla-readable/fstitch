#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/feature.h>
#include <kfs/modman.h>
#include <kfs/wholedisk_lfs.h>

#define INODE_ROOT ((inode_t) 1)
#define INODE_DISK ((inode_t) 2)

#define DISK_NAME "disk"

struct wd_fdesc {
	fdesc_common_t * common;
	fdesc_common_t base;
};
typedef struct wd_fdesc wd_fdesc_t;

union fdesc_cast {
	wd_fdesc_t wd;
	fdesc_t cast;
};

/* stupid compiler warnings... must use unions to evade */
static union fdesc_cast root_fdesc = {wd: {common: &root_fdesc.wd.base, base: {parent: INODE_NONE}}};
static union fdesc_cast disk_fdesc = {wd: {common: &disk_fdesc.wd.base, base: {parent: INODE_ROOT}}};

static int wholedisk_get_root(LFS_t * lfs, inode_t * ino)
{
	*ino = INODE_ROOT;
	return 0;
}

static uint32_t wholedisk_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head)
{
	/* always fail - no block accounting */
	return INVALID_BLOCK;
}

static bdesc_t * wholedisk_lookup_block(LFS_t * object, uint32_t number)
{
	return CALL(object->blockdev, read_block, number, 1);
}

static bdesc_t * wholedisk_synthetic_lookup_block(LFS_t * object, uint32_t number)
{
	return CALL(object->blockdev, synthetic_read_block, number, 1);
}

static fdesc_t * wholedisk_lookup_inode(LFS_t * object, inode_t inode)
{
	if(inode == INODE_ROOT)
		return (fdesc_t *) &root_fdesc;
	if(inode == INODE_DISK)
		return (fdesc_t *) &disk_fdesc;
	return NULL;
}

static int wholedisk_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * inode)
{
	/* only allow the fixed disk name */
	if(parent != INODE_ROOT || strcmp(name, DISK_NAME))
		return -ENOENT;
	if(inode)
		*inode = INODE_DISK;
	return 0;
}

static void wholedisk_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	/* no-op */
}

static uint32_t wholedisk_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	if(file != (fdesc_t *) &disk_fdesc)
		return INVALID_BLOCK;
	return object->blockdev->numblocks;
}

static uint32_t wholedisk_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	if(file != (fdesc_t *) &disk_fdesc)
		return INVALID_BLOCK;
	return offset / object->blocksize;
}

static int wholedisk_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	const char * name;
	size_t namelen;
	
	if(file != (fdesc_t *) &root_fdesc)
		return -ENOTDIR;
	
	if(size < sizeof(*entry) - sizeof(entry->d_name))
		return -EINVAL;
	
	switch(*basep)
	{
		case 0:
			/* . */
			entry->d_type = TYPE_DIR;
			entry->d_fileno = INODE_ROOT;
			entry->d_namelen = 1;
			entry->d_filesize = 0;
			name = ".";
			break;
		case 1:
			/* .. */
			entry->d_type = TYPE_DIR;
			entry->d_fileno = INODE_ROOT;
			entry->d_namelen = 2;
			entry->d_filesize = 0;
			name = "..";
			break;
		case 2:
			/* disk */
			entry->d_type = TYPE_DEVICE;
			entry->d_fileno = INODE_DISK;
			namelen = strlen(DISK_NAME);
			/* do not make DISK_NAME longer than DIRENT_MAXNAMELEN...
			 * strncpy() is defined to pad the remaining space in the
			 * destination with nulls which we can't do here! */
			assert(namelen < DIRENT_MAXNAMELEN);
			entry->d_namelen = namelen;
			entry->d_filesize = object->blocksize * object->blockdev->numblocks;
			name = DISK_NAME;
			break;
		default:
			return -1;
	}
	
	entry->d_reclen = sizeof(*entry) - sizeof(entry->d_name) + entry->d_namelen + 1;
	if(entry->d_reclen > size)
	{
		memset(entry, 0, size);
		return -EINVAL;
	}
	strcpy(entry->d_name, name);
	entry->d_name[entry->d_namelen] = 0;
	
	*basep += 1;
	
	return 0;
}

static int wholedisk_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	/* always fail - size immutable */
	return -EINVAL;
}

static fdesc_t * wholedisk_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, const metadata_set_t * initialmd, inode_t * newino, chdesc_t ** head)
{
	/* always fail - no filenames */
	return NULL;
}

static int wholedisk_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head)
{
	/* always fail - no filenames */
	return -EPERM;
}

static uint32_t wholedisk_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head)
{
	/* always fail - size immutable */
	return INVALID_BLOCK;
}

static int wholedisk_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head)
{
	/* always fail - no block accounting */
	return -EINVAL;
}

static int wholedisk_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head)
{
	/* always fail - no filenames */
	return -EPERM;
}

static int wholedisk_write_block(LFS_t * object, bdesc_t * block, uint32_t number, chdesc_t ** head)
{
	return CALL(object->blockdev, write_block, block, number);
}

static chdesc_t ** wholedisk_get_write_head(LFS_t * object)
{
	return CALL(object->blockdev, get_write_head);
}

static int32_t wholedisk_get_block_space(LFS_t * object)
{
	return CALL(object->blockdev, get_block_space);
}

static const bool wholedisk_features[] = {[KFS_FEATURE_SIZE] = 1, [KFS_FEATURE_FILETYPE] = 1, [KFS_FEATURE_FREESPACE] = 1, [KFS_FEATURE_FILE_LFS] = 1, [KFS_FEATURE_BLOCKSIZE] = 1, [KFS_FEATURE_DEVSIZE] = 1};

static size_t wholedisk_get_max_feature_id(LFS_t * object)
{
	return sizeof(wholedisk_features) / sizeof(wholedisk_features[0]) - 1;
}

static const bool * wholedisk_get_feature_array(LFS_t * object)
{
	return wholedisk_features;
}

static int wholedisk_get_metadata_inode(LFS_t * object, inode_t inode, uint32_t id, size_t size, void * data)
{
	if (id == KFS_FEATURE_SIZE)
	{
		if (size < sizeof(size_t))
			return -ENOMEM;
		size = sizeof(size_t);
		*((size_t *) data) = (inode == INODE_DISK) ? object->blocksize * object->blockdev->numblocks : 0;
	}
	else if (id == KFS_FEATURE_FILETYPE)
	{
		if (size < sizeof(int32_t))
			return -ENOMEM;
		size = sizeof(int32_t);
		*((int32_t *) data) = (inode == INODE_DISK) ? TYPE_DEVICE : TYPE_DIR;
	}
	else if (id == KFS_FEATURE_FREESPACE)
	{
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = 0;
	}
	else if (id == KFS_FEATURE_FILE_LFS)
	{
		if (size < sizeof(object))
			return -ENOMEM;
		size = sizeof(object);

		*((typeof(object) *) data) = object;
	}
	else if (id == KFS_FEATURE_BLOCKSIZE)
	{
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = object->blockdev->blocksize;
	}
	else if (id == KFS_FEATURE_DEVSIZE)
	{
		if (size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);

		*((uint32_t *) data) = object->blockdev->numblocks;
	}
	else
		return -EINVAL;

	return size;
}

static int wholedisk_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	inode_t inode = INODE_NONE;
	if(file == (fdesc_t *) &root_fdesc)
		inode = INODE_ROOT;
	else if(file == (fdesc_t *) &disk_fdesc)
		inode = INODE_DISK;
	return wholedisk_get_metadata_inode(object, inode, id, size, data);
}

static int wholedisk_set_metadata2(LFS_t * object, const fsmetadata_t *fsm, size_t nfsm, chdesc_t ** head)
{
	return -EINVAL;
}

static int wholedisk_set_metadata2_inode(LFS_t * object, inode_t inode, const fsmetadata_t *fsm, size_t nfsm, chdesc_t ** head)
{
	return wholedisk_set_metadata2(object, fsm, nfsm, head);
}

static int wholedisk_set_metadata2_fdesc(LFS_t * object, fdesc_t * file, const fsmetadata_t *fsm, size_t nfsm, chdesc_t ** head)
{
	return wholedisk_set_metadata2(object, fsm, nfsm, head);
}

static int wholedisk_destroy(LFS_t * lfs)
{
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(lfs->blockdev, lfs);
	
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

LFS_t * wholedisk(BD_t * bd)
{
	LFS_t * lfs = malloc(sizeof(*lfs));

	if(!lfs)
		return NULL;
	
	LFS_INIT(lfs, wholedisk);
	OBJMAGIC(lfs) = WHOLEDISK_MAGIC;
	
	lfs->blockdev = bd;
	lfs->blocksize = bd->blocksize;
	
	if(modman_add_anon_lfs(lfs, __FUNCTION__))
	{
		DESTROY(lfs);
		return NULL;
	}
	if(modman_inc_bd(bd, lfs, NULL) < 0)
	{
		modman_rem_lfs(lfs);
		DESTROY(lfs);
		return NULL;
	}
	
	return lfs;
}
