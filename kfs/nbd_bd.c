#include <stdlib.h>
#include <string.h>
#include <inc/error.h>
#include <lib/jiffies.h>
#include <lib/netclient.h>
#include <lib/types.h>
#include <lib/std.h>
#include <lib/stdio.h>
#include <lib/sleep.h>

/* byte ordering */
#include <string.h>
#if defined(KUDOS)
#include <inc/josnic.h>
#elif defined(UNIXUSER)
#include <netinet/in.h>
#endif

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>
#include <kfs/revision.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/nbd_bd.h>

#define NBD_RETRIES 5

struct nbd_info {
	int fd;
	uint32_t length;
	blockman_t * blockman;
	struct ip_addr ip;
	uint16_t blocksize;
	uint16_t port;
};

static int nbd_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "host: %s, port: %d, blocksize: %d, count: %d", kinet_iptoa(info->ip), info->port, info->blocksize, info->length);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%s:%d", kinet_iptoa(info->ip), info->port);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "host: %s, port: %d, blocksize: %d, count: %d", kinet_iptoa(info->ip), info->port, info->blocksize, info->length);
	}
	return 0;
}

static int nbd_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static uint32_t nbd_bd_get_numblocks(BD_t * object)
{
	return ((struct nbd_info *) OBJLOCAL(object))->length;
}

static uint16_t nbd_bd_get_blocksize(BD_t * object)
{
	return ((struct nbd_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t nbd_bd_get_atomicsize(BD_t * object)
{
	return ((struct nbd_info *) OBJLOCAL(object))->blocksize;
}

static int nbd_bd_reset(BD_t * object)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(object);
	uint32_t length;
	uint16_t blocksize;
	int r;
	
	kdprintf(STDERR_FILENO, "%s(): resetting %s:%d\n", __FUNCTION__, kinet_iptoa(info->ip), info->port);
	
	if(info->fd != -1)
	{
		close(info->fd);
		info->fd = -1;
	}
	
	if(kconnect(info->ip, info->port, &info->fd))
		goto error;
	
	r = read(info->fd, &length, sizeof(length));
	if(r != sizeof(length))
		goto error;
	
	r= read(info->fd, &blocksize, sizeof(blocksize));
	if(r != sizeof(blocksize))
		goto error;
	
	/* switch to host byte order */
	length = ntohl(length);
	blocksize = ntohs(blocksize);
	
	if(length != info->length || blocksize != info->blocksize)
		goto error;
	
	return 0;
	
  error:
	if(info->fd != -1)
	{
		close(info->fd);
		info->fd = -1;
	}
	
	return -1;
}

static bdesc_t * nbd_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	int tries;
	
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
		if(!count || number + count > info->length)
			return NULL;

		bdesc = bdesc_alloc(number, info->blocksize, count);
		if(!bdesc)
			return NULL;
		bdesc_autorelease(bdesc);
	}
	
	/* switch to network byte order */
	number = htonl(number);
	
	for(tries = 0; tries != NBD_RETRIES; tries++)
	{
		uint8_t command = 0;
		int r;
		
		/* read it */
		r = write(info->fd, &command, 1);
		if(r != 1)
			goto error;
		
		r = write(info->fd, &number, sizeof(number));
		if(r != sizeof(number))
			goto error;
		
		r = write(info->fd, &count, sizeof(count));
		if(r != sizeof(count))
			goto error;
		
		r = readn(info->fd, bdesc->ddesc->data, info->blocksize);
		if(r != info->blocksize)
			goto error;
		
		if(bdesc->ddesc->synthetic)
			bdesc->ddesc->synthetic = 0;
		else if(blockman_managed_add(info->blockman, bdesc) < 0)
			/* kind of a waste of the read... but we have to do it */
			return NULL;
		
		return bdesc;
		
	error:
		jsleep(tries * HZ / 20);
		nbd_bd_reset(object);
	}
	
	kdprintf(STDERR_FILENO, "%s(): giving up on %s:%d\n", __FUNCTION__, kinet_iptoa(info->ip), info->port);
	return NULL;
}

static bdesc_t * nbd_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(!count || number + count > info->length)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->blocksize, count);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	bdesc->ddesc->synthetic = 1;
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return bdesc;
}

static int nbd_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(object);
	int tries, r = -1;
	
	/* make sure it's a valid block */
	if(block->number + block->count > info->length)
		return -E_INVAL;
	
	/* prepare the block for writing */
	revision_tail_prepare(block, object);
	
	KFS_DEBUG_DBWAIT(block);
	
	for(tries = 0; tries != NBD_RETRIES; tries++)
	{
		uint8_t command = 1;
		uint32_t number;
		
		/* switch to network byte order */
		number = htonl(block->number);
		
		/* write it */
		r = write(info->fd, &command, 1);
		if(r != 1)
			goto error;
		
		r = write(info->fd, &number, sizeof(number));
		if(r != sizeof(number))
			goto error;
		
		r = write(info->fd, &block->count, sizeof(block->count));
		if(r != sizeof(block->count))
			goto error;
		
		r = write(info->fd, block->ddesc->data, block->ddesc->length);
		if(r != info->blocksize)
			goto error;
		
		/* acknowledge the write as successful */
		revision_tail_acknowledge(block, object);
		return 0;
		
	error:
		jsleep(tries * HZ / 20);
		nbd_bd_reset(object);
	}
	
	/* the write failed; don't remove any change descriptors... */
	revision_tail_revert(block, object);
	kdprintf(STDERR_FILENO, "%s(): giving up on %s:%d\n", __FUNCTION__, kinet_iptoa(info->ip), info->port);
	return r;
}

static int nbd_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static int nbd_bd_destroy(BD_t * bd)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(bd);
	int r, val = 0;
	
	val = modman_rem_bd(bd);
	if(val < 0)
		return val;
	
	blockman_destroy(&info->blockman);
	
	r = close(info->fd);
	if(r < 0)
		val = r;
	
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return val;
}

BD_t * nbd_bd(const char * address, uint16_t port)
{
	struct nbd_info * info;
	BD_t * bd;
	int r;
	
	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(struct nbd_info));
	if(!info)
		goto error_bd;
	
	BD_INIT(bd, nbd_bd, info);
	
	if(kgethostbyname(address, &info->ip) < 0)
		goto error_info;
	info->port = port;
	
	if(kconnect(info->ip, port, &info->fd))
		goto error_info;
	
	r = read(info->fd, &info->length, sizeof(info->length));
	if(r != sizeof(info->length))
		goto error_connect;
	
	r= read(info->fd, &info->blocksize, sizeof(info->blocksize));
	if(r != sizeof(info->blocksize))
		goto error_connect;
	
	/* switch to host byte order */
	info->length = ntohl(info->length);
	info->blocksize = ntohs(info->blocksize);
	
	bd->level = 0;
	
	info->blockman = blockman_create(info->blocksize, NULL);
	if(!info->blockman)
		goto error_connect;
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
	
	blockman_destroy(&info->blockman);
  error_connect:
	close(info->fd);
  error_info:
	free(info);
  error_bd:
	free(bd);
	return NULL;
}
