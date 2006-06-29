#include <inc/fd.h>
#include <inc/error.h>
#include <lib/assert.h>
#include <lib/fcntl.h>
#include <lib/panic.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>

#include <kfs/modman.h>
#include <kfs/chdesc.h>
#include <kfs/debug.h>
#include <kfs/opgroup.h>
#include <kfs/lfs.h>
#include <kfs/cfs.h>
#include <kfs/uhfs.h>


#define UHFS_DEBUG 0


#if UHFS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct uhfs_fdesc {
	fdesc_common_t * common;
	fdesc_t * inner;
	inode_t inode;
	uint32_t size_id; /* metadata id for filesize, 0 if not supported */
	bool type; /* whether the type metadata is supported */
};
typedef struct uhfs_fdesc uhfs_fdesc_t;

struct uhfs_state {
	LFS_t * lfs;
	uint32_t nopen;
};


static bool lfs_feature_supported(LFS_t * lfs, inode_t ino, int feature_id)
{
	const size_t num_features = CALL(lfs, get_num_features, ino);
	size_t i;

	for (i=0; i < num_features; i++)
		if (CALL(lfs, get_feature, ino, i)->id == feature_id)
			return 1;

	return 0;
}

static bool check_type_supported(LFS_t * lfs, inode_t ino, fdesc_t * f, uint32_t * filetype)
{
	const bool type_supported = lfs_feature_supported(lfs, ino, KFS_feature_filetype.id);

	if (type_supported)
	{
		int r = CALL(lfs, get_metadata_fdesc, f, KFS_feature_filetype.id, sizeof(*filetype), filetype);
		if (r < 0)
			*filetype = TYPE_INVAL;
		else
			assert(r == sizeof(*filetype));
	}
	else
		*filetype = TYPE_INVAL;

	return type_supported;
}

static uhfs_fdesc_t * uhfs_fdesc_create(fdesc_t * inner, inode_t ino, uint32_t size_id, bool type)
{
	uhfs_fdesc_t * uf = malloc(sizeof(*uf));
	if (!uf)
		return NULL;
	uf->common = inner->common;
	uf->inner = inner;
	uf->inode = ino;
	uf->size_id = size_id;
	uf->type = type;
	return uf;
}

static void uhfs_fdesc_destroy(uhfs_fdesc_t * uf)
{
	uf->common = NULL;
	uf->inner = NULL;
	uf->size_id = 0;
	uf->type = 0;
	free(uf);
}

static void uhfs_fdesc_close(struct uhfs_state * state, uhfs_fdesc_t * uf)
{
	CALL(state->lfs, free_fdesc, uf->inner);
	uhfs_fdesc_destroy(uf);
	state->nopen--;
}



static int uhfs_get_config(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != UHFS_MAGIC)
		return -E_INVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int uhfs_get_status(void * object, int level, char * string, size_t length)
{
	CFS_t * cfs = (CFS_t *) object;
	if(OBJMAGIC(cfs) != UHFS_MAGIC)
		return -E_INVAL;
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	
	snprintf(string, length, "open files: %u", state->nopen);
	return 0;
}

static int uhfs_get_root(CFS_t * cfs, inode_t * ino)
{
	Dprintf("%s()\n", __FUNCTION__);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	return CALL(state->lfs, get_root, ino);
}

static int uhfs_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	return CALL(state->lfs, lookup_name, parent, name, ino);
}

static int uhfs_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s(%p)\n", __FUNCTION__, fdesc);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	uhfs_fdesc_close(state, uf);
	return 0;
}

