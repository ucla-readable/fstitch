#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/hash_map.h>
#include <inc/vector.h>
#include <inc/dirent.h>

#include <kfs/fidman.h>
#include <kfs/chdesc.h>
#include <kfs/depman.h>
#include <kfs/cfs.h>
#include <kfs/bd.h>
#include <kfs/devfs_cfs.h>

#define DEVFS_DEBUG 0

#if DEVFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct bd_entry {
	const char * name;
	BD_t * bd;
	int fid, open_count;
};
typedef struct bd_entry bd_entry_t;

/* "BDACCESS" */
#define DEVFS_MAGIC 0xBDACCE55

struct devfs_state {
	uint32_t magic;
	vector_t * bd_table;
	hash_map_t * fid_map;
	int root_fid, open_count;
};
typedef struct devfs_state devfs_state_t;


static bd_entry_t * bd_entry_create(const char * name, BD_t * bd)
{
	bd_entry_t * bde = malloc(sizeof(*bde));
	if(!bde)
		return NULL;
	
	bde->name = name;
	bde->bd = bd;
	bde->fid = -1;
	bde->open_count = 0;
	return bde;
}

static BD_t * bd_entry_destroy(bd_entry_t * bde)
{
	BD_t * bd = bde->bd;
	bde->name = NULL;
	bde->bd = NULL;
	bde->fid = -1;
	bde->open_count = 0;
	free(bde);
	return bd;
}


static int bde_map_fid(devfs_state_t * state, bd_entry_t * bde)
{
	return hash_map_insert(state->fid_map, (void *) bde->fid, bde);
}

static int bde_unmap_fid(devfs_state_t * state, bd_entry_t * bde)
{
	return -(bde == hash_map_erase(state->fid_map, (void *) bde->fid));
}

static bd_entry_t * bde_lookup_fid(devfs_state_t * state, int fid)
{
	return (bd_entry_t *) hash_map_find_val(state->fid_map, (void *) fid);
}

static bd_entry_t * bde_lookup_name(devfs_state_t * state, const char * name)
{
	Dprintf("%s(0x%08x, \"%s\")\n", __FUNCTION__, bd_table, name);
	const size_t bd_table_size = vector_size(state->bd_table);
	int i;
	
	for(i = 0; i < bd_table_size; i++)
	{
		bd_entry_t * bde = (bd_entry_t *) vector_elt(state->bd_table, i);
		if(!strcmp(bde->name, name))
			return bde;
	}
	
	return NULL;
}

static int bde_lookup_index(devfs_state_t * state, const char * name)
{
	Dprintf("%s(0x%08x, \"%s\")\n", __FUNCTION__, bd_table, name);
	const size_t bd_table_size = vector_size(state->bd_table);
	int i;
	
	for(i = 0; i < bd_table_size; i++)
	{
		const bd_entry_t * bde = (bd_entry_t *) vector_elt(state->bd_table, i);
		if(!strcmp(bde->name, name))
			return i;
	}
	
	return -E_NOT_FOUND;
}


static int devfs_open(CFS_t * cfs, const char * name, int mode)
{
	Dprintf("%s(\"%s\", %d)\n", __FUNCTION__, name, mode);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde;
	int r;
	
	/* open / as a directory */
	if(!name[0] || !strcmp(name, "/"))
	{
		if(state->root_fid == -1)
		{
			assert(!state->open_count);
			state->root_fid = create_fid();
		}
		
		state->open_count++;
		return state->root_fid;
	}
	
	bde = bde_lookup_name(state, name);
	if(!bde)
		return -E_NOT_FOUND;
	
	if(bde->fid == -1)
	{
		assert(!bde->open_count);
		bde->fid = create_fid();
		if((r = bde_map_fid(state, bde)) < 0)
		{
			release_fid(bde->fid);
			bde->fid = -1;
		}
	}
	
	bde->open_count++;
	return bde->fid;
}

