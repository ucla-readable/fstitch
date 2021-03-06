/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/blockman.h>
#include <fscore/modman.h>
#include <fscore/revision.h>

#include <modules/unix_file_bd.h>

// define as 1 to make writes and syncs non-synchronous
#define RECKLESS_WRITE_SPEED 1

// block io activity logging
static FILE * block_log = NULL;
static size_t block_log_users = 0;

struct unix_file_info {
	BD_t my_bd;
	
	char * fname;
	int fd;
	blockman_t blockman;
	int user_name;
};

static bdesc_t * unix_file_bd_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct unix_file_info * info = (struct unix_file_info *) object;
	bdesc_t * bdesc;
	off_t seeked;
	int r;
	
	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);
		
	bdesc = blockman_lookup(&info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->length == count * object->blocksize);
		bdesc_ensure_linked_page(bdesc, page);
		if(!bdesc->synthetic)
			return bdesc;
	}
	else
	{
		bdesc = bdesc_alloc(number, object->blocksize, count, page);
		if(!bdesc)
			return NULL;
		bdesc_autorelease(bdesc);
	}
	
	seeked = lseek(info->fd, number * object->blocksize, SEEK_SET);
	if(seeked != number * object->blocksize)
	{
		perror("lseek");
		assert(0);
	}
	
	r = read(info->fd, bdesc_data(bdesc), bdesc->length);
	if(r != bdesc->length)
	{
		if(r < 0)
			perror("read");
		assert(0);
	}
	
	if(block_log)
		for(r = 0; r < count; r++)
			fprintf(block_log, "%d read %u %d\n", info->user_name, number + r, r);
	
	if(bdesc->synthetic)
		bdesc->synthetic = 0;
	else
		blockman_add(&info->blockman, bdesc, number);
	
	return bdesc;
}

static bdesc_t * unix_file_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count, page_t * page)
{
	struct unix_file_info * info = (struct unix_file_info *) object;
	bdesc_t * bdesc;

	/* make sure it's a valid block */
	assert(count && number + count <= object->numblocks);

	bdesc = blockman_lookup(&info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->length == count * object->blocksize);
		bdesc_ensure_linked_page(bdesc, page);
		return bdesc;
	}

	bdesc = bdesc_alloc(number, object->blocksize, count, page);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);

	bdesc->synthetic = 1;

	blockman_add(&info->blockman, bdesc, number);

	return bdesc;
}

static int unix_file_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct unix_file_info * info = (struct unix_file_info *) object;
	int r, revision_back;
	off_t seeked;

	/* make sure it's a valid block */
	assert(block->length && number + block->length / object->blocksize <= object->numblocks);

	seeked = lseek(info->fd, number * object->blocksize, SEEK_SET);
	if(seeked != number * object->blocksize)
	{
		perror("lseek");
		assert(0);
	}
	
#if REVISION_TAIL_INPLACE
	revision_back = revision_tail_prepare(block, object);
	if(revision_back < 0)
	{
		kpanic("revision_tail_prepare gave: %d\n", revision_back);
		return revision_back;
	}
	if(write(info->fd, block->data, block->length) != block->length)
	{
		perror("write");
		assert(0);
	}
#else
	static uint8_t buffer[4096];
	assert(block->length <= 4096);
	revision_back = revision_tail_prepare(block, object, buffer);
	if(revision_back < 0)
	{
		kpanic("revision_tail_prepare gave: %d\n", revision_back);
		return revision_back;
	}
	if(write(info->fd, buffer, block->length) != block->length)
	{
		perror("write");
		assert(0);
	}
#endif

	if(block_log)
		fprintf(block_log, "%d write %u %d\n", info->user_name, number, block->flags);

	r = revision_tail_acknowledge(block, object);
	if(r < 0)
	{
		kpanic("revision_tail_acknowledge gave error: %i\n", r);
		return r;
	}

	if(revision_back != r)
		printf("%s(): block %u: revision_back (%d) != revision_forward (%d)\n", __FUNCTION__, number, revision_back, r);

	return 0;
}

/* WARNING: From man 2 sync:
 * "Note that while fsync() will flush all data from the host to the
 * drive (i.e. the "permanent storage device"), the drive itself may
 * not physically write the data to the platters for quite some time
 * and it may be written in an out-of-order sequence." */
// NOTE: Mac OS X has the fcntl() command F_FULLFSYNC to flush a drive's buffer
static int unix_file_bd_flush(BD_t * object, uint32_t block, patch_t * ch)
{
#if !RECKLESS_WRITE_SPEED
	struct unix_file_info * info = (struct unix_file_info *) object;
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

static patch_t ** unix_file_bd_get_write_head(BD_t * object)
{
	return NULL;
}

static int32_t unix_file_bd_get_block_space(BD_t * object)
{
	return 0;
}

static int unix_file_bd_destroy(BD_t * bd)
{
	struct unix_file_info * info = (struct unix_file_info *) bd;
	int r;

	r = modman_rem_bd(bd);
	if(r < 0)
		return r;

	blockman_destroy(&info->blockman);

	close(info->fd);
	memset(info, 0, sizeof(*info));
	free(info);

	if(block_log)
	{
		block_log_users--;
		if(!block_log)
		{
			r = fclose(block_log);
			if(r == EOF)
			{
				perror("fclose(block_log)");
				kpanic("unable to close block log\n");
			}
			block_log = NULL;
		}
	}
	
	return 0;
}

BD_t * unix_file_bd(const char * fname, uint16_t blocksize)
{
	struct unix_file_info * info = malloc(sizeof(*info));
	BD_t * bd;
	struct stat sb;
	uint32_t blocks;
	int r;
	
	if(!info)
		return NULL;
	bd = &info->my_bd;
	
	r = stat(fname, &sb);
	if(r == -1)
	{
		perror("stat");
		kpanic("unable to stat '%s'\n", fname);
	}
	blocks = sb.st_size / blocksize;
	if(sb.st_size != (blocks * blocksize))
		kpanic("file %s's size is not block-aligned\n", fname);
	if(blocks < 1)
	{
		free(info);
		return NULL;
	}
	
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
		return NULL;
	}
	if(blockman_init(&info->blockman) < 0)
	{
		close(info->fd);
		free(info);
		return NULL;
	}

	BD_INIT(bd, unix_file_bd);
	bd->level = 0;
	bd->graph_index = 0;

	bd->numblocks = blocks;
	bd->blocksize = blocksize;
	bd->atomicsize = blocksize;
	
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
	info->user_name = block_log_users;
	
	return bd;
}
