#include <inc/lib.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/partition.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/partition_bd.h>
#include <kfs/pc_ptable_bd.h>

struct ptable_info {
	BD_t * bd;
	bdesc_t * ptable_sector;
};
#define ptable ((struct pc_ptable *) &info->ptable_sector->ddesc->data[PTABLE_OFFSET])

#define SECTSIZE 512

/* initialize the PC partition table reader */
void * pc_ptable_init(BD_t * bd)
{
	struct ptable_info * info;
	
	/* make sure the block size is SECTSIZE */
	if(CALL(bd, get_blocksize) != SECTSIZE)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	
	/* read the partition table */
	info->ptable_sector = CALL(bd, read_block, 0);
	if(!info->ptable_sector)
	{
		free(info);
		return NULL;
	}
	bdesc_retain(&info->ptable_sector);
	
	info->bd = bd;
	
	if(info->ptable_sector->ddesc->data[PTABLE_MAGIC_OFFSET] != PTABLE_MAGIC[0] ||
	   info->ptable_sector->ddesc->data[PTABLE_MAGIC_OFFSET + 1] != PTABLE_MAGIC[1])
	{
		printf("No partition table found!\n");
		bdesc_release(&info->ptable_sector);
		free(info);
		return NULL;
	}
	
	return info;
}

/* count the partitions */
int pc_ptable_count(void * _info)
{
	struct ptable_info * info = (struct ptable_info *) _info;
	int i, count = 0;
	
	for(i = 0; i != 4; i++)
		if(ptable[i].lba_length)
			count++;
	
	return count;
}

/* get the partition type */
uint8_t pc_ptable_type(void * _info, int index)
{
	struct ptable_info * info = (struct ptable_info *) _info;
	if(index < 1 || index > 4)
		return 0;
	return ptable[index - 1].type;
}

/* get a partition block device */
BD_t * pc_ptable_bd(void * _info, int index)
{
	struct ptable_info * info = (struct ptable_info *) _info;
	
	if(index < 1 || index > 4)
		return 0;
	
	index--;
	return partition_bd(info->bd, ptable[index].lba_start, ptable[index].lba_length);
}

/* free the partition table structures */
void pc_ptable_free(void * info)
{
	bdesc_release(&((struct ptable_info *) info)->ptable_sector);
	free(info);
}
