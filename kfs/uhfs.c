#include <inc/lib.h>
#include <inc/hash_map.h>
#include <inc/malloc.h>
#include <inc/fd.h>

#include <kfs/fidman.h>
#include <kfs/chdesc.h>
#include <kfs/lfs.h>
#include <kfs/cfs.h>
#include <kfs/uhfs.h>


#define UHFS_DEBUG 0


#if UHFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct open_file {
	int fid;
	fdesc_t * fdesc;
	uint32_t size_id; /* metadata id for filesize, 0 if not supported */
	bool type; /* whether the type metadata is supported */
};
typedef struct open_file open_file_t;

struct uhfs_state {
	LFS_t * lfs;
	hash_map_t * open_files;
};


static bool lfs_feature_supported(LFS_t * lfs, const char * name, int feature_id)
{
	const size_t num_features = CALL(lfs, get_num_features, name);
	size_t i;

	for (i=0; i < num_features; i++)
		if (CALL(lfs, get_feature, name, i)->id == feature_id)
			return 1;

	return 0;
}

static open_file_t * open_file_create(int fid, fdesc_t * fdesc, uint32_t size_id, bool type)
{
	open_file_t * f = malloc(sizeof(*f));
	if (!f)
		return NULL;
	f->fid = fid;
	f->fdesc = fdesc;
	f->size_id = size_id;
	f->type = type;
	return f;
}

static void open_file_destroy(open_file_t * f)
{
	f->fid = -1;
	f->fdesc = NULL;
	f->size_id = 0;
	f->type = 0;
	free(f);
}

static void open_file_close(LFS_t * lfs, open_file_t * f)
{
	int r;
	CALL(lfs, free_fdesc, f->fdesc);

	r = release_fid(f->fid);
	assert(0 <= r);

	open_file_destroy(f);
}



// TODO:
// - respect mode
static int uhfs_open(CFS_t * cfs, const char * name, int mode)
{
	Dprintf("%s(\"%s\", %d)\n", __FUNCTION__, name, mode);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	fdesc_t * fdesc;
	uint32_t size_id = 0;
	bool type = 0;
	int fid;
	open_file_t * f;
	int r;
	
	/* look up the name */
	fdesc = CALL(state->lfs, lookup_name, name);

	if ((mode & O_CREAT) && !fdesc)
	{
		const uint32_t blksize = CALL(state->lfs, get_blocksize);
		chdesc_t * head, * tail;
		bdesc_t * blk;

		head = tail = NULL;
		fdesc = CALL(state->lfs, allocate_name, name, 0, NULL, &head, &tail);
		if (!fdesc)
			return -E_UNSPECIFIED;

		tail = NULL;
		blk = CALL(state->lfs, allocate_block, blksize, 0, &head, &tail);
		if (!blk)
			return -E_UNSPECIFIED;

		tail = NULL;
		r = CALL(state->lfs, append_file_block, fdesc, blk, &head, &tail);
		if (r < 0)
			return r;
	}
	else
		if (!fdesc)
			return -E_NOT_FOUND;
	
	/* detect whether the filesize and filetype features are supported */
	{
		const size_t num_features = CALL(state->lfs, get_num_features, name);
		size_t i;
		for (i=0; i < num_features; i++)
		{
			const feature_t * f = CALL(state->lfs, get_feature, name, i);
			if (f->id == KFS_feature_size.id)
				size_id = KFS_feature_size.id;
			else if (f->id == KFS_feature_filetype.id)
				type = 1;

			if (size_id && type)
				break;
		}
	}


	fid = create_fid();
	if (fid < 0) {
		CALL(state->lfs, free_fdesc, fdesc);
		return fid;
	}
	f = open_file_create(fid, fdesc, size_id, type);
	if (!f) {
		CALL(state->lfs, free_fdesc, fdesc);
		return -E_NO_MEM;
	}
	r = hash_map_insert(state->open_files, (void*) fid, f);
	if (r < 0)
	{
		open_file_destroy(f);
		CALL(state->lfs, free_fdesc, fdesc);
		return -E_NO_MEM;
	}

	return fid;
}

static int uhfs_close(CFS_t * cfs, int fid)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, fid);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	open_file_t * f;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;

	open_file_close(state->lfs, f);
	return 0;
}

