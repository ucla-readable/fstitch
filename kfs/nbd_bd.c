#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/lib.h>
/* for byte order translations */
#include <inc/net/ipv4/lwip/inet.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/nbd_bd.h>

#define NBD_RETRIES 5

struct nbd_info {
	int fd[2];
	uint32_t length;
	uint16_t blocksize;
	uint16_t port;
	struct ip_addr ip;
};

static int nbd_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "host: %s, port: %d, blocksize: %d, count: %d", inet_iptoa(info->ip), info->port, info->blocksize, info->length);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%s:%d", inet_iptoa(info->ip), info->port);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "host: %s, port: %d, blocksize: %d, count: %d", inet_iptoa(info->ip), info->port, info->blocksize, info->length);
	}
	return 0;
}

static int nbd_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
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
	int i, r;
	
	fprintf(STDERR_FILENO, "%s(): resetting %s:%d\n", __FUNCTION__, inet_iptoa(info->ip), info->port);
	
	for(i = 0; i != 2; i++)
		if(info->fd[i] != -1)
		{
			close(info->fd[i]);
			info->fd[i] = -1;
		}
	
	if(connect(info->ip, info->port, info->fd))
		goto error;
	
	r = read(info->fd[0], &length, sizeof(length));
	if(r != sizeof(length))
		goto error;
	
	r= read(info->fd[0], &blocksize, sizeof(blocksize));
	if(r != sizeof(blocksize))
		goto error;
	
	/* switch to host byte order */
	length = ntohl(length);
	blocksize = ntohs(blocksize);
	
	if(length != info->length || blocksize != info->blocksize)
		goto error;
	
	return 0;
	
  error:
	for(i = 0; i != 2; i++)
		if(info->fd[i] != -1)
		{
			close(info->fd[i]);
			info->fd[i] = -1;
		}
	
	return -1;
}

static bdesc_t * nbd_bd_read_block(BD_t * object, uint32_t number)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(object);
	int tries;
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return NULL;
	
	for(tries = 0; tries != NBD_RETRIES; tries++)
	{
		uint8_t command = 0;
		bdesc_t * bdesc;
		int r;
		
		bdesc = bdesc_alloc(object, number, info->blocksize);
		if(!bdesc)
			return NULL;
		
		/* switch to network byte order */
		number = htonl(number);
		
		/* read it */
		r = write(info->fd[1], &command, 1);
		if(r != 1)
			goto error;
		
		r = write(info->fd[1], &number, sizeof(number));
		if(r != sizeof(number))
			goto error;
		
		r = readn(info->fd[0], bdesc->ddesc->data, info->blocksize);
		if(r != info->blocksize)
			goto error;
		
		return bdesc;
		
	error:
		bdesc_drop(&bdesc);
		sleep(tries * 5);
		nbd_bd_reset(object);
	}
	
	fprintf(STDERR_FILENO, "%s(): giving up on %s:%d\n", __FUNCTION__, inet_iptoa(info->ip), info->port);
	return NULL;
}

static int nbd_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(object);
	int tries, r = -1;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->length)
		return -E_INVAL;
	
	for(tries = 0; tries != NBD_RETRIES; tries++)
	{
		uint8_t command = 1;
		uint32_t number;
		
		/* switch to network byte order */
		number = htonl(block->number);
		
		/* write it */
		r = write(info->fd[1], &command, 1);
		if(r != 1)
			goto error;
		
		r = write(info->fd[1], &number, sizeof(number));
		if(r != sizeof(number))
			goto error;
		
		r = write(info->fd[1], block->ddesc->data, info->blocksize);
		if(r != info->blocksize)
			goto error;
		
		/* drop the hot potato */
		bdesc_drop(&block);
		
		return 0;
		
	error:
		sleep(tries * 5);
		nbd_bd_reset(object);
	}
	
	bdesc_drop(&block);
	
	fprintf(STDERR_FILENO, "%s(): giving up on %s:%d\n", __FUNCTION__, inet_iptoa(info->ip), info->port);
	return r;
}

static int nbd_bd_sync(BD_t * object, bdesc_t * block)
{
	return 0;
}

static int nbd_bd_destroy(BD_t * bd)
{
	struct nbd_info * info = (struct nbd_info *) OBJLOCAL(bd);
	int r, val = 0;
	
	val = modman_rem_bd(bd);
	if(val < 0)
		return val;
	
	r = close(info->fd[0]);
	if(r < 0)
		val = r;
	
	r = close(info->fd[1]);
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
	OBJLOCAL(bd) = info;
	
	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = 0;
	OBJASSIGN(bd, nbd_bd, get_config);
	OBJASSIGN(bd, nbd_bd, get_status);
	ASSIGN(bd, nbd_bd, get_numblocks);
	ASSIGN(bd, nbd_bd, get_blocksize);
	ASSIGN(bd, nbd_bd, get_atomicsize);
	ASSIGN(bd, nbd_bd, read_block);
	ASSIGN(bd, nbd_bd, write_block);
	ASSIGN(bd, nbd_bd, sync);
	DESTRUCTOR(bd, nbd_bd, destroy);
	
	if(gethostbyname(address, &info->ip) < 0)
		goto error_info;
	info->port = port;
	
	if(connect(info->ip, port, info->fd))
		goto error_info;
	
	r = read(info->fd[0], &info->length, sizeof(info->length));
	if(r != sizeof(info->length))
		goto error_connect;
	
	r= read(info->fd[0], &info->blocksize, sizeof(info->blocksize));
	if(r != sizeof(info->blocksize))
		goto error_connect;
	
	/* switch to host byte order */
	info->length = ntohl(info->length);
	info->blocksize = ntohs(info->blocksize);
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
	
  error_connect:
	close(info->fd[0]);
	close(info->fd[1]);
  error_info:
	free(info);
  error_bd:
	free(bd);
	return NULL;
}
