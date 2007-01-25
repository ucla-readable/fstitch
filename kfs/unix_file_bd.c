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

// define as 1 to make writes and syncs non-synchronous
#define RECKLESS_WRITE_SPEED 1

// block io activity logging
static FILE * block_log = NULL;
static size_t block_log_users = 0;


struct unix_file_info {
	char *fname;
	int fd;
	uint32_t blockcount;
	uint16_t blocksize;
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
	if(length > 0)
		string[0] = 0;
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
unix_file_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	off_t seeked;
	int r;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		if(!bdesc->ddesc->synthetic)
			return bdesc;
	}
	else
	{
		/* make sure it's a valid block */
		if(!count || number + count > info->blockcount)
			return NULL;
		
		bdesc = bdesc_alloc(number, info->blocksize, count);
		if(bdesc == NULL)
			return NULL;
		bdesc_autorelease(bdesc);
	}
	
	seeked = lseek(info->fd, number * info->blocksize, SEEK_SET);
	if(seeked != number * info->blocksize)
	{
		perror("lseek");
		assert(0);
	}
	
	r = read(info->fd, bdesc->ddesc->data, bdesc->ddesc->length);
	if(r != bdesc->ddesc->length)
	{
		if(r < 0)
			perror("read");
		assert(0);
	}
	
	if(block_log)
		for(r = 0; r < count; r++)
			fprintf(block_log, "%p read %u %d\n", object, number + r, r);
	
	if(bdesc->ddesc->synthetic)
		bdesc->ddesc->synthetic = 0;
	else if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return bdesc;
}

static bdesc_t *
unix_file_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	bdesc_t * bdesc;

	/* make sure it's a valid block */
	if(!count || number + count > info->blockcount)
		return NULL;

	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		return bdesc;
	}

	bdesc = bdesc_alloc(number, info->blocksize, count);
	if(bdesc == NULL)
		return NULL;
	bdesc_autorelease(bdesc);

	bdesc->ddesc->synthetic = 1;

	if(blockman_managed_add(info->blockman, bdesc) < 0)
		return NULL;

	return bdesc;
}

static int
unix_file_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	int r;
	int revision_forward, revision_back;
	off_t seeked;
	
	if(block->number + block->count > info->blockcount)
	{
		panic("wrote bad block number\n");
		return -E_INVAL;
	}

	r = revision_tail_prepare(block, object);
	if(r < 0)
	{
		panic("revision_tail_prepare gave: %i\n", r);
		return r;
	}
	revision_back = r;

	seeked = lseek(info->fd, block->number * info->blocksize, SEEK_SET);
	if(seeked != block->number * info->blocksize)
	{
		perror("lseek");
		assert(0);
	}
	if(write(info->fd, block->ddesc->data, block->ddesc->length) != block->ddesc->length)
	{
		perror("write");
		assert(0);
	}

	if(block_log)
		fprintf(block_log, "%p write %u\n", object, block->number);

	r = revision_tail_acknowledge(block, object);
	if(r < 0)
	{
		panic("revision_tail_acknowledge gave error: %i\n", r);
		return r;
	}
	revision_forward = r;

	if (revision_back != revision_forward)
		printf("%s(): block %u: revision_back (%d) != revision_forward (%d)\n", __FUNCTION__, block->number, revision_back, revision_forward);

	return 0;
}

/* WARNING: From man 2 sync:
 * "Note that while fsync() will flush all data from the host to the
 * drive (i.e. the "permanent storage device"), the drive itself may
 * not physically write the data to the platters for quite some time
 * and it may be written in an out-of-order sequence." */
// NOTE: MacOSX has the fcntl() command F_FULLFSYNC to flush a drive's buffer
static int
unix_file_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
#if !RECKLESS_WRITE_SPEED
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(object);
	if(fsync(info->fd))
	{
		perror("fsync");
		assert(0);
	}
#endif
	/* FLUSH_EMPTY is OK even if we did flush something,
	 * because unix_file_bd is a terminal BD */
	return FLUSH_EMPTY;
}

static int
unix_file_bd_destroy(BD_t * bd)
{
	struct unix_file_info * info = (struct unix_file_info *) OBJLOCAL(bd);
	int r;

	r = modman_rem_bd(bd);
	if(r < 0) return r;

	blockman_destroy(&info->blockman);

	close(info->fd);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);

	if(block_log)
	{
		block_log_users--;
		if(!block_log)
		{
			r = fclose(block_log);
			if(r == EOF)
			{
				perror("fclose(block_log)");
				panic("unable to close block log\n");
			}
			block_log = NULL;
		}
	}
	
	return 0;
}

BD_t *
unix_file_bd(const char *fname, uint16_t blocksize)
{
	struct unix_file_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	struct stat sb;
	uint32_t blocks;
	int r;
	
	if(!bd)
		return NULL;
	
	r = stat(fname, &sb);
	if(r == -1)
	{
		perror("stat");
		panic("unable to stat %s\n", fname);
	}
	blocks = sb.st_size / blocksize;
	if(sb.st_size != (blocks * blocksize))
		panic("file %s's size is not block-aligned\n", fname);
	if(blocks < 1)
		return NULL;

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
#if RECKLESS_WRITE_SPEED
	info->fd = open(fname, O_RDWR, 0);
#else
	info->fd = open(fname, O_RDWR | O_DSYNC, 0);
#endif
	if(!info->fd == -1)
	{
		perror("open");
		free(info);
		free(bd);
		return NULL;
	}
	info->blockman = blockman_create(blocksize, NULL, NULL);
	if(!info->blockman)
	{
		close(info->fd);
		free(info);
		free(bd);
		return NULL;
	}

	BD_INIT(bd, unix_file_bd, info);
	bd->level = 0;
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}

	if(block_log || getenv("BLOCK_LOG"))
	{
		if(block_log)
			block_log_users++;
		else
		{
			block_log = fopen(getenv("BLOCK_LOG"), "a");
			if(!block_log)
				perror("fopen(block_log)");

			// separate multiple uses of a log file
			fprintf(block_log, "block_log start\n");
		}
	}
	
	return bd;
}