static int uhfs_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t target_size)
{
	Dprintf("%s(%p, 0x%x)\n", __FUNCTION__, fdesc, target_size);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	const size_t blksize = CALL(state->lfs, get_blocksize);
	size_t nblks, target_nblks = ROUNDUP32(target_size, blksize) / blksize;
	chdesc_t * prev_head = NULL, * save_head;
	int r;

	nblks = CALL(state->lfs, get_file_numblocks, uf->inner);

	/* Truncate and free the blocks no longer in use because of this trunc */
	for (; target_nblks < nblks; nblks--)
	{
		/* Truncate the block */
		uint32_t block = CALL(state->lfs, truncate_file_block, uf->inner, &prev_head);
		if (block == INVALID_BLOCK)
			return -E_UNSPECIFIED;

		save_head = prev_head;

		/* Now free the block */
		r = CALL(state->lfs, free_block, uf->inner, block, &prev_head);
		if (r < 0)
			return r;

		prev_head = save_head;
	}

	/* Update the file's size as recorded by lfs, which also updates
	   the byte-level size (rather than block-level in the above) */
	if (uf->size_id)
	{
		size_t size;

		r = CALL(state->lfs, get_metadata_fdesc, uf->inner, uf->size_id, sizeof(size), &size);
		if (r < 0)
			return r;
		assert(r == sizeof(size));

		if (target_size < size)
		{
			r = CALL(state->lfs, set_metadata_fdesc, uf->inner, uf->size_id, sizeof(target_size), &target_size, &prev_head);
			if (r < 0)
				return r;
		}
	}

	return 0;
}

static int open_common(struct uhfs_state * state, fdesc_t * inner, inode_t ino, fdesc_t ** outer)
{
	uint32_t size_id = 0;
	bool type = 0;
	uhfs_fdesc_t * uf;

	/* detect whether the filesize and filetype features are supported */
	{
		const size_t num_features = CALL(state->lfs, get_num_features, ino);
		size_t i;
		for (i=0; i < num_features; i++)
		{
			const feature_t * f = CALL(state->lfs, get_feature, ino, i);
			if (f->id == KFS_feature_size.id)
				size_id = KFS_feature_size.id;
			else if (f->id == KFS_feature_filetype.id)
				type = 1;

			if (size_id && type)
				break;
		}
	}

	uf = uhfs_fdesc_create(inner, ino, size_id, type);
	if (!uf)
	{
		CALL(state->lfs, free_fdesc, inner);
		*outer = NULL;
		return -E_NO_MEM;
	}

	state->nopen++;
	*outer = (fdesc_t *) uf;
	return 0;
}

// TODO:
// - respect mode
static int uhfs_open(CFS_t * cfs, inode_t ino, int mode, fdesc_t ** fdesc)
{
	Dprintf("%s(%u, %d)\n", __FUNCTION__, ino, mode);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	fdesc_t * inner;
	int r;
	uint32_t filetype;

	if ((mode & O_CREAT))
		return -E_INVAL;

	/* look up the ino */
	inner = CALL(state->lfs, lookup_inode, ino);
	if (!inner)
		return -E_NOT_FOUND;

	if ((mode & O_WRONLY) || (mode & O_RDWR))
	{
		if (check_type_supported(state->lfs, ino, inner, &filetype))
		{
			if (filetype == TYPE_DIR)
				return -E_UNSPECIFIED; // -E_EISDIR
			else if (filetype == TYPE_INVAL)
				return -E_UNSPECIFIED; // This seems bad too
		}
	}

	r = open_common(state, inner, ino, fdesc);
	if (r < 0)
		return r;

	/* HACK: don't do this for wholedisk LFS modules */
	if (mode & O_TRUNC && OBJMAGIC(state->lfs) != WHOLEDISK_MAGIC)
	{
		int s = uhfs_truncate(cfs, *fdesc, 0);
		if (s < 0)
		{
			uhfs_close(cfs, *fdesc);
			*fdesc = NULL;
			return s;
		}
	}

	return r;
}

