#include <lib/platform.h>
#include <lib/vector.h>
#include <lib/dirent.h>

#include <kfs/chdesc.h>
#include <kfs/modman.h>
#include <kfs/cfs.h>
#include <kfs/bd.h>
#include <kfs/kfsd.h>
#include <kfs/devfs_cfs.h>

#define DEVFS_DEBUG 0

#if DEVFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

/* Idea: use index into bd_table as inode number... no need for a hash set. */

struct devfs_fdesc {
	fdesc_common_t * common;
	fdesc_common_t base;
	const char * name;
	union {
		inode_t inode;
		BD_t * bd;
		CFS_t * root;
	};
	int open_count;
};
typedef struct devfs_fdesc devfs_fdesc_t;

struct devfs_state {
	CFS_t cfs;
	
	vector_t * bd_table;
	devfs_fdesc_t root_fdesc;
};
typedef struct devfs_state devfs_state_t;


static devfs_fdesc_t * devfs_fdesc_create(inode_t parent, const char * name, BD_t * bd)
{
	devfs_fdesc_t * fdesc = malloc(sizeof(*fdesc));
	if(!fdesc)
		return NULL;
	
	fdesc->common = &fdesc->base;
	fdesc->base.parent = parent;
	fdesc->name = name;
	fdesc->bd = bd;
	fdesc->open_count = 0;
	return fdesc;
}

static BD_t * devfs_fdesc_destroy(devfs_fdesc_t * fdesc)
{
	BD_t * bd = fdesc->bd;
	fdesc->common = NULL;
	fdesc->name = NULL;
	fdesc->bd = NULL;
	fdesc->open_count = 0;
	free(fdesc);
	return bd;
}


static devfs_fdesc_t * devfd_lookup_name(devfs_state_t * state, const char * name, int * index)
{
	Dprintf("%s(0x%08x, \"%s\")\n", __FUNCTION__, state, name);
	const size_t bd_table_size = vector_size(state->bd_table);
	int i;
	
	for(i = 0; i < bd_table_size; i++)
	{
		devfs_fdesc_t * fdesc = (devfs_fdesc_t *) vector_elt(state->bd_table, i);
		if(!strcmp(fdesc->name, name))
		{
			if(index)
				*index = i;
			return fdesc;
		}
	}
	
	if(index)
		*index = -1;
	return NULL;
}

static devfs_fdesc_t * devfd_lookup_inode(devfs_state_t * state, inode_t inode)
{
	Dprintf("%s(0x%08x, %u)\n", __FUNCTION__, state, inode);
	const size_t bd_table_size = vector_size(state->bd_table);
	int i;
	
	for(i = 0; i < bd_table_size; i++)
	{
		devfs_fdesc_t * fdesc = (devfs_fdesc_t *) vector_elt(state->bd_table, i);
		if(fdesc->inode == inode)
			return fdesc;
	}
	
	if(state->root_fdesc.inode == inode)
		return &state->root_fdesc;
	
	return NULL;
}


static bool devfs_bd_in_use(BD_t * bd)
{
	int i;
	const modman_entry_bd_t * entry = modman_lookup_bd(bd);
	for(i = 0; i != vector_size(entry->users); i++)
		if(modman_lookup_bd(vector_elt(entry->users, i)))
			break;
	return i != vector_size(entry->users);
}

static int devfs_get_root(CFS_t * cfs, inode_t * inode)
{
	Dprintf("%s()\n", __FUNCTION__);
	devfs_state_t * state = (devfs_state_t *) cfs;
	*inode = state->root_fdesc.inode;
	return 0;
}

static int devfs_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * inode)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, inode, name);
	devfs_state_t * state = (devfs_state_t *) cfs;
	devfs_fdesc_t * fdesc;
	
	if(parent != state->root_fdesc.inode)
		return -EINVAL;
	
	if(!name[0] || !strcmp(name, "/"))
	{
		*inode = state->root_fdesc.inode;
		return 0;
	}
	
	if(name[0] == '/')
		name++;
	fdesc = devfd_lookup_name(state, name, NULL);
	if(!fdesc)
		return -ENOENT;
	
	*inode = fdesc->inode;
	return 0;
}

