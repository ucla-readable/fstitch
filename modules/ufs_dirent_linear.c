/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>

#include <fscore/debug.h>

#include <modules/ufs_dirent_linear.h>

struct ufsmod_dirent_info {
	UFSmod_dirent_t ufsmod_dirent;

	struct ufs_info *info;
};

#define GET_UFS_INFO(object) (((struct ufsmod_dirent_info *) (object))->info)

static int read_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, struct UFS_direct * entry, uint32_t * basep)
{
	struct ufs_info * info = GET_UFS_INFO(object);
	struct UFS_direct * dirent;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, offset;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!entry)
		return -EINVAL;

	// Make sure it's a directory and we can read from it
	if (dirf->f_type != TYPE_DIR)
		return -ENOTDIR;

	if (*basep >= dirf->f_inode.di_size)
		return -1;

	blockno = CALL(info->parts.base, get_file_block, (fdesc_t *) dirf, ROUNDDOWN32(*basep, super->fs_fsize));
	if (blockno != INVALID_BLOCK)
		dirblock = CALL(info->parts.base, lookup_block, blockno, NULL);
	if (!dirblock)
		return -ENOENT;

	offset = *basep % super->fs_fsize;
	dirent = (struct UFS_direct *) (bdesc_data(dirblock) + offset);

	if (offset + dirent->d_reclen > super->fs_fsize
			|| dirent->d_reclen < dirent->d_namlen)
		return -1;

	entry->d_ino = dirent->d_ino;
	entry->d_reclen = dirent->d_reclen;
	entry->d_type = dirent->d_type;
	entry->d_namlen = dirent->d_namlen;
	strncpy(entry->d_name, dirent->d_name, dirent->d_namlen);
	entry->d_name[dirent->d_namlen] = 0;

	*basep += dirent->d_reclen;
	return 0;
}

// Writes a directory entry, does not check for free space
static int write_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, struct UFS_direct entry, uint32_t basep, patch_t ** head)
{
	struct ufs_info * info = GET_UFS_INFO(object);
	bdesc_t * block;
	uint32_t foffset, blockno;
	uint16_t offset, actual_len;
	int r;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || !dirf)
		return -EINVAL;

	actual_len = sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN;

	offset = basep % super->fs_fsize;
	foffset = basep - offset;
	blockno = CALL(info->parts.base, get_file_block, (fdesc_t *) dirf, foffset);
	if (blockno == INVALID_BLOCK)
		return -ENOENT;
	block = CALL(info->ubd, read_block, blockno, 1, NULL);
	if (!block)
		return -ENOENT;

	r = patch_create_byte(block, info->ubd, offset, actual_len, &entry, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "write dirent");

	return CALL(info->ubd, write_block, block, blockno);
}

static int ufs_dirent_linear_insert_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, struct dirent dirinfo, patch_t ** head)
{
	struct ufs_info * info = GET_UFS_INFO(object);
	struct UFS_direct entry, last_entry;
	uint32_t offset, len, prev_offset, last_basep = 0, basep = 0;
	int r;
	uint8_t fs_type;
	bool alloc = 0;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || !dirf || ufs_check_name(dirinfo.d_name) || offset < 0)
		return -EINVAL;

	// Prepare the UFS_direct entry
	fs_type = fstitch_to_ufs_type(dirinfo.d_type);
	if (fs_type == (uint8_t) -EINVAL)
		return -EINVAL;

	entry.d_type = fs_type;
	entry.d_ino = dirinfo.d_fileno;
	entry.d_namlen = dirinfo.d_namelen;
	strcpy(entry.d_name, dirinfo.d_name);
	entry.d_name[entry.d_namlen] = 0;
	len = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN, 4);

	// Find a place to put the new entry
	while (1) {
		prev_offset = last_basep;
		last_basep = basep;
		r = read_dirent(object, dirf, &last_entry, &basep);
		if (r < 0 && r != -1)
			return r;
		if (r == -1) { // EOF, return where next entry starts
			offset = ROUNDUP32(basep, 512);
			alloc = 1;
			break;
		}

		// An entry already exists
		if (last_entry.d_ino) {
			// Check to see if entry has room leftover for our entry
			uint32_t actual_len= ROUNDUP32(sizeof(struct UFS_direct)
					+ last_entry.d_namlen - UFS_MAXNAMELEN, 4);
			if (last_entry.d_reclen - actual_len >= len) {
				// Return entry to insert after
				offset = last_basep + actual_len;
				break;
			}
		}
		else {
			if (last_entry.d_reclen >= len) {
				offset = last_basep; // return blank entry location
				break;
			}
		}
	}

	// Need to extend directory
	if (alloc) {
		uint32_t newsize = offset + 512;
		// Need to allocate/append fragment
		if (offset % super->fs_fsize == 0) {
			bdesc_t * block;
			uint32_t blockno = CALL(info->parts.base, allocate_block, (fdesc_t *) dirf, 0, head);
			if (blockno == INVALID_BLOCK)
				return -1;
			block = CALL(info->ubd, synthetic_read_block, blockno, 1, NULL);
			assert(block); // FIXME Leiz == Lazy
			r = patch_create_init(block, info->ubd, head);
			assert(r >= 0); // FIXME Leiz == Lazy
			FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "clear dirblock");
			r = CALL(info->parts.base, append_file_block, (fdesc_t *) dirf, blockno, head);
			if (r < 0)
				return r;
		}

		// Set directory size
		fsmetadata_t fsm;
		fsm.fsm_feature = FSTITCH_FEATURE_SIZE;
		fsm.fsm_value.u = newsize;
		r = CALL(info->parts.base, set_metadata2_fdesc, (fdesc_t *) dirf, &fsm, 1, head);
		if (r < 0)
			return r;
	}

	// Inserting after existing entry
	if (!alloc && last_entry.d_ino) {
		uint32_t last_len = ROUNDUP32(sizeof(struct UFS_direct)
				+ last_entry.d_namlen - UFS_MAXNAMELEN, 4);
		entry.d_reclen = last_entry.d_reclen - last_len;
		r = write_dirent(object, dirf, entry, offset, head);
		if (r < 0)
			return r;
		last_entry.d_reclen = last_len;
		return write_dirent(object, dirf, last_entry, offset - last_len, head);
	}
	else {
		if (alloc) // Writing to new fragment
			entry.d_reclen = 512;
		else // Overwriting blank entry
			entry.d_reclen = last_entry.d_reclen;
		return write_dirent(object, dirf, entry, offset, head);
	}
}