static int devfs_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(%d)\n", __FUNCTION__, fid);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde;
	
	/* close / as a directory */
	if(fid != -1 && fid == state->root_fid)
	{
		assert(state->open_count);
		if(!--state->open_count)
		{
			release_fid(state->root_fid);
			state->root_fid = -1;
		}
		
		return 0;
	}
	
	bde = bde_lookup_fid(state, fid);
	if(!bde)
		return -E_NOT_FOUND;
	
	assert(bde->open_count);
	if(!--bde->open_count)
	{
		bde_unmap_fid(state, bde);
		release_fid(bde->fid);
		bde->fid = -1;
	}
	
	return 0;
}

/* This function looks a lot like uhfs_read() */
static int devfs_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde = bde_lookup_fid(state, fid);
	
	if(bde)
	{
		const uint32_t blocksize = CALL(bde->bd, get_blocksize);
		const uint32_t blockoffset = offset - (offset % blocksize);
		const uint32_t file_size = blocksize * CALL(bde->bd, get_numblocks);
		uint32_t size_read = 0, dataoffset = (offset % blocksize);
		bdesc_t * bdesc;
		
		if(file_size <= offset)
			return -E_EOF;
		if(offset + size > file_size)
			size = file_size - offset;
		while(size_read < size)
		{
			uint32_t limit;
			uint32_t read_byte = blockoffset + (offset % blocksize) - dataoffset + size_read;
			
			bdesc = CALL(bde->bd, read_block, read_byte / blocksize);
			if(!bdesc)
				return size_read ? size_read : -E_EOF;
			
			limit = MIN(bdesc->length - dataoffset, size - size_read);
			memcpy((uint8_t *) data + size_read, bdesc->ddesc->data + dataoffset, limit);
			size_read += limit;
			/* dataoffset only needed for first block */
			dataoffset = 0;
			
			bdesc_drop(&bdesc);
		}
		
		return size_read ? size_read : (size ? -E_EOF : 0);
	}
	
	return -E_INVAL;
}

static int devfs_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%d, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde = bde_lookup_fid(state, fid);
	
	if(bde)
	{
		const uint32_t blocksize = CALL(bde->bd, get_blocksize);
		const uint32_t blockoffset = offset - (offset % blocksize);
		const uint32_t file_size = blocksize * CALL(bde->bd, get_numblocks);
		uint32_t size_written = 0, dataoffset = (offset % blocksize);
		bdesc_t * bdesc;
		
		if(file_size <= offset)
			return -E_EOF;
		if(offset + size > file_size)
			size = file_size - offset;
		while(size_written < size)
		{
			uint32_t limit;
			uint32_t write_byte = blockoffset + (offset % blocksize) - dataoffset + size_written;
			chdesc_t * head;
			chdesc_t * tail;
			
			bdesc = CALL(bde->bd, read_block, write_byte / blocksize);
			if(!bdesc)
				return size_written ? size_written : -E_EOF;
			
			limit = MIN(bdesc->length - dataoffset, size - size_written);
			if(chdesc_create_byte(bdesc, dataoffset, limit, (uint8_t *) data + size_written, &head, &tail))
			{
				bdesc_drop(&bdesc);
				break;
			}
			if(depman_add_chdesc(head))
			{
				/* FIXME kill change descriptors */
				bdesc_drop(&bdesc);
				break;
			}
			if(CALL(bde->bd, write_block, bdesc))
				break;
			
			size_written += limit;
			/* dataoffset only needed for first block */
			dataoffset = 0;
		}
		
		return size_written ? size_written : (size ? -E_EOF : 0);
	}
	
	return -E_INVAL;
}

