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

struct nbd_info {
	int fd[2];
	uint32_t length;
	uint16_t blocksize;
	uint16_t port;
	struct ip_addr ip;
};

static uint32_t nbd_bd_get_numblocks(BD_t * object)
{
	return ((struct nbd_info *) object->instance)->length;
}

static uint16_t nbd_bd_get_blocksize(BD_t * object)
{
	return ((struct nbd_info *) object->instance)->blocksize;
}

static uint16_t nbd_bd_get_atomicsize(BD_t * object)
{
	return ((struct nbd_info *) object->instance)->blocksize;
}

static bdesc_t * nbd_bd_read_block(BD_t * object, uint32_t number)
{
	struct nbd_info * info = (struct nbd_info *) object->instance;
	uint8_t command = 0;
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	if(number >= info->length)
		return NULL;
	
	bdesc = bdesc_alloc(object, number, 0, info->blocksize);
	if(!bdesc)
		return NULL;
	
	/* switch to network byte order */
	number = htonl(number);
	
	/* FIXME check for errors */
	/* read it */
	write(info->fd[1], &command, 1);
	write(info->fd[1], &number, sizeof(number));
	readn(info->fd[0], bdesc->ddesc->data, info->blocksize);
	
	return bdesc;
}

static int nbd_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct nbd_info * info = (struct nbd_info *) object->instance;
	uint8_t command = 1;
	uint32_t number;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->length)
		return -E_INVAL;
	
	/* switch to network byte order */
	number = htonl(block->number);
	
	/* write it */
	write(info->fd[1], &command, 1);
	write(info->fd[1], &number, sizeof(number));
	write(info->fd[1], block->ddesc->data, info->blocksize);
	
	/* drop the hot potato */
	bdesc_drop(&block);
	
	return 0;
}

static int nbd_bd_sync(BD_t * object, bdesc_t * block)
{
	return 0;
}

static int nbd_bd_destroy(BD_t * bd)
{
	struct nbd_info * info = (struct nbd_info *) bd->instance;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	close(info->fd[0]);
	close(info->fd[1]);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * nbd_bd(const char * address, uint16_t port)
{
	struct nbd_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(struct nbd_info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	bd->instance = info;
	
	ASSIGN(bd, nbd_bd, get_numblocks);
	ASSIGN(bd, nbd_bd, get_blocksize);
	ASSIGN(bd, nbd_bd, get_atomicsize);
	ASSIGN(bd, nbd_bd, read_block);
	ASSIGN(bd, nbd_bd, write_block);
	ASSIGN(bd, nbd_bd, sync);
	ASSIGN_DESTROY(bd, nbd_bd, destroy);
	
	if(inet_atoip(address, &info->ip) != 1)
	{
		free(info);
		free(bd);
		return NULL;
	}
	info->port = port;
	
	if(connect(info->ip, port, info->fd))
	{
		free(info);
		free(bd);
		return NULL;
	}
	
	read(info->fd[0], &info->length, sizeof(info->length));
	read(info->fd[0], &info->blocksize, sizeof(info->blocksize));
	
	/* switch to host byte order */
	info->length = ntohl(info->length);
	info->blocksize = ntohs(info->blocksize);
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