static int uhfs_create(CFS_t * cfs, inode_t parent, const char * name, int mode, const metadata_set_t * initialmd, fdesc_t ** fdesc, inode_t * newino)
{
	Dprintf("%s(parent %u, name %s, %d)\n", __FUNCTION__, parent, name, mode);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	inode_t existing_ino;
	int type;
	chdesc_t * prev_head;
	fdesc_t * inner;
	int r;

	*newino = INODE_NONE;
	*fdesc = NULL;

	r = CALL(state->lfs, lookup_name, parent, name, &existing_ino);
	if (r >= 0)
	{
		kdprintf(STDERR_FILENO, "%s(%u, \"%s\"): file already exists. What should we do? (Returning error)\n", __FUNCTION__, parent, name);
		return -E_FILE_EXISTS;
	}

	r = initialmd->get(initialmd->arg, KFS_feature_filetype.id, sizeof(type), &type);
	if (r < 0)
		return r;
	assert(type == TYPE_FILE || type == TYPE_SYMLINK);

	prev_head = NULL;
	inner = CALL(state->lfs, allocate_name, parent, name, type, NULL, initialmd, newino, &prev_head);
	if (!inner)
		return -E_UNSPECIFIED;

	r = open_common(state, inner, *newino, fdesc);
	if (r < 0)
		*newino = INODE_NONE;
	return r;
}