static int devfs_get_dirent(devfs_state_t * state, dirent_t * dirent, int nbytes, uint32_t * basep)
{
	const size_t size = vector_size(state->bd_table);
	uint16_t reclen = sizeof(*dirent) - sizeof(dirent->d_name) + 1;
	uint8_t namelen;
	bd_entry_t * bde;
	
	if(*basep < 0 || size <= *basep)
		return -E_UNSPECIFIED;
	
	bde = (bd_entry_t *) vector_elt(state->bd_table, *basep);
	namelen = strlen(bde->name);
	reclen += namelen;
	if(reclen > nbytes)
		return -E_UNSPECIFIED;
	
	(*basep)++;
	
	dirent->d_fileno = (uint32_t) bde->bd;
	dirent->d_filesize = CALL(bde->bd, get_blocksize) * CALL(bde->bd, get_numblocks);
	dirent->d_reclen = reclen;
	dirent->d_type = TYPE_DEVICE;
	dirent->d_namelen = namelen;
	strncpy(dirent->d_name, bde->name, DIRENT_MAXNAMELEN);
	
	return 0;
}

/* This function looks a lot like uhfs_getdirentries() */
static int devfs_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	uint32_t i;
	int nbytes_read = 0;
	int r = 0;
	
	/* check for the special file / */
	if(!(fid != -1 && fid == state->root_fid))
		return -E_INVAL;
	
	for(i = 0; nbytes_read < nbytes; i++)
	{
		r = devfs_get_dirent(state, (dirent_t *) buf, nbytes - nbytes_read, basep);
		if(r < 0)
			goto exit;
		nbytes_read += ((dirent_t *) buf)->d_reclen;
		buf += ((dirent_t *) buf)->d_reclen;
	}
	
exit:
	if(!nbytes || nbytes_read > 0)
		return nbytes_read;
	else
		return r;
}

static int devfs_truncate(CFS_t * cfs, int fid, uint32_t size)
{
	return -E_INVAL;
}

static int devfs_unlink(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	/* I suppose we could support removing block devices. It might pose some issues however. */
	return -E_INVAL;
}

static int devfs_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	return -E_INVAL;
}

static int devfs_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	/* I suppose we could support renaming block devices. It might pose some issues however. */
	return -E_INVAL;
}

static int devfs_mkdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	return -E_INVAL;
}

static int devfs_rmdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	return -E_INVAL;
}

static const feature_t * devfs_features[] = {&KFS_feature_size, &KFS_feature_filetype};

static size_t devfs_get_num_features(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	return sizeof(devfs_features) / sizeof(devfs_features[0]);
}

static const feature_t * devfs_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, num);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde = bde_lookup_name(state, name);
	
	if(!bde || num < 0 || num >= sizeof(devfs_features) / sizeof(devfs_features[0]))
		return NULL;
	return devfs_features[num];
}

static int devfs_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, id);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde = NULL;
	
	/* check for the special file / */
	if(name[0] && strcmp(name, "/"))
	{
		bde = bde_lookup_name(state, name);
		if(!bde)
			return -E_NOT_FOUND;
	}
	
	if(id == KFS_feature_size.id)
	{
		const size_t file_size = bde ? CALL(bde->bd, get_blocksize) * CALL(bde->bd, get_numblocks) : 0;
		*data = malloc(sizeof(file_size));
		if(!*data)
			return -E_NO_MEM;
		*size = sizeof(file_size);
		memcpy(*data, &file_size, sizeof(file_size));
	}
	else if(id == KFS_feature_filetype.id)
	{
		const int32_t type = bde ? TYPE_DEVICE : TYPE_DIR;
		*data = malloc(sizeof(type));
		if(!*data)
			return -E_NO_MEM;
		*size = sizeof(type);
		memcpy(*data, &type, sizeof(type));
	}
	else
		return -E_INVAL;
	
	return 0;
}

static int devfs_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(\"%s\", 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, name, id, size, data);
	return -E_INVAL;
}

static int devfs_sync(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde;
	
	if(!name || !name[0])
	{
		const size_t bd_table_size = vector_size(state->bd_table);
		int i;
		
		/* FIXME save return values? */
		for(i = 0; i < bd_table_size; i++)
			CALL(((bd_entry_t *) vector_elt(state->bd_table, i))->bd, sync, NULL);
		
		return 0;
	}
	
	bde = bde_lookup_name(state, name);
	
	if(!bde)
		return -E_NOT_FOUND;
	
	return CALL(bde->bd, sync, NULL);
}

