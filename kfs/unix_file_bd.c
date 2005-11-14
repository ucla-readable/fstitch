#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inc/error.h>
#include <lib/types.h>
#include <lib/panic.h>
#include <sys/types.h>
#include <sys/stat.h>


#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>
#include <kfs/modman.h>
#include <kfs/unix_file_bd.h>
#include <kfs/revision.h>
#include <kfs/josfs_base.h>

/* We can remove this part once we no longer format as JOS by default */
#ifdef KUDOS_INC_FS_H
#error inc/fs.h got included in mem_bd.c
#endif

struct unix_file_info {
	char *fname;
	int fd;
	uint32_t blockcount;
	uint16_t blocksize;
	uint16_t level;
	blockman_t * blockman;
};

static int
unix_file_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_BRIEF:
			snprintf(string, length, "%d(%dblks)",
					 info->blocksize, info->blockcount);
			break;
		case CONFIG_VERBOSE:
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "%d bytes x %d blocks, %s",
					 info->blocksize, info->blockcount, info->fname);
	}
	return 0;
}

static int
unix_file_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t
unix_file_bd_get_numblocks(BD_t * object)
{
	return ((struct unix_file_info*) OBJLOCAL(object))->blockcount;
}

static uint16_t
unix_file_bd_get_blocksize(BD_t * object)
{
	return ((struct unix_file_info*) OBJLOCAL(object))->blocksize;
}

static uint16_t
unix_file_bd_get_atomicsize(BD_t * object)
{
	return unix_file_bd_get_blocksize(object);
}

static bdesc_t *
unix_file_bd_read_block(BD_t * object, uint32_t number)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	bdesc_t * ret;
	int r;
	off_t seeked;

	if (number >= info->blockcount)
		return NULL;

	ret = blockman_managed_lookup(info->blockman, number);
	if (ret)
		return ret;

	ret = bdesc_alloc(number, info->blocksize);
	if (ret == NULL)
		return NULL;
	bdesc_autorelease(ret);
	
	seeked = lseek(info->fd, number * info->blocksize, SEEK_SET);
	if (seeked != number * info->blocksize)
	{
		perror("lseek");
		assert(0);
	}

	r = read(info->fd, ret->ddesc->data, info->blocksize);
	if (r != info->blocksize)
	{
		if (r < 0)
			perror("read");
		assert(0);
	}

	r = blockman_managed_add(info->blockman, ret);
	if (r < 0)
		return NULL;
	return ret;
}

static bdesc_t *
unix_file_bd_synthetic_read_block(BD_t * object, uint32_t number,
								  bool * synthetic)
{
	bdesc_t * ret = unix_file_bd_read_block(object, number);
	if(ret)
		*synthetic = 0;
	return ret;
}

static int
unix_file_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int
unix_file_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	int r;
	off_t seeked;
	
	if(block->ddesc->length != info->blocksize) {
		panic("wrote block with bad length\n");
		return -E_INVAL;
	}
	if (block->number >= info->blockcount) {
		panic("wrote bad block number\n");
		return -E_INVAL;
	}

	r = revision_tail_prepare(block, object);
	if (r != 0) {
		panic("revision_tail_prepare gave: %e\n", r);
		return r;
	}

	seeked = lseek(info->fd, block->number * info->blocksize, SEEK_SET);
	if (seeked != block->number * info->blocksize)
	{
		perror("lseek");
		assert(0);
	}
	if (write(info->fd, block->ddesc->data, info->blocksize) != info->blocksize)
	{
		perror("write");
		assert(0);
	}

	r = revision_tail_acknowledge(block, object);
	if (r != 0) {
		panic("revision_tail_acknowledge gave error: %e\n", r);
		return r;
	}

	return 0;
}

#warning From man 2 sync:
#warning Note that while fsync() will flush all data from the host to the
#warning drive (i.e. the "permanent storage device"), the drive itself may
#warning not physically write the data to the platters for quite some time
#warning and it may be written in an out-of-order sequence.
// NOTE: MacOSX's has the fcntl() command F_FULLFSYNC to flush a drive's buffer
static int
unix_file_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	if (fsync(info->fd))
	{
		perror("fsync");
		assert(0);
	}
	return 0;
}

static uint16_t
unix_file_bd_get_devlevel(BD_t * object)
{
	return ((struct unix_file_info *) OBJLOCAL(object))->level;
}

static int
unix_file_bd_destroy(BD_t * bd)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(bd);
	int r;

	r = modman_rem_bd(bd);
	if (r < 0) return r;

	blockman_destroy(&info->blockman);

	close(info->fd);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

BD_t *
unix_file_bd(char *fname, uint32_t blocks, uint16_t blocksize)
{
	struct unix_file_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	struct stat sb;
	int r;
	
	if (blocks < 1)
		return NULL;

	if(!bd)
		return NULL;
	
	r = stat(fname, &sb);
	if (r == -1) {
		perror("stat");
		panic("unable to stat %s\n", fname);
	}
	if (sb.st_size != (blocks * blocksize)) {
		panic("file %s has wrong size\n", fname);
	}

	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	info->blockcount = blocks;
	info->blocksize = blocksize;

	// TODO: use O_DIRECT open flag on linux
	// NOTE: linux implements O_DSYNC using O_SYNC :(
#if defined(__MACH__)
	info->fd = open(fname, O_RDWR, 0);
#else
	info->fd = open(fname, O_RDWR | O_DSYNC, 0);
#endif
	if (!info->fd == -1) {
		perror("open");
		free(info);
		free(bd);
		return NULL;
	}
#if defined(__MACH__)
	// disable caching for file on Mac OS X
	r = fcntl(info->fd, F_NOCACHE, 1);
	if (r == -1) {
		perror("fcntl");
		close(info->fd);
		free(info);
		free(bd);
		return NULL;
	}
#endif
	info->blockman = blockman_create();
	if (!info->blockman) {
		close(info->fd);
		free(info);
		free(bd);
		return NULL;
	}

	info->level = 0;
	
	BD_INIT(bd, unix_file_bd, info);
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
