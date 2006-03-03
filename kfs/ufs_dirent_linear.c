#include <kfs/ufs_dirent_linear.h>

static int read_dirent(UFS_Dirent_t * object, ufs_fdesc_t * dirf, struct UFS_direct * entry, uint32_t * basep)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_direct * dirent;
	bdesc_t * dirblock = NULL;
	uint32_t blockno, offset;

	if (!entry)
		return -E_INVAL;

	// Make sure it's a directory and we can read from it
	if (dirf->f_type != TYPE_DIR)
		return -E_NOT_DIR;

	if (*basep >= dirf->f_inode.di_size)
		return -E_EOF;

	blockno = CALL(info->parts.base, get_file_block, (fdesc_t *) dirf, ROUNDDOWN32(*basep, info->super->fs_fsize));
	if (blockno != INVALID_BLOCK)
		dirblock = CALL(info->parts.base, lookup_block, blockno);
	if (!dirblock)
		return -E_NOT_FOUND;

	offset = *basep % info->super->fs_fsize;
	dirent = (struct UFS_direct *) (dirblock->ddesc->data + offset);

	if (offset + dirent->d_reclen > info->super->fs_fsize
			|| dirent->d_reclen < dirent->d_namlen)
		return -E_UNSPECIFIED;

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
static int write_dirent(UFS_Dirent_t * object, ufs_fdesc_t * dirf, struct UFS_direct entry, uint32_t basep, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	bdesc_t * block;
	uint32_t foffset, blockno;
	uint16_t offset, actual_len;
	int r;

	if (!head || !tail || !dirf)
		return -E_INVAL;

	actual_len = sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN;

	offset = basep % info->super->fs_fsize;
	foffset = basep - offset;
	blockno = CALL(info->parts.base, get_file_block, (fdesc_t *) dirf, foffset);
	if (blockno == INVALID_BLOCK)
		return -E_NOT_FOUND;
	block = CALL(info->ubd, read_block, blockno);
	if (!block)
		return -E_NOT_FOUND;

	r = chdesc_create_byte(block, info->ubd, offset, actual_len,
			&entry, head, tail);
	if (r < 0)
		return r;

	return CALL(info->ubd, write_block, block);
}

// tries to find an empty entry for a filename of len, in directory dirf
static int ufs_dirent_linear_find_free_dirent(UFS_Dirent_t * object, ufs_fdesc_t * dirf, uint32_t len)
{
	struct UFS_direct entry;
	uint32_t basep = 0, last_basep, actual_len;
	int r;

	if (!dirf)
		return -E_INVAL;

	len = ROUNDUP32(sizeof(struct UFS_direct) + len - UFS_MAXNAMELEN, 4);

	while (1) {
		last_basep = basep;
		r = read_dirent(object, dirf, &entry, &basep);
		if (r < 0 && r != -E_EOF)
			return r;
		if (r == -E_EOF) // EOF, return where next entry starts
			return basep;

		if (entry.d_ino) {
			// Check to see if entry has room leftover for our entry
			actual_len = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN, 4);
			if (entry.d_reclen - actual_len >= len)
				return last_basep; // return entry to insert after
		}
		else {
			if (entry.d_reclen >= len)
				return last_basep; // return blank entry location
		}
	}
}

static int ufs_dirent_linear_insert_dirent(UFS_Dirent_t * object, ufs_fdesc_t * dirf, inode_t ino, uint8_t type, const char * name, int offset, chdesc_t ** head, chdesc_t ** tail)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_direct entry, last_entry;
	chdesc_t * tmptail;
	chdesc_t ** newtail = tail;
	uint32_t len, last_len, blockno, newsize = offset + 512;
	int r, p = offset, alloc = 0;
	uint8_t fs_type;

	if (!head || !tail || !dirf || check_name(name) || offset < 0)
		return -E_INVAL;

	len = ROUNDUP32(sizeof(struct UFS_direct) + entry.d_namlen - UFS_MAXNAMELEN, 4);

	switch(type)
	{
		case TYPE_FILE:
			fs_type = UFS_DT_REG;
			break;
		case TYPE_DIR:
			fs_type = UFS_DT_DIR;
			break;
		case TYPE_SYMLINK:
			fs_type = UFS_DT_LNK;
			break;
			// case TYPE_DEVICE: ambiguous
		default:
			return -E_INVAL;
	}

	entry.d_type = fs_type;
	entry.d_ino = ino;
	entry.d_namlen = strlen(name);
	strcpy(entry.d_name, name);
	entry.d_name[entry.d_namlen] = 0;

	// Need to extend directory
	if (offset >= dirf->f_inode.di_size) {
		// Need to allocate/append fragment
		if (offset % info->super->fs_fsize == 0) {
			blockno = CALL(info->parts.base, allocate_block, (fdesc_t *) dirf, 0, head, newtail);
			if (blockno == INVALID_BLOCK)
				return -E_UNSPECIFIED;
			newtail = &tmptail;
			r = CALL(info->parts.base, append_file_block, (fdesc_t *) dirf, blockno, head, newtail);
			if (r < 0)
				return r;
		}

		// Set directory size
		r = CALL(info->parts.base, set_metadata_fdesc, (fdesc_t *) dirf, KFS_feature_size.id, sizeof(uint32_t), &newsize, head, newtail);
		if (r < 0)
			return r;
		alloc = 1;
	}

	r = read_dirent(object, dirf, &last_entry, &p);
	if (r < 0)
		return r;

	// Inserting after existing entry
	if (!alloc && last_entry.d_ino) {
		last_len = ROUNDUP32(sizeof(struct UFS_direct) + last_entry.d_namlen - UFS_MAXNAMELEN, 4);
		entry.d_reclen = last_entry.d_reclen - last_len;
		r = write_dirent(object, dirf, entry, offset + last_len, head, newtail);
		if (r < 0)
			return r;
		newtail = &tmptail;
		last_entry.d_reclen = last_len;
		r = write_dirent(object, dirf, last_entry, offset, head, newtail);
		return r;
	}
	else {
		if (alloc) // Writing to new fragment
			entry.d_reclen = 512;
		else // Overwriting blank entry
			entry.d_reclen = last_entry.d_reclen;
		return write_dirent(object, dirf, entry, offset, head, newtail);
	}
}