static int uhfs_read(CFS_t * cfs, fdesc_t * fdesc, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(cfs, %p, %p, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	const uint32_t blocksize = CALL(state->lfs, get_blocksize);
	const uint32_t blockoffset = offset - (offset % blocksize);
	uint32_t dataoffset = (offset % blocksize);
	uint32_t size_read = 0;
	uint32_t file_size = -1;
	uint32_t filetype;

	if (check_type_supported(state->lfs, uf->inode, uf->inner, &filetype))
	{
		if (filetype == TYPE_DIR)
			return -E_UNSPECIFIED; // -E_EISDIR
		else if (filetype == TYPE_INVAL)
			return -E_UNSPECIFIED; // This seems bad too
	}

	/* if we have filesize, use it! */
	if (uf->size_id)
	{
		int r;
		r = CALL(state->lfs, get_metadata_fdesc, uf->inner, uf->size_id, sizeof(file_size), &file_size);
		if (r < 0)
			return r;
		assert(r == sizeof(file_size));
	}
	while (size_read < size)
	{
		uint32_t limit, number;
		bdesc_t * block = NULL;

		number = CALL(state->lfs, get_file_block, uf->inner, blockoffset + (offset % blocksize) - dataoffset + size_read);
		if (number != INVALID_BLOCK)
			block = CALL(state->lfs, lookup_block, number);
		if (!block)
			return size_read ? size_read : -E_EOF;

		limit = MIN(block->ddesc->length - dataoffset, size - size_read);
		if (uf->size_id)
			if (offset + size_read + limit > file_size)
				limit = file_size - offset - size_read;

		memcpy((uint8_t*)data + size_read, block->ddesc->data + dataoffset, limit);
		size_read += limit;
		/* dataoffset only needed for first block */
		dataoffset = 0;

		if (!limit)
			break;
	}

	return size_read ? size_read : (size ? -E_EOF : 0);
}

static void uhfs_mark_data(chdesc_t * head, chdesc_t * tail)
{
	chmetadesc_t * meta;
	if(head->flags & CHDESC_DATA)
		return;
	if(head->type != NOOP)
	{
		KFS_DEBUG_SEND(KDB_MODULE_CHDESC_ALTER, KDB_CHDESC_SET_FLAGS, head, CHDESC_DATA);
		head->flags |= CHDESC_DATA;
	}
	if(head == tail)
		return;
	for(meta = head->dependencies; meta; meta = meta->dependency.next)
		uhfs_mark_data(meta->dependency.desc, tail);
}

static int uhfs_write(CFS_t * cfs, fdesc_t * fdesc, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%p, %p, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	BD_t * const bd = CALL(state->lfs, get_blockdev);
	const uint32_t blocksize = CALL(state->lfs, get_blocksize);
	const uint32_t blockoffset = offset - (offset % blocksize);
	uint32_t dataoffset = (offset % blocksize);
	uint32_t size_written = 0, filesize = 0, target_size;
	chdesc_t * prev_head = NULL, * tail;
	int r;

	if (uf->size_id) {
		r = CALL(state->lfs, get_metadata_fdesc, uf->inner, uf->size_id, sizeof(filesize), &filesize);
		if (r < 0)
			goto uhfs_write_exit;
		assert(r == sizeof(filesize));
	}

	// FIXME: support lfses that do not support file_size
	target_size = filesize;

	// FIXME: support sparse files in some way
	if (offset > filesize) {
		while (offset > filesize)
		{
			r = uhfs_write(cfs, fdesc, NULL, filesize, offset - filesize);
			if (r < 0)
				return r;
			if (r == 0)
				return -E_UNSPECIFIED;
			filesize += r;
		}
	}

	// do we really want to just return size_written if an operation failed?
	// - yes, so that the caller knows what did get done successuflly
	// also, if something fails, do we still update filesize? - we should

	while (size_written < size)
	{
		uint32_t number;
		bdesc_t * block = NULL;
		chdesc_t * save_head;
		const uint32_t length = MIN(blocksize - dataoffset, size - size_written);

		number = CALL(state->lfs, get_file_block, uf->inner, blockoffset + (offset % blocksize) - dataoffset + size_written);
		if (number == INVALID_BLOCK)
		{
			bool synthetic;
			const int type = TYPE_FILE; /* TODO: can this be other types? */

			save_head = prev_head;
			prev_head = NULL; /* no need to link with previous chains here */

			number = CALL(state->lfs, allocate_block, uf->inner, type, &prev_head);
			if (number == INVALID_BLOCK)
				goto uhfs_write_written_exit;

			/* get the block to zero it */
			block = CALL(state->lfs, synthetic_lookup_block, number, &synthetic);
			if (!block)
			{
				no_block:
				prev_head = NULL;
				r = CALL(state->lfs, free_block, uf->inner, number, &prev_head);
				assert(r >= 0);
				goto uhfs_write_written_exit;
			}

			r = opgroup_prepare_head(&prev_head);
			/* can we do better than this? */
			assert(r >= 0);

			/* save the tail */
			tail = prev_head;

			/* zero it */
			r = chdesc_create_init(block, bd, &prev_head);
			if (r < 0)
			{
				if (synthetic)
					CALL(state->lfs, cancel_synthetic_block, number);
				goto no_block;
			}
			/* note that we do not write it - we will write it later */

			uhfs_mark_data(prev_head, tail);

			r = opgroup_finish_head(prev_head);
			/* can we do better than this? */
			assert(r >= 0);

			/* append it to the file, depending on zeroing it */
			r = CALL(state->lfs, append_file_block, uf->inner, number, &prev_head);
			if (r < 0)
			{
				/* we don't need to worry about unzeroing the
				 * block - it was free anyhow, so we can leave
				 * it alone if we write it as-is */
				prev_head = NULL;
				CALL(state->lfs, write_block, block, &prev_head);
				goto no_block;
			}

			/* the data written will end up depending on the zeroing
			 * automatically, so just use the previous head here */
			prev_head = save_head;
		}
		else
		{
			if (length < blocksize)
			{
				block = CALL(state->lfs, lookup_block, number);
				if (!block)
					goto uhfs_write_written_exit;
			}
			else
			{
				/* Since the entire block is to be overwritten we can
				 * avoid a read and do a synthetic read. However,
				 * we must init the disk so this introduces the possibility
				 * that we order writes to write the zeros and then the data.
				 * We could crash etc before the data write, corrupting the
				 * file data! On the other hand, this removes the need to
				 * read; a big win for randomized file overwritting. */
				bool synthetic;
				block = CALL(state->lfs, synthetic_lookup_block, number, &synthetic);
				if (!block)
				{
					if (synthetic)
						CALL(state->lfs, cancel_synthetic_block, number);
					goto uhfs_write_written_exit;
				}

				if (synthetic)
				{
					r = opgroup_prepare_head(&prev_head);
					/* can we do better than this? */
					assert(r >= 0);

					/* save the tail */
					tail = prev_head;

					r = chdesc_create_init(block, bd, &prev_head);
					if (r < 0)
						goto uhfs_write_written_exit;

					uhfs_mark_data(prev_head, tail);

					r = opgroup_finish_head(prev_head);
					/* can we do better than this? */
					assert(r >= 0);
				}
			}
		}

		r = opgroup_prepare_head(&prev_head);
		/* can we do better than this? */
		assert(r >= 0);

		/* save the tail */
		tail = prev_head;

		/* write the data to the block */
		r = chdesc_create_byte(block, bd, dataoffset, length, data ? (uint8_t *) data + size_written : NULL, &prev_head);
		if (r < 0)
			goto uhfs_write_written_exit;

		uhfs_mark_data(prev_head, tail);

		r = opgroup_finish_head(prev_head);
		/* can we do better than this? */
		assert(r >= 0);

		save_head = prev_head;

		r = CALL(state->lfs, write_block, block, &prev_head);
		assert(r >= 0);

		prev_head = save_head;

		size_written += length;
		dataoffset = 0; /* dataoffset only needed for first block */
	}

	if (uf->size_id) {
		if (offset + size_written > target_size) {
			target_size = offset + size_written;
			r = CALL(state->lfs, set_metadata_fdesc, uf->inner, uf->size_id, sizeof(target_size), &target_size, &prev_head);
			if (r < 0)
				goto uhfs_write_exit;
		}
	}

uhfs_write_written_exit:
	r = size_written;
uhfs_write_exit:
	return r;
}

static int uhfs_get_dirent(CFS_t * cfs, fdesc_t * fdesc, dirent_t * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("%s(%p, %p, %d, %p)\n", __FUNCTION__, fdesc, entry, size, basep);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;

	if (!size)
		return 0;
	return CALL(state->lfs, get_dirent, uf->inner, entry, size, basep);
}

static int unlink_file(CFS_t * cfs, inode_t ino, inode_t parent, const char * name, fdesc_t * f, chdesc_t ** prev_head)
{
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	const bool link_supported = lfs_feature_supported(state->lfs, ino, KFS_feature_nlinks.id);
	int i, r;
	uint32_t nblocks;
	uint32_t nlinks;
	chdesc_t ** save_head;

	if (link_supported) {
		r = CALL(state->lfs, get_metadata_fdesc, f, KFS_feature_nlinks.id, sizeof(nlinks), &nlinks);
		if (r < 0) {
			CALL(state->lfs, free_fdesc, f);
			return r;
		}
		assert(r == sizeof(nlinks));

		if (nlinks > 1) {
			CALL(state->lfs, free_fdesc, f);
			return CALL(state->lfs, remove_name, parent, name, prev_head);
		}
	}

	nblocks = CALL(state->lfs, get_file_numblocks, f);
	for (i = 0 ; i < nblocks; i++) {
		uint32_t number = CALL(state->lfs, truncate_file_block, f, prev_head);
		if (number == INVALID_BLOCK) {
			CALL(state->lfs, free_fdesc, f);
			return -E_INVAL;
		}

		save_head = prev_head;

		r = CALL(state->lfs, free_block, f, number, prev_head);
		if (r < 0) {
			CALL(state->lfs, free_fdesc, f);
			return r;
		}

		prev_head = save_head;
	}

	CALL(state->lfs, free_fdesc, f);

	return CALL(state->lfs, remove_name, parent, name, prev_head);
}

static int unlink_name(CFS_t * cfs, inode_t parent, const char * name, chdesc_t ** head)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	inode_t ino;
	bool dir_supported;
	fdesc_t * f;
	uint32_t filetype;
	int r;

	r = CALL(state->lfs, lookup_name, parent, name, &ino);
	if (r < 0)
		return r;

	f = CALL(state->lfs, lookup_inode, ino);
	if (!f)
		return -E_UNSPECIFIED;

	dir_supported = check_type_supported(state->lfs, ino, f, &filetype);
	if (dir_supported) {
		if (filetype == TYPE_INVAL) {
			CALL(state->lfs, free_fdesc, f);
			return -E_UNSPECIFIED;
		}

		if (filetype == TYPE_DIR) {
			CALL(state->lfs, free_fdesc, f);
			return -E_INVAL;
		}
	}

	return unlink_file(cfs, ino, parent, name, f, head);
}