static int devfs_open(CFS_t * cfs, inode_t inode, int mode, fdesc_t ** fdesc)
{
	Dprintf("%s(%u, %d)\n", __FUNCTION__, inode, mode);
	devfs_state_t * state = (devfs_state_t *) cfs;
	devfs_fdesc_t * devfd;
	
	/* open / as a directory */
	if(inode == state->root_fdesc.inode)
	{
		*fdesc = (fdesc_t *) &state->root_fdesc;
		state->root_fdesc.open_count++;
		return 0;
	}
	
	devfd = devfd_lookup_inode(state, inode);
	if(!devfd)
		return -ENOENT;
	
	/* don't allow writing to a BD that is used by another BD */
	if((mode & O_ACCMODE) != O_RDONLY)
		if(devfs_bd_in_use(devfd->bd))
			return -EPERM;
	
	*fdesc = (fdesc_t *) devfd;
	devfd->open_count++;
	return 0;
}

static int devfs_create(CFS_t * cfs, inode_t parent, const char * name, int mode, const metadata_set_t * initialmd, fdesc_t ** fdesc, inode_t * new_inode)
{
	Dprintf("%s(%u, \"%s\", %d)\n", __FUNCTION__, parent, name, mode);
	return -EPERM;
}

static int devfs_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, fdesc);
	devfs_fdesc_t * devfd = (devfs_fdesc_t *) fdesc;
	if(!devfd->open_count)
		return -EINVAL;
	devfd->open_count--;
	return 0;
}

/* This function looks a lot like uhfs_read() */
static int devfs_read(CFS_t * cfs, fdesc_t * fdesc, page_t * page, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%08x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	devfs_fdesc_t * devfd = (devfs_fdesc_t *) fdesc;
	
	const uint32_t blocksize = devfd->bd->blocksize;
	const uint32_t blockoffset = offset - (offset % blocksize);
	const uint32_t file_size = blocksize * devfd->bd->numblocks;
	uint32_t size_read = 0, dataoffset = (offset % blocksize);
	bdesc_t * bdesc;

	if(file_size <= offset)
		return -1;
	if(offset + size > file_size)
		size = file_size - offset;
	while(size_read < size)
	{
		uint32_t limit;
		uint32_t read_byte = blockoffset + (offset % blocksize) - dataoffset + size_read;

		bdesc = CALL(devfd->bd, read_block, read_byte / blocksize, 1, page);
		if(!bdesc)
			return size_read ? size_read : -1;

		limit = MIN(bdesc->length - dataoffset, size - size_read);
		memcpy((uint8_t *) data + size_read, bdesc_data(bdesc) + dataoffset, limit);
		size_read += limit;
		/* dataoffset only needed for first block */
		dataoffset = 0;
	}

	return size_read ? size_read : (size ? -1 : 0);
}

static int devfs_write(CFS_t * cfs, fdesc_t * fdesc, page_t * page, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%08x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	devfs_fdesc_t * devfd = (devfs_fdesc_t *) fdesc;
	
	const uint32_t blocksize = devfd->bd->blocksize;
	const uint32_t blockoffset = offset - (offset % blocksize);
	const uint32_t file_size = blocksize * devfd->bd->numblocks;
	uint32_t size_written = 0, dataoffset = (offset % blocksize);
	bdesc_t * bdesc;
	int r = 0;

	/* don't allow writing to a BD that is used by another BD */
	if(devfs_bd_in_use(devfd->bd))
		return -EPERM;

	/* now do the actual write */
	if(file_size <= offset)
		return -1;
	if(offset + size > file_size)
		size = file_size - offset;
	while(size_written < size)
	{
		const uint32_t limit = MIN(blocksize - dataoffset, size - size_written);
		const uint32_t write_byte = blockoffset + (offset % blocksize) - dataoffset + size_written;
		const uint32_t blockno = write_byte / blocksize;
		chdesc_t * head = NULL;

		if(!dataoffset && limit == blocksize)
			/* we can do a synthetic read in this case */
			bdesc = CALL(devfd->bd, synthetic_read_block, blockno, 1, page);
		else
			bdesc = CALL(devfd->bd, read_block, blockno, 1, page);
		if(!bdesc)
			return size_written ? size_written : -1;
		r = chdesc_create_byte(bdesc, devfd->bd, dataoffset, limit, (uint8_t *) data + size_written, &head);
		if(r < 0)
			break;
		r = CALL(devfd->bd, write_block, bdesc, blockno);
		if(r < 0)
		{
			/* FIXME clean up chdescs */
			break;
		}

		size_written += limit;
		/* dataoffset only needed for first block */
		dataoffset = 0;
	}

	return size_written ? size_written : (size ? ((r < 0) ? r : -1) : 0);
}