static int ufs_dirent_linear_get_dirent(UFS_Dirent_t * object, ufs_fdesc_t * dirf, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	struct lfs_info * info = (struct lfs_info *) OBJLOCAL(object);
	struct UFS_direct dirent;
	struct UFS_dinode inode;
	uint32_t actual_len;
	int r;

	if (!entry)
		return -E_INVAL;

	r = read_dirent(object, dirf, &dirent, basep);
	if (r < 0)
		return r;

	actual_len = sizeof(struct dirent) + dirent.d_namlen - DIRENT_MAXNAMELEN;
	if (size < actual_len)
		return -E_INVAL;

	if (dirent.d_ino) {
		r = read_inode(info, dirent.d_ino, &inode); 
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

	switch(dirent.d_type)
	{
		case UFS_DT_REG:
			entry->d_type = TYPE_FILE;
			break;
		case UFS_DT_DIR:
			entry->d_type = TYPE_DIR;
			break;
		case UFS_DT_LNK:
			entry->d_type = TYPE_SYMLINK;
			break;
		case UFS_DT_CHR:
		case UFS_DT_BLK:
			entry->d_type = TYPE_DEVICE;
			break;
		default:
			entry->d_type = TYPE_INVAL;
	}
	entry->d_fileno = dirent.d_ino;
	entry->d_reclen = actual_len;
	entry->d_namelen = dirent.d_namlen;
	strncpy(entry->d_name, dirent.d_name, dirent.d_namlen);
	entry->d_name[dirent.d_namlen] = 0;

	return 0;
}

static int ufs_dirent_linear_search_dirent(UFS_Dirent_t * object, ufs_fdesc_t * dirf, const char * name, inode_t * ino, int * offset)
{
	uint32_t basep = 0, last_basep;
	struct dirent entry;
	int r = 0;

	if (!dirf || check_name(name))
		return -E_INVAL;

	while (r >= 0) {
		last_basep = basep;
		r = ufs_dirent_linear_get_dirent(object, dirf, &entry, sizeof(struct dirent), &basep);
		if (r < 0)
		{
			if (r == -E_EOF)
				return -E_NOT_FOUND;
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

static int ufs_dirent_linear_delete_dirent(UFS_Dirent_t * object, ufs_fdesc_t * dirf, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	struct UFS_direct last_entry, entry;
	uint32_t basep, last_basep, p;
	int r, offset;

	if (!head || !tail || !dirf || check_name(name))
		return -E_INVAL;

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
		return write_dirent(object, dirf, entry, offset, head, tail);
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
		return -E_UNSPECIFIED;
	}

	// Get our entry
	p = basep;
	r = read_dirent(object, dirf, &entry, &p);
	if (r < 0)
		return r;

	last_entry.d_reclen += entry.d_reclen;

	return write_dirent(object, dirf, last_entry, last_basep, head, tail);
}

static int ufs_dirent_linear_modify_dirent(UFS_Dirent_t * object, ufs_fdesc_t * file, struct dirent entry, uint32_t basep, chdesc_t ** head, chdesc_t ** tail)
{
	struct UFS_direct e;

	switch(entry.d_type)
	{
		case TYPE_FILE:
			e.d_type = UFS_DT_REG;
			break;
		case TYPE_DIR:
			e.d_type = UFS_DT_DIR;
			break;
		case TYPE_SYMLINK:
			e.d_type = UFS_DT_LNK;
			break;
			// case TYPE_DEVICE: ambiguous
		default:
			return -E_INVAL;
	}

	e.d_ino = entry.d_fileno;
	e.d_reclen = entry.d_reclen;
	e.d_namlen = entry.d_namelen;
	strncpy(e.d_name, entry.d_name, entry.d_namelen + 1);

	return write_dirent(object, file, e, basep, head, tail);
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

static int ufs_dirent_linear_destroy(UFS_Dirent_t * obj)
{
	free(OBJLOCAL(obj));
	memset(obj, 0, sizeof(*obj));
	free(obj);

	return 0;
}

UFS_Dirent_t * ufs_dirent_linear(struct lfs_info * info)
{
	UFS_Dirent_t * obj = malloc(sizeof(*obj));

	if (!obj)
		return NULL;

	UFS_DIRENT_INIT(obj, ufs_dirent_linear, info);
	return obj;
}