static int uhfs_read(CFS_t * cfs, int fid, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(cfs, 0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	open_file_t * f;
	const uint32_t blocksize = CALL(state->lfs, get_blocksize);
	const uint32_t blockoffset = offset - (offset % blocksize);
	uint32_t dataoffset = (offset % blocksize);
	bdesc_t * bd;
	uint32_t size_read = 0;
	uint32_t file_size = -1;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;

	/* if we have filesize, use it! */
	if (f->size_id)
	{
		size_t md_size;
		void * data;
		int r;
		if ((r = CALL(state->lfs, get_metadata_fdesc, f->fdesc, f->size_id, &md_size, &data)) < 0)
			return r;
		assert(md_size == sizeof(file_size));
		file_size = *((uint32_t *) data);
		free(data);
	}
	while (size_read < size)
	{
		uint32_t limit;
		
		bd = CALL(state->lfs, get_file_block, f->fdesc, blockoffset + (offset % blocksize) - dataoffset + size_read);
		if (!bd)
			return size_read ? size_read : -E_EOF;

		limit = MIN(bd->length - dataoffset, size - size_read);
		if (f->size_id)
			if (offset + size_read + limit > file_size)
				limit = file_size - offset - size_read;
		
		memcpy((uint8_t*)data + size_read, bd->ddesc->data + dataoffset, limit);
		size_read += limit;
		/* dataoffset only needed for first block */
		dataoffset = 0;

		bdesc_drop(&bd);
		if (!limit)
			break;
	}

	return size_read ? size_read : (size ? -E_EOF : 0);
}

static int uhfs_write(CFS_t * cfs, int fid, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(0x%x, 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, fid, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	open_file_t * f;
	const uint32_t blocksize = CALL(state->lfs, get_blocksize);
	const uint32_t blockoffset = offset - (offset % blocksize);
	uint32_t dataoffset = (offset % blocksize);
	bdesc_t * bd;
	uint32_t size_written = 0;
	chdesc_t * prevhead = NULL;
	chdesc_t * tail;
	int r;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;

	while (size_written < size)
	{
		/* get the block to write to - maybe just get a block number in the future, if we are writing the whole block? */
		bd = CALL(state->lfs, get_file_block, f->fdesc, blockoffset + (offset % blocksize) - dataoffset + size_written);
		if (!bd)
		{
			const int type = TYPE_FILE; /* TODO: can this be other types? */
			prevhead = NULL; /* no need to link with previous chains here */
			bd = CALL(state->lfs, allocate_block, blocksize, type, &prevhead, &tail);
			if (!bd)
				return size_written;
			// TODO allocated block needs to be appended to a file
		}

		/* write the data to the block */
		tail = NULL;
		const uint32_t n = MIN(bd->length - dataoffset, size - size_written);
		r = CALL(state->lfs, write_block, bd, dataoffset, n, (uint8_t*)data + size_written, &prevhead, &tail);
		if (r < 0)
			return size_written;
		size_written += n;
		dataoffset = 0; /* dataoffset only needed for first block */
	}

	return size_written;
}

static int uhfs_getdirentries(CFS_t * cfs, int fid, char * buf, int nbytes, uint32_t * basep)
{
	Dprintf("%s(%d, 0x%x, %d, 0x%x)\n", __FUNCTION__, fid, buf, nbytes, basep);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	open_file_t * f;
	uint32_t i;
	int nbytes_read = 0;
	int r = 0;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;

	for (i=0; nbytes_read < nbytes; i++)
	{
		r = CALL(state->lfs, get_dirent, f->fdesc, (dirent_t *) buf, nbytes - nbytes_read, basep);
		if (r < 0)
			goto exit;
		nbytes_read += ((dirent_t *) buf)->d_reclen;
		buf += ((dirent_t *) buf)->d_reclen;
	}

  exit:
	if (!nbytes || nbytes_read > 0)
		return nbytes_read;
	else
		return r;
}

static int uhfs_truncate(CFS_t * cfs, int fid, uint32_t target_size)
{
	Dprintf("%s(%d, 0x%x)\n", __FUNCTION__, fid, target_size);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	const size_t blksize = CALL(state->lfs, get_blocksize);
	open_file_t * f;
	size_t nblks;
	size_t target_nblks = ROUNDUP32(target_size, blksize) / blksize;
	bdesc_t * block;
	chdesc_t * prevhead = NULL;
	chdesc_t * tail;
	int r;

	f = hash_map_find_val(state->open_files, (void*) fid);
	if (!f)
		return -E_INVAL;

	nblks = CALL(state->lfs, get_file_numblocks, f->fdesc);

	/* Truncate and free the blocks no longer in use because of this trunc */
	for (; target_nblks < nblks; nblks--)
	{
		/* Truncate the block */
		tail = NULL;
		block = CALL(state->lfs, truncate_file_block, f->fdesc, &prevhead, &tail);
		if (!block)
			return -E_UNSPECIFIED;

		/* Now free the block */
		tail = NULL;
		r = CALL(state->lfs, free_block, block, &prevhead, &tail);
		if (r < 0)
			return r;
	}

	/* Update the file's size as recorded by lfs, which also updates
	   the byte-level size (rather than block-level in the above) */
	if (f->size_id)
	{
		void * data;
		size_t data_len;
		size_t size;

		r = CALL(state->lfs, get_metadata_fdesc, f->fdesc, f->size_id, &data_len, &data);
		if (r < 0)
			return r;
		assert(data_len == sizeof(target_size));
		size = *(size_t *) data;
		free(data);

		if (target_size < size)
		{
			tail = NULL;
			r = CALL(state->lfs, set_metadata_fdesc, f->fdesc, f->size_id, sizeof(target_size), &target_size, &prevhead, &tail);
			if (r < 0)
				return r;
		}
	}

	return 0;
}

// FIXME don't remove directories
static int uhfs_unlink(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	// 1. See if this is the last link, in which case truncate to zero?
	// 2. remove_name() [which does free_fdesc()]
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	const bool link_supported = lfs_feature_supported(state->lfs, name, KFS_feature_nlinks.id);
	fdesc_t * f;
	int r;
	uint32_t nlinks;
	size_t data_len;
	void * data;
	bdesc_t * blk;

	f = CALL(state->lfs, lookup_name, name);
	if (!f)
		return -E_NOT_FOUND;

	if (link_supported) {
		r = CALL(state->lfs, get_metadata_fdesc, f, KFS_feature_nlinks.id, &data_len, &data);
		if (r < 0) {
			CALL(state->lfs, free_fdesc, f);
			return r;
		}

		assert(data_len == sizeof(nlinks));
		nlinks = *(uint32_t *) data;
		free(data);

		if (nlinks > 1) {
			CALL(state->lfs, free_fdesc, f);
			return CALL(state->lfs, remove_name, name, NULL, NULL); // FIXME chdesc
		}
	}

	do {
		blk = CALL(state->lfs, truncate_file_block, f, NULL, NULL); // FIXME chdesc
		if (!blk)
			break;
		r = CALL(state->lfs, free_block, blk, NULL, NULL); // FIXME chdesc
		if (r < 0) {
			CALL(state->lfs, free_fdesc, f);
			return r;
		}
	} while (blk);

	CALL(state->lfs, free_fdesc, f);
	return CALL(state->lfs, remove_name, name, NULL, NULL); // FIXME chdesc
}

static int uhfs_link(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	fdesc_t * oldf, * newf, * f;
	const bool type_supported = lfs_feature_supported(state->lfs, oldname, KFS_feature_filetype.id);
	uint32_t oldtype;
	chdesc_t * prevhead;
	chdesc_t * tail;
	int r;

	oldf = CALL(state->lfs, lookup_name, oldname);
	if (!oldf)
		return -E_NOT_FOUND;

	/* determine oldname's type to set newname's type */
	if (!type_supported)
		panic("%s() requires LFS filetype feature support to determine whether newname is to be a file or directory", __FUNCTION__);
	{
		void * data;
		size_t data_len;
		r= CALL(state->lfs, get_metadata_fdesc, oldf, KFS_feature_filetype.id, &data_len, &data);
		if (r < 0) {
			CALL(state->lfs, free_fdesc, oldf);
			return r;
		}

		assert(data_len == sizeof(oldtype));
		oldtype = *(uint32_t *) data;
		free(data);
	}

	if ((f = CALL(state->lfs, lookup_name, newname))) {
		CALL(state->lfs, free_fdesc, f);
		CALL(state->lfs, free_fdesc, oldf);
		return -E_FILE_EXISTS;
	}
	CALL(state->lfs, free_fdesc, f);

	tail = NULL;
	newf = CALL(state->lfs, allocate_name, newname, oldtype, oldf, &prevhead, &tail);
	if (!newf) {
		CALL(state->lfs, free_fdesc, oldf);
		return -E_UNSPECIFIED;
	}

	if (type_supported)
	{
		tail = NULL;
		r = CALL(state->lfs, set_metadata_fdesc, newf, KFS_feature_filetype.id, sizeof(oldtype), &oldtype, &prevhead, &tail);
		if (r < 0) {
			CALL(state->lfs, free_fdesc, oldf);
			CALL(state->lfs, free_fdesc, newf);
			return r;
		}
	}
	CALL(state->lfs, free_fdesc, oldf);
	CALL(state->lfs, free_fdesc, newf);

	return 0;
}

static int uhfs_rename(CFS_t * cfs, const char * oldname, const char * newname)
{
	Dprintf("%s(\"%s\", \"%s\")\n", __FUNCTION__, oldname, newname);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	chdesc_t * prevhead = NULL, * tail = NULL;
	int r;

	r = CALL(state->lfs, rename, oldname, newname, &prevhead, &tail);
	if (r < 0)
		return r;

	return 0;
}

static int uhfs_mkdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	fdesc_t * f;
	chdesc_t * prevhead = NULL;
	chdesc_t * tail;
	int r;

	if ((f = CALL(state->lfs, lookup_name, name))) {
		CALL(state->lfs, free_fdesc, f);
		return -E_FILE_EXISTS;
	}

	tail = NULL;
	f = CALL(state->lfs, allocate_name, name, TYPE_DIR, NULL, &prevhead, &tail);
	if (!f)
		return -E_UNSPECIFIED;

	/* set the filetype metadata */
	if (lfs_feature_supported(state->lfs, name, KFS_feature_filetype.id))
	{
		const int type = TYPE_DIR;
		tail = NULL;
		r = CALL(state->lfs, set_metadata_fdesc, f, KFS_feature_filetype.id, sizeof(type), &type, &prevhead, &tail);
		if (r < 0)
		{
			/* ignore remove_name() error in favor of the real error */
			tail = NULL;
			CALL(state->lfs, free_fdesc, f);
			(void) CALL(state->lfs, remove_name, name, &prevhead, &tail);
			return r;
		}
	}

	CALL(state->lfs, free_fdesc, f);

	return 0;
}

// TODO: implement
static int uhfs_rmdir(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	// 1. ensure no entries?
	// 2. truncate?
	// 3. remove_name() [which does free_fdesc()]
	return -E_UNSPECIFIED;
}

static size_t uhfs_get_num_features(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

	return CALL(state->lfs, get_num_features, name);
}

static const feature_t * uhfs_get_feature(CFS_t * cfs, const char * name, size_t num)
{
	Dprintf("\"%s\", 0x%x)\n", __FUNCTION__, name, num);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

   return CALL(state->lfs, get_feature, name, num);
}

static int uhfs_get_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t * size, void ** data)
{
	Dprintf("%s(\"%s\", 0x%x)\n", __FUNCTION__, name, id);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

	return CALL(state->lfs, get_metadata_name, name, id, size, data);
}