static int uhfs_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	chdesc_t * prev_head = NULL;
	return unlink_name(cfs, parent, name, &prev_head);
}

static int empty_get_metadata(void * arg, uint32_t id, size_t size, void * data)
{
	return -E_NOT_FOUND;
}

static int uhfs_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, %u, \"%s\")\n", __FUNCTION__, ino, newparent, newname);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	inode_t newino;
	fdesc_t * oldf, * newf;
	bool type_supported;
	uint32_t oldtype;
	chdesc_t * prev_head = NULL;
	metadata_set_t initialmd = { .get = empty_get_metadata, .arg = NULL };
	int r;

	oldf = CALL(state->lfs, lookup_inode, ino);
	if (!oldf)
		return -E_UNSPECIFIED;

	type_supported = check_type_supported(state->lfs, ino, oldf, &oldtype);
	/* determine old's type to set new's type */
	if (!type_supported)
		panic("%s() requires LFS filetype feature support to determine whether newname is to be a file or directory", __FUNCTION__);
	{
		if (oldtype == TYPE_INVAL) {
			CALL(state->lfs, free_fdesc, oldf);
			return -E_UNSPECIFIED;
		}
	}

	if (CALL(state->lfs, lookup_name, newparent, newname, &newino) >= 0) {
		CALL(state->lfs, free_fdesc, oldf);
		return -E_FILE_EXISTS;
	}

	newf = CALL(state->lfs, allocate_name, newparent, newname, oldtype, oldf, &initialmd, &newino, &prev_head);
	if (!newf) {
		CALL(state->lfs, free_fdesc, oldf);
		return -E_UNSPECIFIED;
	}

	if (type_supported)
	{
		r = CALL(state->lfs, set_metadata_fdesc, newf, KFS_feature_filetype.id, sizeof(oldtype), &oldtype, &prev_head);
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

static int uhfs_rename(CFS_t * cfs, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, \"%s\", %u, \"%s\")\n", __FUNCTION__, oldparent, oldname, newparent, newname);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	inode_t ino;
	chdesc_t * prev_head = NULL;
	int r;

	r = CALL(state->lfs, lookup_name, newparent, newname, &ino);
	if (r < 0 && r != -E_NOT_FOUND)
		return r;
	if (r >= 0)
	{
		r = unlink_name(cfs, newparent, newname, &prev_head);
		if (r < 0)
			return r;
	}

	r = CALL(state->lfs, rename, oldparent, oldname, newparent, newname, &prev_head);
	if (r < 0)
		return r;

	return 0;
}

