#include <inc/lib.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/partition.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/pc_ptable_bd.h>

struct ptable_info {
	BD_t * bd;
	uint32_t start;
	uint32_t length;
};

#define SECTSIZE 512
	
static uint32_t pc_ptable_bd_get_numblocks(BD_t * object)
{
	return ((struct ptable_info *) object->instance)->length;
}

static uint32_t pc_ptable_bd_get_blocksize(BD_t * object)
{
	return SECTSIZE;
}

static bdesc_t * pc_ptable_bd_read_block(BD_t * object, uint32_t number)
{
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	if(number >= ((struct ptable_info *) object->instance)->length)
		return NULL;
	
	bdesc = CALL(((struct ptable_info *) object->instance)->bd, read_block, ((struct ptable_info *) object->instance)->start + number);
	
	/* ensure we can alter the structure without conflict */
	bdesc_alter(&bdesc);
	
	/* adjust the block descriptor to match the partition */
	bdesc->bd = object;
	bdesc->number = number;
	
	return bdesc;
}

static int pc_ptable_bd_write_block(BD_t * object, bdesc_t * block)
{
	/* make sure this is the right block device */
	if(block->bd != object)
		return -1;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != SECTSIZE)
		return -1;
	
	/* make sure it's a valid block */
	if(block->number >= ((struct ptable_info *) object->instance)->length)
		return -1;
	
	block->translated++;
	block->bd = ((struct ptable_info *) object->instance)->bd;
	block->number -= ((struct ptable_info *) object->instance)->start;
	
	/* write it */
	CALL(block->bd, write_block, block);
	
	block->bd = object;
	block->number += ((struct ptable_info *) object->instance)->start;
	block->translated--;
	
	return 0;
}

static int pc_ptable_bd_sync(BD_t * object, bdesc_t * block)
{
	return 0;
}

static int pc_ptable_bd_destroy(BD_t * bd)
{
	free(bd->instance);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * pc_ptable_bd(BD_t * disk, uint8_t partition)
{
	BD_t * bd;
	bdesc_t * bdesc;
	struct pc_ptable * ptable;
	
	/* partition numbers are 1-based */
	if(!partition || partition > 4)
		return NULL;
	
	/* make sure the block size matches */
	if(CALL(disk, get_blocksize) != SECTSIZE)
		return NULL;
	
	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	bd->instance = malloc(sizeof(struct ptable_info));
	if(!bd->instance)
	{
		free(bd);
		return NULL;
	}
	
	ASSIGN(bd, pc_ptable_bd, get_numblocks);
	ASSIGN(bd, pc_ptable_bd, get_blocksize);
	ASSIGN(bd, pc_ptable_bd, read_block);
	ASSIGN(bd, pc_ptable_bd, write_block);
	ASSIGN(bd, pc_ptable_bd, sync);
	ASSIGN_DESTROY(bd, pc_ptable_bd, destroy);
	
	/* read the partition table */
	bdesc = CALL(disk, read_block, 0);
	
	bdesc_reference(&bdesc);
	if(bdesc->data[PTABLE_MAGIC_OFFSET] != PTABLE_MAGIC[0] || bdesc->data[PTABLE_MAGIC_OFFSET + 1] != PTABLE_MAGIC[1])
	{
		printf("No partition table found!\n");
		bdesc_release(&bdesc);
		free(bd->instance);
		free(bd);
		return NULL;
	}
	
	ptable = (struct pc_ptable *) &bdesc->data[PTABLE_OFFSET];
	partition--;
	
	if(ptable[partition].type != PTABLE_KUDOS_TYPE)
		printf("WARNING: Using non-KudOS partition %d!\n", partition + 1);
	printf("Initialized partition %d: %2x [%d:%d]\n", partition + 1, ptable[partition].type, ptable[partition].lba_start, ptable[partition].lba_length);
	
	((struct ptable_info *) bd->instance)->bd = disk;
	((struct ptable_info *) bd->instance)->start = ptable[partition].lba_start;
	((struct ptable_info *) bd->instance)->length = ptable[partition].lba_length;
	
	bdesc_release(&bdesc);
	
	return bd;
}
