#include <lib/platform.h>

#include <kfs/debug.h>
#include <kfs/ufs_dirent_linear.h>

static int read_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, struct UFS_direct * entry, uint32_t * basep)
{
	struct ufs_info * info = (struct ufs_info *) OBJLOCAL(object);
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
		dirblock = CALL(info->parts.base, lookup_block, blockno);
	if (!dirblock)
		return -ENOENT;

	offset = *basep % super->fs_fsize;
	dirent = (struct UFS_direct *) (dirblock->ddesc->data + offset);

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
static int write_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, struct UFS_direct entry, uint32_t basep, chdesc_t ** head)
{
	struct ufs_info * info = (struct ufs_info *) OBJLOCAL(object);
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
	block = CALL(info->ubd, read_block, blockno, 1);
	if (!block)
		return -ENOENT;

	r = chdesc_create_byte(block, info->ubd, offset, actual_len, &entry, head);
	if (r < 0)
		return r;
	KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "write dirent");

	return CALL(info->ubd, write_block, block);
}

static int ufs_dirent_linear_insert_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, struct dirent dirinfo, chdesc_t ** head)
{
	struct ufs_info * info = (struct ufs_info *) OBJLOCAL(object);
	struct UFS_direct entry, last_entry;
	uint32_t offset, len, prev_offset, last_basep = 0, basep = 0;
	int r;
	uint8_t fs_type;
	bool alloc = 0;
	const struct UFS_Super * super = CALL(info->parts.p_super, read);

	if (!head || !dirf || ufs_check_name(dirinfo.d_name) || offset < 0)
		return -EINVAL;

	// Prepare the UFS_direct entry
	fs_type = kfs_to_ufs_type(dirinfo.d_type);
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
			block = CALL(info->ubd, synthetic_read_block, blockno, 1);
			assert(block); // FIXME Leiz == Lazy
			r = chdesc_create_init(block, info->ubd, head);
			assert(r >= 0); // FIXME Leiz == Lazy
			KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_CHDESC_LABEL, *head, "clear dirblock");
			r = CALL(info->parts.base, append_file_block, (fdesc_t *) dirf, blockno, head);
			if (r < 0)
				return r;
		}

		// Set directory size
		r = CALL(info->parts.base, set_metadata_fdesc, (fdesc_t *) dirf, KFS_feature_size.id, sizeof(uint32_t), &newsize, head);
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
	struct ufs_info * info = (struct ufs_info *) OBJLOCAL(object);
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
		entry->d_filesize = inode.di_size;
	}
	else
		entry->d_filesize = 0;

	entry->d_type = ufs_to_kfs_type(dirent.d_type);
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

static int ufs_dirent_linear_delete_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * dirf, const char * name, chdesc_t ** head)
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

static int ufs_dirent_linear_modify_dirent(UFSmod_dirent_t * object, ufs_fdesc_t * file, struct dirent entry, uint32_t basep, chdesc_t ** head)
{
	struct UFS_direct e;

	e.d_type = kfs_to_ufs_type(entry.d_type);
	if (e.d_type == (uint8_t) -EINVAL)
		return -EINVAL;

	e.d_ino = entry.d_fileno;
	e.d_reclen = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namelen - UFS_MAXNAMELEN, 4);
	e.d_namlen = entry.d_namelen;
	strncpy(e.d_name, entry.d_name, entry.d_namelen + 1);

	return write_dirent(object, file, e, basep, head);
}

static int ufs_dirent_linear_get_config(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_dirent_linear_get_status(void * object, int level, char * string, size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_dirent_linear_destroy(UFSmod_dirent_t * obj)
{
	memset(obj, 0, sizeof(*obj));
	free(obj);
	return 0;
}

UFSmod_dirent_t * ufs_dirent_linear(struct ufs_info * info)
{
	UFSmod_dirent_t * obj;
   
	if (!info)
		return NULL;

	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;

	UFS_DIRENT_INIT(obj, ufs_dirent_linear, info);
	return obj;
}