static int uhfs_mkdir(CFS_t * cfs, inode_t parent, const char * name, const metadata_set_t * initialmd, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	inode_t existing_ino;
	fdesc_t * f;
	chdesc_t * prev_head = NULL;
	int r;

	if (CALL(state->lfs, lookup_name, parent, name, &existing_ino) >= 0)
		return -E_FILE_EXISTS;

	f = CALL(state->lfs, allocate_name, parent, name, TYPE_DIR, NULL, initialmd, ino, &prev_head);
	if (!f)
		return -E_UNSPECIFIED;

	/* set the filetype metadata */
	if (lfs_feature_supported(state->lfs, *ino, KFS_feature_filetype.id))
	{
		const int type = TYPE_DIR;
		r = CALL(state->lfs, set_metadata_fdesc, f, KFS_feature_filetype.id, sizeof(type), &type, &prev_head);
		if (r < 0)
		{
			/* ignore remove_name() error in favor of the real error */
			CALL(state->lfs, free_fdesc, f);
			(void) CALL(state->lfs, remove_name, parent, name, &prev_head);
			return r;
		}
	}

	CALL(state->lfs, free_fdesc, f);

	return 0;
}

static int uhfs_rmdir(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	inode_t ino;
	bool dir_supported;
	fdesc_t * f;
	struct dirent entry;
	uint32_t filetype;
	uint32_t basep = 0;
	int r, retval = -E_INVAL;

	r = CALL(state->lfs, lookup_name, parent, name, &ino);
	if (r < 0)
		return r;

	f = CALL(state->lfs, lookup_inode, ino);
	if (!f)
		return -E_UNSPECIFIED;
	f->common->parent = parent;

	dir_supported = check_type_supported(state->lfs, ino, f, &filetype);
	if (dir_supported) {
		if (filetype == TYPE_INVAL) {
			CALL(state->lfs, free_fdesc, f);
			return -E_UNSPECIFIED;
		}

		if (filetype == TYPE_DIR) {
			do {
				r = CALL(state->lfs, get_dirent, f, &entry, sizeof(struct dirent), &basep);
				if (!strcmp(entry.d_name, ".")
					|| !strcmp(entry.d_name, "..")) {
					r = 1;
					entry.d_name[0] = 0;
				}
				if (r < 0) {
					chdesc_t * prev_head = NULL;
					return unlink_file(cfs, ino, parent, name, f, &prev_head);
				}
			} while (r != 0);
			retval = -E_NOT_EMPTY;
		}
		else {
			retval = -E_NOT_DIR;
		}
	}

	CALL(state->lfs, free_fdesc, f);
	return retval;
}