static int devfs_get_dirent_helper(devfs_state_t * state, dirent_t * dirent, int nbytes, uint32_t * basep)
{
	const size_t size = vector_size(state->bd_table);
	uint16_t reclen = sizeof(*dirent) - sizeof(dirent->d_name) + 1;
	const char * name;
	inode_t inode;
	uint32_t filesize;
	uint8_t type;
	uint8_t namelen;
	
	if(*basep < 0 || size + 2 <= *basep)
		return -1;
	
	if(*basep == 0)
	{
		name = ".";
		inode = state->root_fdesc.inode;
		filesize = 0;
		type = TYPE_DIR;
	}
	else if(*basep == 1)
	{
		name = "..";
		inode = state->root_fdesc.inode;
		filesize = 0;
		type = TYPE_DIR;
	}
	else
	{
		devfs_fdesc_t * fdesc = (devfs_fdesc_t *) vector_elt(state->bd_table, *basep - 2);
		name = fdesc->name;
		inode = fdesc->inode;
		filesize = fdesc->bd->blocksize * fdesc->bd->numblocks;
		type = TYPE_DEVICE;
	}
	
	namelen = strlen(name);
	reclen += namelen;
	if(reclen > nbytes)
		return -EINVAL;
	
	(*basep)++;
	
	dirent->d_fileno = inode;
	dirent->d_filesize = filesize;
	dirent->d_reclen = reclen;
	dirent->d_type = type;
	dirent->d_namelen = namelen;
	strncpy(dirent->d_name, name, namelen + 1);
	
	return 0;
}

static int devfs_get_dirent(CFS_t * cfs, fdesc_t * fdesc, dirent_t * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("%s(0x%08x, 0x%x, %d, 0x%x)\n", __FUNCTION__, fdesc, entry, size, basep);
	devfs_state_t * state = (devfs_state_t *) cfs;
	
	/* check for the special file / */
	if(fdesc != (fdesc_t *) &state->root_fdesc)
		return -EINVAL;

	if(!size)
		return 0;
	return devfs_get_dirent_helper(state, entry, size, basep);
}

static int devfs_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t size)
{
	/* pretend to truncate successfully */
	return 0;
}

static int devfs_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	/* I suppose we could support removing block devices. It might pose some issues however. */
	return -EPERM;
}

static int devfs_link(CFS_t * cfs, inode_t inode, inode_t new_parent, const char * new_name)
{
	Dprintf("%s(%u, %u, \"%s\")\n", __FUNCTION__, inode, new_parent, new_name);
	return -EPERM;
}

static int devfs_rename(CFS_t * cfs, inode_t old_parent, const char * old_name, inode_t new_parent, const char * new_name)
{
	Dprintf("%s(%u, \"%s\", %u, \"%s\")\n", __FUNCTION__, old_parent, old_name, new_parent, new_name);
	/* I suppose we could support renaming block devices. It might pose some issues however. */
	return -EPERM;
}

static int devfs_mkdir(CFS_t * cfs, inode_t parent, const char * name, const metadata_set_t * initialmd, inode_t * inode)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	return -EPERM;
}

static int devfs_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	return -EPERM;
}

static const bool devfs_features[] = {[KFS_FEATURE_SIZE] = 1, [KFS_FEATURE_FILETYPE] = 1, [KFS_FEATURE_FREESPACE] = 1, [KFS_FEATURE_BLOCKSIZE] = 1, [KFS_FEATURE_DEVSIZE] = 1};

static size_t devfs_get_max_feature_id(CFS_t * cfs)
{
	Dprintf("%s()\n", __FUNCTION__);
	return sizeof(devfs_features) / sizeof(devfs_features[0]) - 1;
}

static const bool * devfs_get_feature_array(CFS_t * cfs)
{
	Dprintf("%s()\n", __FUNCTION__);
	return devfs_features;
}