static int ufs_dirent_linear_get_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	struct ufs_info * info = GET_UFS_INFO(object);
	struct UFS_direct dirent;
	struct UFS_dinode inode;
	uint32_t actual_len;
	uint32_t new_basep;
	int r;

	if (!entry)
		return -EINVAL;

	new_basep = *basep;
	r = read_dirent(object, dirf, &dirent, &new_basep);
	if (r < 0)
		return r;

	actual_len = sizeof(struct dirent) + dirent.d_namlen - DIRENT_MAXNAMELEN;
	if (size < actual_len)
		return -EINVAL;

	if (dirent.d_ino) {
		r = ufs_read_inode(info, dirent.d_ino, &inode); 
		if (r < 0)
			return r;

		if (inode.di_size > UFS_MAXFILESIZE) {
			printf("%s: file too big?\n", __FUNCTION__);
			inode.di_size &= UFS_MAXFILESIZE;
		}
	}

	entry->d_type = ufs_to_fstitch_type(dirent.d_type);
	entry->d_fileno = dirent.d_ino;
	entry->d_reclen = actual_len;
	entry->d_namelen = dirent.d_namlen;
	strncpy(entry->d_name, dirent.d_name, dirent.d_namlen);
	entry->d_name[dirent.d_namlen] = 0;
	*basep = new_basep;

	return 0;
}

static int ufs_dirent_linear_search_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, const char * name, inode_t * ino, int * offset)
{
	uint32_t basep = 0, last_basep;
	struct dirent entry;
	int r = 0;

	if (!dirf || ufs_check_name(name))
		return -EINVAL;

	while (r >= 0) {
		last_basep = basep;
		r = ufs_dirent_linear_get_dirent(object, dirf, &entry, sizeof(struct dirent), &basep);
		if (r < 0)
		{
			if (r == -1)
				return -ENOENT;
			else
				return r;
		}
		if (entry.d_fileno == 0) // Blank spot
			continue;
		if (!strcmp(entry.d_name, name)) {
			if (ino)
				*ino = entry.d_fileno;
			if (offset)
				*offset = last_basep;
			return 0;
		}
	}

	return 0;
}

static int ufs_dirent_linear_delete_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, const char * name, patch_t ** head)
{
	struct UFS_direct last_entry, entry;
	uint32_t basep, last_basep, p;
	int r, offset;

	if (!head || !dirf || ufs_check_name(name))
		return -EINVAL;

	r = ufs_dirent_linear_search_dirent(object, dirf, name, NULL, &offset);
	if (r < 0)
		return r;

	if (offset % 512 == 0) {
		// We are the first entry in the fragment
		p = offset;
		r = read_dirent(object, dirf, &entry, &p);
		if (r < 0)
			return r;

		entry.d_ino = 0;
		return write_dirent(object, dirf, entry, offset, head);
	}

	// Find the entry in front of us
	basep = 0;
	do {
		last_basep = basep;
		r = read_dirent(object, dirf, &last_entry, &basep);
		if (r < 0)
			return r;
	} while (basep < offset);

	// we went past the entry somehow?
	if (basep != offset) {
		printf("%s: went past the directory entry\n", __FUNCTION__);
		return -1;
	}

	// Get our entry
	p = basep;
	r = read_dirent(object, dirf, &entry, &p);
	if (r < 0)
		return r;

	last_entry.d_reclen += entry.d_reclen;

	return write_dirent(object, dirf, last_entry, last_basep, head);
}

static int ufs_dirent_linear_modify_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * file, struct dirent entry, uint32_t basep, patch_t ** head)
{
	struct UFS_direct e;

	e.d_type = fstitch_to_ufs_type(entry.d_type);
	if (e.d_type == (uint8_t) -EINVAL)
		return -EINVAL;

	e.d_ino = entry.d_fileno;
	e.d_reclen = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namelen - UFS_MAXNAMELEN, 4);
	e.d_namlen = entry.d_namelen;
	strncpy(e.d_name, entry.d_name, entry.d_namelen + 1);

	return write_dirent(object, file, e, basep, head);
}

static int ufs_dirent_linear_destroy(UFSmod_dirent_t * obj)
{
	struct ufsmod_dirent_info *info = (struct ufsmod_dirent_info *) obj;
	memset(info, 0, sizeof(*info));
	free(info);
	return 0;
}

UFSmod_dirent_t * ufs_dirent_linear(struct ufs_info * info)
{
	struct ufsmod_dirent_info * obj;
   
	if (!info)
		return NULL;

	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;

	UFS_DIRENT_INIT(&obj->ufsmod_dirent, ufs_dirent_linear);
	obj->info = info;
	return &obj->ufsmod_dirent;
}