static size_t uhfs_get_num_features(CFS_t * cfs, inode_t ino)
{
	Dprintf("%s(%u)\n", __FUNCTION__, ino);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);

	return CALL(state->lfs, get_num_features, ino);
}

static const feature_t * uhfs_get_feature(CFS_t * cfs, inode_t ino, size_t num)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, num);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);

   return CALL(state->lfs, get_feature, ino, num);
}

static int uhfs_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, void * data)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, id);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);

	return CALL(state->lfs, get_metadata_inode, ino, id, size, data);
}

static int uhfs_set_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, const void * data)
{
	Dprintf("%s(%u, 0x%x, 0x%x, %p)\n", __FUNCTION__, ino, id, size, data);
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	chdesc_t * prev_head = NULL;

	return CALL(state->lfs, set_metadata_inode, ino, id, size, data, &prev_head);
}

static int uhfs_destroy(CFS_t * cfs)
{
	struct uhfs_state * state = (struct uhfs_state *) OBJLOCAL(cfs);
	int r;

	if (state->nopen > 0)
		kdprintf(STDERR_FILENO, "%s(%s): orphaning %u open fdescs\n", __FUNCTION__, modman_name_cfs(cfs), state->nopen);

	r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;
	modman_dec_lfs(state->lfs, cfs);

	free(OBJLOCAL(cfs));
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
	{
		free(cfs);
		return NULL;
	}

	CFS_INIT(cfs, uhfs, state);
	OBJMAGIC(cfs) = UHFS_MAGIC;

	state->lfs = lfs;
	state->nopen = 0;

	if(modman_add_anon_cfs(cfs, __FUNCTION__))
	{
		DESTROY(cfs);
		return NULL;
	}
	if(modman_inc_lfs(lfs, cfs, NULL) < 0)
	{
		modman_rem_cfs(cfs);
		DESTROY(cfs);
		return NULL;
	}
	
	return cfs;
}
