#include <lib/platform.h>
#include <lib/pool.h>

#include <fscore/modman.h>
#include <fscore/patch.h>
#include <fscore/debug.h>
#include <fscore/patchgroup.h>
#include <fscore/lfs.h>
#include <fscore/cfs.h>
#include <fscore/uhfs.h>


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
	feature_id_t size_id; /* metadata id for filesize, 0 if not supported */
	bool type; /* whether the type metadata is supported */
};
typedef struct uhfs_fdesc uhfs_fdesc_t;

struct uhfs_state {
	CFS_t cfs;
	
	LFS_t * lfs;
	patch_t ** write_head;
	uint32_t nopen;
};

DECLARE_POOL(uhfs_fdesc, uhfs_fdesc_t);
static int n_uhfs_instances;

static bool lfs_feature_supported(LFS_t * lfs, feature_id_t id)
{
	const size_t max_id = CALL(lfs, get_max_feature_id);
	const bool * id_array = CALL(lfs, get_feature_array);
	if(id > max_id)
		return 0;
	return id_array[id];
}

static bool check_type_supported(LFS_t * lfs, fdesc_t * f, uint32_t * filetype)
{
	const bool type_supported = lfs_feature_supported(lfs, FSTITCH_FEATURE_FILETYPE);

	if (type_supported)
	{
		int r = CALL(lfs, get_metadata_fdesc, f, FSTITCH_FEATURE_FILETYPE, sizeof(*filetype), filetype);
		if (r < 0)
			*filetype = TYPE_INVAL;
		else
			assert(r == sizeof(*filetype));
	}
	else
		*filetype = TYPE_INVAL;

	return type_supported;
}

static uhfs_fdesc_t * uhfs_fdesc_create(fdesc_t * inner, inode_t ino, feature_id_t size_id, bool type)
{
	// TODO: increase mem bucket resolution to diff 16 and 20B allocs?
	uhfs_fdesc_t * uf = uhfs_fdesc_alloc();
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
	uhfs_fdesc_free(uf);
}

static void uhfs_fdesc_close(struct uhfs_state * state, uhfs_fdesc_t * uf)
{
	CALL(state->lfs, free_fdesc, uf->inner);
	uhfs_fdesc_destroy(uf);
	state->nopen--;
}



static int uhfs_get_root(CFS_t * cfs, inode_t * ino)
{
	Dprintf("%s()\n", __FUNCTION__);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	return CALL(state->lfs, get_root, ino);
}

static int uhfs_lookup(CFS_t * cfs, inode_t parent, const char * name, inode_t * ino)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	return CALL(state->lfs, lookup_name, parent, name, ino);
}

static int uhfs_close(CFS_t * cfs, fdesc_t * fdesc)
{
	Dprintf("%s(%p)\n", __FUNCTION__, fdesc);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	uhfs_fdesc_close(state, uf);
	return 0;
}

static int uhfs_truncate(CFS_t * cfs, fdesc_t * fdesc, uint32_t target_size)
{
	Dprintf("%s(%p, 0x%x)\n", __FUNCTION__, fdesc, target_size);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	const size_t blksize = state->lfs->blocksize;
	size_t nblks, target_nblks = ROUNDUP32(target_size, blksize) / blksize;
	patch_t * prev_head = state->write_head ? *state->write_head : NULL;
	patch_t * save_head;
	int r;

	nblks = CALL(state->lfs, get_file_numblocks, uf->inner);

	/* Truncate and free the blocks no longer in use because of this trunc */
	for (; target_nblks < nblks; nblks--)
	{
		/* Truncate the block */
		uint32_t block = CALL(state->lfs, truncate_file_block, uf->inner, &prev_head);
		if (block == INVALID_BLOCK)
			return -1;

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

		if (target_size <= size)
		{
			fsmetadata_t fsm;
			fsm.fsm_feature = uf->size_id;
			fsm.fsm_value.u = target_size;
			r = CALL(state->lfs, set_metadata2_fdesc, uf->inner, &fsm, 1, &prev_head);
			if (r < 0)
				return r;
		}
	}

	return 0;
}