static int uhfs_set_metadata(CFS_t * cfs, const char * name, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(\"%s\", 0x%x, 0x%x, 0x%x)\n", __FUNCTION__, name, id, size, data);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	chdesc_t * prevhead = NULL, * tail = NULL;
	int r;

	r = CALL(state->lfs, set_metadata_name, name, id, size, data, &prevhead, &tail);
	if (r < 0)
		return r;

	return r;
}

static int uhfs_sync(CFS_t * cfs, const char * name)
{
	Dprintf("%s(\"%s\")\n", __FUNCTION__, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;

	return CALL(state->lfs, sync, name);
}

static int uhfs_destroy(CFS_t * cfs)
{
	struct uhfs_state * state = (struct uhfs_state *) cfs->instance;
	hash_map_destroy(state->open_files);
	free(cfs->instance);
	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}


CFS_t * uhfs(LFS_t * lfs)
{
	struct uhfs_state * state;
	CFS_t * cfs;

	cfs = malloc(sizeof(*cfs));
	if(!cfs)
		return NULL;

	state = malloc(sizeof(*state));
	if(!state)
		goto error_uhfs;
	cfs->instance = state;

	ASSIGN(cfs, uhfs, open);
	ASSIGN(cfs, uhfs, close);
	ASSIGN(cfs, uhfs, read);
	ASSIGN(cfs, uhfs, write);
	ASSIGN(cfs, uhfs, getdirentries);
	ASSIGN(cfs, uhfs, truncate);
	ASSIGN(cfs, uhfs, unlink);
	ASSIGN(cfs, uhfs, link);
	ASSIGN(cfs, uhfs, rename);
	ASSIGN(cfs, uhfs, mkdir);
	ASSIGN(cfs, uhfs, rmdir);
	ASSIGN(cfs, uhfs, get_num_features);
	ASSIGN(cfs, uhfs, get_feature);
	ASSIGN(cfs, uhfs, get_metadata);
	ASSIGN(cfs, uhfs, set_metadata);
	ASSIGN(cfs, uhfs, sync);
	ASSIGN_DESTROY(cfs, uhfs, destroy);

	state->lfs = lfs;

	state->open_files = hash_map_create();
	if (!state->open_files)
		goto error_state;

	return cfs;

  error_state:
	free(state);
 error_uhfs:
	free(cfs);
	return NULL;
}