static int devfs_get_metadata(CFS_t * cfs, inode_t inode, uint32_t id, size_t size, void * data)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, inode, id);
	devfs_state_t * state = (devfs_state_t *) cfs;
	devfs_fdesc_t * fdesc = NULL;

	if(inode && inode != state->root_fdesc.inode)
	{
		fdesc = devfd_lookup_inode(state, inode);
		if(!fdesc)
			return -ENOENT;
	}
	
	if(id == KFS_FEATURE_SIZE)
	{
		if(size < sizeof(size_t))
			return -ENOMEM;
		size = sizeof(size_t);
		*((size_t *) data) = fdesc ? fdesc->bd->blocksize * fdesc->bd->numblocks : 0;
	}
	else if(id == KFS_FEATURE_FILETYPE)
	{
		if(size < sizeof(int32_t))
			return -ENOMEM;
		size = sizeof(int32_t);
		*((int32_t *) data) = fdesc ? TYPE_DEVICE : TYPE_DIR;
	}
	else if(id == KFS_FEATURE_FREESPACE || id == KFS_FEATURE_BLOCKSIZE || id == KFS_FEATURE_DEVSIZE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = 0;
	}
	else
		return -EINVAL;
	
	return size;
}

static int devfs_set_metadata2(CFS_t * cfs, inode_t inode, const fsmetadata_t *fsm, size_t nfsm)
{
	Dprintf("%s(%u, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, inode, id, size, data);
	return -EPERM;
}

static void devfs_real_destroy(void * void_devfs_cfs)
{
	devfs_state_t * state = (devfs_state_t *) void_devfs_cfs;
	
	vector_destroy(state->bd_table);
	memset(state, 0, sizeof(*state));
	free(state);
}

static int devfs_destroy(CFS_t * cfs)
{
	/* FIXME: check open counts of all modules and the root! */
	int r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;

	r = kfsd_register_shutdown_module(devfs_real_destroy, cfs, SHUTDOWN_POSTMODULES);
	assert(!r);

	return 0;
}

CFS_t * devfs_cfs(const char * names[], BD_t * bds[], size_t num_entries)
{
	devfs_state_t * state;
	CFS_t * cfs;
	size_t i;
	int r;
	
	state = malloc(sizeof(*state));
	if(!state)
		return NULL;

	cfs = &state->cfs;
	CFS_INIT(cfs, devfs);
	OBJMAGIC(cfs) = DEVFS_MAGIC;
	
	state->root_fdesc.common = &state->root_fdesc.base;
	state->root_fdesc.base.parent = (inode_t) cfs;
	state->root_fdesc.root = cfs;
	state->root_fdesc.open_count = 0;
	
	state->bd_table = vector_create();
	if(!state->bd_table)
		goto error_cfs;
	
	for(i = 0; i < num_entries; i++)
		if((r = devfs_bd_add(cfs, names[i], bds[i])) < 0)
			goto error_bd_table;
	
	if(modman_add_anon_cfs(cfs, __FUNCTION__))
	{
		DESTROY(cfs);
		return NULL;
	}
	
	return cfs;
	
error_bd_table:
	vector_destroy(state->bd_table);
error_cfs:
	free(cfs);
	return NULL;
}

int devfs_bd_add(CFS_t * cfs, const char * name, BD_t * bd)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, bd);
	devfs_state_t * state = (devfs_state_t *) cfs;
	devfs_fdesc_t * fdesc;
	int r;
	
	/* make sure this is really a device FS */
	if(OBJMAGIC(cfs) != DEVFS_MAGIC)
		return -EINVAL;
	
	/* don't allow / in names */
	if(strchr(name, '/'))
		return -EINVAL;
	
	fdesc = devfd_lookup_name(state, name, NULL);
	if(fdesc)
		return -EBUSY;
	
	fdesc = devfs_fdesc_create(state->root_fdesc.inode, name, bd);
	if(!fdesc)
		return -ENOMEM;
	
	if((r = vector_push_back(state->bd_table, fdesc)) < 0)
	{
		devfs_fdesc_destroy(fdesc);
		return r;
	}
	
	if((r = modman_inc_bd(bd, cfs, name)) < 0)
	{
		vector_pop_back(state->bd_table);
		devfs_fdesc_destroy(fdesc);
		return r;
	}
	
	return 0;
}

// TODO: can this function take a module pointer instead of a name?
BD_t * devfs_bd_remove(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	devfs_state_t * state = (devfs_state_t *) cfs;
	devfs_fdesc_t * fdesc;
	int i;
	
	/* make sure this is really a device FS */
	if(OBJMAGIC(cfs) != DEVFS_MAGIC)
		return NULL;
	
	fdesc = devfd_lookup_name(state, name, &i);
	if(!fdesc)
		return NULL;
	
	if(fdesc->open_count)
		return NULL;
	
	vector_erase(state->bd_table, i);
	
	modman_dec_bd(fdesc->bd, cfs);
	
	return devfs_fdesc_destroy(fdesc);
}