static int open_common(struct uhfs_state * state, fdesc_t * inner, inode_t ino, fdesc_t ** outer)
{
	feature_id_t size_id = FSTITCH_FEATURE_NONE;
	bool type = 0;
	uhfs_fdesc_t * uf;

	/* detect whether the filesize and filetype features are supported */
	{
		const size_t max_id = CALL(state->lfs, get_max_feature_id);
		const bool * id_array = CALL(state->lfs, get_feature_array);
		if(FSTITCH_FEATURE_SIZE <= max_id && id_array[FSTITCH_FEATURE_SIZE])
			size_id = FSTITCH_FEATURE_SIZE;
		if(FSTITCH_FEATURE_FILETYPE <= max_id && id_array[FSTITCH_FEATURE_FILETYPE])
			type = 1;
	}

	uf = uhfs_fdesc_create(inner, ino, size_id, type);
	if (!uf)
	{
		CALL(state->lfs, free_fdesc, inner);
		*outer = NULL;
		return -ENOMEM;
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
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	uint32_t filetype;
	fdesc_t * inner;
	int r;

	if ((mode & O_CREAT))
		return -EINVAL;

	/* look up the ino */
	inner = CALL(state->lfs, lookup_inode, ino);
	if (!inner)
		return -ENOENT;

	if ((mode & O_WRONLY) || (mode & O_RDWR))
	{
		if (check_type_supported(state->lfs, inner, &filetype))
		{
			if (filetype == TYPE_DIR)
				return -1; // -EISDIR
			else if (filetype == TYPE_INVAL)
				return -1; // This seems bad too
		}
	}

	r = open_common(state, inner, ino, fdesc);
	if (r < 0)
		return r;

	/* HACK: don't do this for wholedisk LFS modules */
	if ((mode & O_TRUNC) && OBJMAGIC(state->lfs) != WHOLEDISK_MAGIC)
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
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	patch_t * prev_head = state->write_head ? *state->write_head : NULL;
	inode_t existing_ino;
	fdesc_t * inner;
	int type, r;

	*newino = INODE_NONE;
	*fdesc = NULL;

	r = CALL(state->lfs, lookup_name, parent, name, &existing_ino);
	if (r >= 0)
	{
		fprintf(stderr, "%s(%u, \"%s\"): file already exists. What should we do? (Returning error)\n", __FUNCTION__, parent, name);
		return -EEXIST;
	}

	r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_FILETYPE, sizeof(type), &type);
	if (r < 0)
		return r;
	assert(type == TYPE_FILE || type == TYPE_SYMLINK);

	inner = CALL(state->lfs, allocate_name, parent, name, type, NULL, initialmd, newino, &prev_head);
	if (!inner)
		return -1;

	r = open_common(state, inner, *newino, fdesc);
	if (r < 0)
		*newino = INODE_NONE;
	return r;
}