static int devfs_destroy(CFS_t * cfs)
{
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	
	hash_map_destroy(state->fid_map);
	vector_destroy(state->bd_table);
	memset(state, 0, sizeof(*state));
	free(state);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * devfs_cfs(const char * names[], BD_t * bds[], size_t num_entries)
{
	devfs_state_t * state;
	CFS_t * cfs;
	size_t i;
	int r;
	
	cfs = malloc(sizeof(*cfs));
	if(!cfs)
		return NULL;
	
	state = malloc(sizeof(*state));
	if(!state)
		goto error_cfs;
	cfs->instance = state;
	
	ASSIGN(cfs, devfs, open);
	ASSIGN(cfs, devfs, close);
	ASSIGN(cfs, devfs, read);
	ASSIGN(cfs, devfs, write);
	ASSIGN(cfs, devfs, getdirentries);
	ASSIGN(cfs, devfs, truncate);
	ASSIGN(cfs, devfs, unlink);
	ASSIGN(cfs, devfs, link);
	ASSIGN(cfs, devfs, rename);
	ASSIGN(cfs, devfs, mkdir);
	ASSIGN(cfs, devfs, rmdir);
	ASSIGN(cfs, devfs, get_num_features);
	ASSIGN(cfs, devfs, get_feature);
	ASSIGN(cfs, devfs, get_metadata);
	ASSIGN(cfs, devfs, set_metadata);
	ASSIGN(cfs, devfs, sync);
	ASSIGN_DESTROY(cfs, devfs, destroy);
	
	state->magic = DEVFS_MAGIC;
	state->root_fid = -1;
	state->open_count = 0;
	
	state->fid_map = hash_map_create();
	if(!state->fid_map)
		goto error_state;
	
	state->bd_table = vector_create();
	if(!state->bd_table)
		goto error_fid_map;
	
	for(i = 0; i < num_entries; i++)
		if((r = devfs_bd_add(cfs, names[i], bds[i])) < 0)
			goto error_bd_table;
	
	return cfs;
	
error_bd_table:
	vector_destroy(state->bd_table);
error_fid_map:
	hash_map_destroy(state->fid_map);
error_state:
	free(cfs->instance);
error_cfs:
	free(cfs);
	return NULL;
}

int devfs_bd_add(CFS_t * cfs, const char * name, BD_t * bd)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, path, path_cfs);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde;
	int r;
	
	/* make sure this is really a device FS */
	if (state->magic != DEVFS_MAGIC)
		return -E_INVAL;
	
	bde = bde_lookup_name(state, name);
	if(bde)
		return -E_INVAL;
	
	bde = bd_entry_create(name, bd);
	if(!bde)
		return -E_NO_MEM;
	
	if((r = vector_push_back(state->bd_table, bde)) < 0)
	{
		bd_entry_destroy(bde);
		return r;
	}
	
	fprintf(STDERR_FILENO, "devfs_cfs: new device %s\n", name);
	return 0;
}

BD_t * devfs_bd_remove(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, path);
	devfs_state_t * state = (devfs_state_t *) cfs->instance;
	bd_entry_t * bde;
	int i;
	
	/* make sure this is really a device FS */
	if(state->magic != DEVFS_MAGIC)
		return NULL;
	
	i = bde_lookup_index(state, name);
	if(i < 0)
		return NULL;
	bde = (bd_entry_t *) vector_elt(state->bd_table, i);
	
	fprintf(STDERR_FILENO, "devfs_cfs: removed device %s\n", name);
	if(bde->fid != -1)
	{
		bde_unmap_fid(state, bde);
		release_fid(bde->fid);
		bde->fid = -1;
		bde->open_count = 0;
	}
	vector_erase(state->bd_table, i);
	
	return bd_entry_destroy(bde);
}