static int uhfs_read(CFS_t * cfs, fdesc_t * fdesc, page_t * page, void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(cfs, %p, %p, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	const uint32_t blocksize = state->lfs->blocksize;
	const uint32_t blockoffset = offset - (offset % blocksize);
	const uint32_t pageoffset = offset & (PAGE_SIZE - 1);
	uint32_t dataoffset = (offset % blocksize);
	uint32_t size_read = 0;
	uint32_t file_size = -1;
	uint32_t filetype;

	if (check_type_supported(state->lfs, uf->inner, &filetype))
	{
		if (filetype == TYPE_DIR)
			return -1; // -EISDIR
		else if (filetype == TYPE_INVAL)
			return -1; // This seems bad too
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
		{
			bool in_first_page = (pageoffset + size_read) < PAGE_SIZE;
			page_t * cur_page = in_first_page ? page : NULL;
			block = CALL(state->lfs, lookup_block, number, cur_page);
		}
		if (!block)
			return size_read ? size_read : -1;

		limit = MIN(block->length - dataoffset, size - size_read);
		if (uf->size_id)
			if (offset + size_read + limit > file_size)
				limit = file_size - offset - size_read;

		memcpy((uint8_t*)data + size_read, bdesc_data(block) + dataoffset, limit);
		size_read += limit;
		/* dataoffset only needed for first block */
		dataoffset = 0;

		if (!limit)
			break;
	}

	return size_read ? size_read : (size ? -1 : 0);
}

static int uhfs_write(CFS_t * cfs, fdesc_t * fdesc, page_t * page, const void * data, uint32_t offset, uint32_t size)
{
	Dprintf("%s(%p, %p, 0x%x, 0x%x)\n", __FUNCTION__, fdesc, data, offset, size);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;
	BD_t * const bd = state->lfs->blockdev;
	const uint32_t blocksize = state->lfs->blocksize;
	const uint32_t blockoffset = offset - (offset % blocksize);
	const uint32_t pageoffset = offset & (PAGE_SIZE - 1);
	uint32_t dataoffset = (offset % blocksize);
	uint32_t size_written = 0, filesize = 0, target_size;
	patch_t * write_head = state->write_head ? *state->write_head : NULL;
	patch_t * head = write_head, * tail;
	int r = 0;

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
			r = uhfs_write(cfs, fdesc, NULL, NULL, filesize, offset - filesize);
			if (r < 0)
				return r;
			if (r == 0)
				return -1;
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
		patch_t * save_head;
		const uint32_t length = MIN(blocksize - dataoffset, size - size_written);
		bool in_first_page = (pageoffset + size_written) < PAGE_SIZE;
		page_t * cur_page = in_first_page ? page : NULL;
		head = write_head;

		number = CALL(state->lfs, get_file_block, uf->inner, blockoffset + (offset % blocksize) - dataoffset + size_written);
		if (number == INVALID_BLOCK)
		{
			number = CALL(state->lfs, allocate_block, uf->inner, 0, &head);
			if (number == INVALID_BLOCK)
			{
				r = -ENOSPC;
				goto uhfs_write_written_exit;
			}

			/* get the block to zero it */
			block = CALL(state->lfs, synthetic_lookup_block, number, cur_page);
			if (!block)
			{
				int t;
				no_block:
				head = write_head;
				t = CALL(state->lfs, free_block, uf->inner, number, &head);
				assert(t >= 0);
				if(size_written)
					goto uhfs_write_written_exit;
				goto uhfs_write_exit;
			}

			r = patchgroup_prepare_head(&head);
			/* can we do better than this? */
			assert(r >= 0);

			/* save the tail */
			tail = head;

			/* zero it */
			r = patch_create_init(block, bd, &head);
			if (r < 0)
				goto no_block;
			FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, head, "init data block");
			/* note that we do not write it - we will write it later */

			FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_FLAGS, head, PATCH_DATA);
			head->flags |= PATCH_DATA;

			r = patchgroup_finish_head(head);
			/* can we do better than this? */
			assert(r >= 0);

			/* append it to the file, depending on zeroing it */
			r = CALL(state->lfs, append_file_block, uf->inner, number, &head);
			if (r < 0)
			{
				/* we don't need to worry about unzeroing the
				 * block - it was free anyhow, so we can leave
				 * it alone if we write it as-is */
				head = write_head;
				CALL(state->lfs, write_block, block, number, &head);
				goto no_block;
			}

			/* the data written will end up depending on the zeroing
			 * automatically, so just use the previous head here */
			head = write_head;
		}
		else
		{
			if (length < blocksize)
			{
				block = CALL(state->lfs, lookup_block, number, cur_page);
				if (!block)
					goto uhfs_write_written_exit;
			}
			else
			{
				/* Since the entire block is to be overwritten we can
				 * avoid a read and do a synthetic read. */
				block = CALL(state->lfs, synthetic_lookup_block, number, cur_page);
				if (!block)
					goto uhfs_write_written_exit;
			}
		}

		r = patchgroup_prepare_head(&head);
		/* can we do better than this? */
		assert(r >= 0);

		/* save the tail */
		tail = head;

		/* write the data to the block */
		r = patch_create_byte(block, bd, dataoffset, length, data ? (uint8_t *) data + size_written : NULL, &head);
		if (r < 0)
			goto uhfs_write_written_exit;
		FSTITCH_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_PATCH_LABEL, head, "write file data");

		FSTITCH_DEBUG_SEND(KDB_MODULE_PATCH_ALTER, KDB_PATCH_SET_FLAGS, head, PATCH_DATA);
		head->flags |= PATCH_DATA;

		r = patchgroup_finish_head(head);
		/* can we do better than this? */
		assert(r >= 0);

		save_head = head;

		r = CALL(state->lfs, write_block, block, number, &head);
		assert(r >= 0);

		head = save_head;

		size_written += length;
		dataoffset = 0; /* dataoffset only needed for first block */
	}

	if (uf->size_id) {
		if (offset + size_written > target_size) {
			fsmetadata_t fsm;
			fsm.fsm_feature = uf->size_id;
			fsm.fsm_value.u = offset + size_written;
			r = CALL(state->lfs, set_metadata2_fdesc, uf->inner, &fsm, 1, &head);
			if (r < 0)
				goto uhfs_write_exit;
		}
	}

uhfs_write_written_exit:
	if(size_written)
		r = size_written;
uhfs_write_exit:
	return r;
}

static int uhfs_get_dirent(CFS_t * cfs, fdesc_t * fdesc, dirent_t * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("%s(%p, %p, %d, %p)\n", __FUNCTION__, fdesc, entry, size, basep);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	uhfs_fdesc_t * uf = (uhfs_fdesc_t *) fdesc;

	if (!size)
		return 0;
	return CALL(state->lfs, get_dirent, uf->inner, entry, size, basep);
}

static int unlink_file(CFS_t * cfs, inode_t ino, inode_t parent, const char * name, fdesc_t * f, patch_t ** prev_head)
{
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	const bool link_supported = lfs_feature_supported(state->lfs, FSTITCH_FEATURE_NLINKS);
	const bool delete_supported = lfs_feature_supported(state->lfs, FSTITCH_FEATURE_DELETE);
	int r;

	if (link_supported) {
		uint32_t nlinks;
		r = CALL(state->lfs, get_metadata_fdesc, f, FSTITCH_FEATURE_NLINKS, sizeof(nlinks), &nlinks);
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

	if (!delete_supported) {
		int i;
		patch_t * save_head;
		uint32_t nblocks = CALL(state->lfs, get_file_numblocks, f);
		for (i = 0 ; i < nblocks; i++) {
			uint32_t number = CALL(state->lfs, truncate_file_block, f, prev_head);
			if (number == INVALID_BLOCK) {
				CALL(state->lfs, free_fdesc, f);
				return -EINVAL;
			}

			save_head = *prev_head;

			r = CALL(state->lfs, free_block, f, number, prev_head);
			if (r < 0) {
				CALL(state->lfs, free_fdesc, f);
				return r;
			}

			*prev_head = save_head;
		}
	}

	CALL(state->lfs, free_fdesc, f);

	return CALL(state->lfs, remove_name, parent, name, prev_head);
}

static int unlink_name(CFS_t * cfs, inode_t parent, const char * name, patch_t ** head)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
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
		return -1;

	dir_supported = check_type_supported(state->lfs, f, &filetype);
	if (dir_supported) {
		if (filetype == TYPE_INVAL) {
			CALL(state->lfs, free_fdesc, f);
			return -1;
		}

		if (filetype == TYPE_DIR) {
			CALL(state->lfs, free_fdesc, f);
			return -EINVAL;
		}
	}

	return unlink_file(cfs, ino, parent, name, f, head);
}

static int uhfs_unlink(CFS_t * cfs, inode_t parent, const char * name)
{
	Dprintf("%s(%u, \"%s\")\n", __FUNCTION__, parent, name);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	patch_t * prev_head = state->write_head ? *state->write_head : NULL;
	return unlink_name(cfs, parent, name, &prev_head);
}

static int empty_get_metadata(void * arg, feature_id_t id, size_t size, void * data)
{
	return -ENOENT;
}

static int uhfs_link(CFS_t * cfs, inode_t ino, inode_t newparent, const char * newname)
{
	Dprintf("%s(%u, %u, \"%s\")\n", __FUNCTION__, ino, newparent, newname);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	inode_t newino;
	fdesc_t * oldf, * newf;
	bool type_supported;
	uint32_t oldtype;
	patch_t * prev_head = state->write_head ? *state->write_head : NULL;
	metadata_set_t initialmd = { .get = empty_get_metadata, .arg = NULL };
	int r;

	oldf = CALL(state->lfs, lookup_inode, ino);
	if (!oldf)
		return -1;

	type_supported = check_type_supported(state->lfs, oldf, &oldtype);
	/* determine old's type to set new's type */
	if (!type_supported)
		kpanic("%s() requires LFS filetype feature support to determine whether newname is to be a file or directory", __FUNCTION__);
	if (oldtype == TYPE_INVAL)
	{
		CALL(state->lfs, free_fdesc, oldf);
		return -1;
	}

	if (CALL(state->lfs, lookup_name, newparent, newname, &newino) >= 0)
	{
		CALL(state->lfs, free_fdesc, oldf);
		return -EEXIST;
	}

	newf = CALL(state->lfs, allocate_name, newparent, newname, oldtype, oldf, &initialmd, &newino, &prev_head);
	if (!newf)
	{
		CALL(state->lfs, free_fdesc, oldf);
		return -1;
	}

	if (type_supported)
	{
		fsmetadata_t fsm;
		fsm.fsm_feature = FSTITCH_FEATURE_FILETYPE;
		fsm.fsm_value.u = oldtype;
		r = CALL(state->lfs, set_metadata2_fdesc, newf, &fsm, 1, &prev_head);
		if (r < 0)
		{
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
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	patch_t * prev_head = state->write_head ? *state->write_head : NULL;
	inode_t ino;
	int r;

	r = CALL(state->lfs, lookup_name, newparent, newname, &ino);
	if (r < 0 && r != -ENOENT)
		return r;
	if (r >= 0)
	{
		// FIXME: does not atomically replace newparent/newname
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
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	patch_t * prev_head = state->write_head ? *state->write_head : NULL;
	inode_t existing_ino;
	fdesc_t * f;
	int r;

	if (CALL(state->lfs, lookup_name, parent, name, &existing_ino) >= 0)
		return -EEXIST;

	f = CALL(state->lfs, allocate_name, parent, name, TYPE_DIR, NULL, initialmd, ino, &prev_head);
	if (!f)
		return -1;

	/* set the filetype metadata */
	if (lfs_feature_supported(state->lfs, FSTITCH_FEATURE_FILETYPE))
	{
		fsmetadata_t fsm;
		fsm.fsm_feature = FSTITCH_FEATURE_FILETYPE;
		fsm.fsm_value.u = TYPE_DIR;
		r = CALL(state->lfs, set_metadata2_fdesc, f, &fsm, 1, &prev_head);
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
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	inode_t ino;
	bool dir_supported;
	fdesc_t * f;
	struct dirent entry;
	uint32_t filetype;
	uint32_t basep = 0;
	int r, retval = -EINVAL;

	r = CALL(state->lfs, lookup_name, parent, name, &ino);
	if (r < 0)
		return r;

	f = CALL(state->lfs, lookup_inode, ino);
	if (!f)
		return -1;
	f->common->parent = parent;

	dir_supported = check_type_supported(state->lfs, f, &filetype);
	if (dir_supported) {
		if (filetype == TYPE_INVAL) {
			CALL(state->lfs, free_fdesc, f);
			return -1;
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
					patch_t * prev_head = state->write_head ? *state->write_head : NULL;
					return unlink_file(cfs, ino, parent, name, f, &prev_head);
				}
			} while (r != 0);
			retval = -ENOTEMPTY;
		}
		else {
			retval = -ENOTDIR;
		}
	}

	CALL(state->lfs, free_fdesc, f);
	return retval;
}

static size_t uhfs_get_max_feature_id(CFS_t * cfs)
{
	Dprintf("%s(%u)\n", __FUNCTION__, ino);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	return CALL(state->lfs, get_max_feature_id);
}

static const bool * uhfs_get_feature_array(CFS_t * cfs)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, num);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	return CALL(state->lfs, get_feature_array);
}

static int uhfs_get_metadata(CFS_t * cfs, inode_t ino, uint32_t id, size_t size, void * data)
{
	Dprintf("%s(%u, 0x%x)\n", __FUNCTION__, ino, id);
	struct uhfs_state * state = (struct uhfs_state *) cfs;

	return CALL(state->lfs, get_metadata_inode, ino, id, size, data);
}

static int uhfs_set_metadata2(CFS_t * cfs, inode_t ino, const fsmetadata_t *fsm, size_t nfsm)
{
	Dprintf("%s(%u, 0x%x, 0x%x, %p)\n", __FUNCTION__, ino, id, nfsm, fsm);
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	patch_t * prev_head = state->write_head ? *state->write_head : NULL;

	return CALL(state->lfs, set_metadata2_inode, ino, fsm, nfsm, &prev_head);
}

static int uhfs_destroy(CFS_t * cfs)
{
	struct uhfs_state * state = (struct uhfs_state *) cfs;
	int r;

	if (state->nopen > 0)
		fprintf(stderr, "%s(%s): orphaning %u open fdescs\n", __FUNCTION__, modman_name_cfs(cfs), state->nopen);

	r = modman_rem_cfs(cfs);
	if(r < 0)
		return r;
	modman_dec_lfs(state->lfs, cfs);

	n_uhfs_instances--;
	if(!n_uhfs_instances)
		uhfs_fdesc_free_all();

	memset(state, 0, sizeof(*state));
	free(state);

	return 0;
}


CFS_t * uhfs(LFS_t * lfs)
{
	struct uhfs_state * state;
	CFS_t * cfs;

	state = malloc(sizeof(*state));
	if(!state)
		return NULL;
	cfs = &state->cfs;

	CFS_INIT(cfs, uhfs);
	OBJMAGIC(cfs) = UHFS_MAGIC;

	state->lfs = lfs;
	state->write_head = CALL(lfs, get_write_head);
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
	
	n_uhfs_instances++;
	
	return cfs;
}
