#include <inc/lib.h>
#include <inc/partition.h>

#include <kfs/bdesc.h>
#include <kfs/ide_pio_bd.h>
#include <kfs/pc_ptable_bd.h>

static uint32_t bdesc_sum(bdesc_t * bdesc)
{
	uint32_t sum = 0;
	uint32_t i;
	for(i = 0; i != bdesc->length; i++)
	{
		sum *= 3;
		sum += bdesc->data[i];
	}
	return sum;
}

void umain(int argc, char * argv[])
{
	BD_t * bd;
	BD_t * part = NULL;
	void * ptbl;
	uint32_t i;
	
	if(sys_grant_io(0))
	{
		printf("Failed to get I/O priveleges.\n");
		return;
	}
	
	bd = ide_pio_bd(0);
	ptbl = pc_ptable_init(bd);
	if(ptbl)
	{
		uint32_t max = pc_ptable_count(ptbl);
		printf("Found %d partitions.\n", max);
		for(i = 1; i <= max; i++)
		{
			uint8_t type = pc_ptable_type(ptbl, i);
			printf("Partition %d has type %02x\n", i, type);
			if(type == PTABLE_KUDOS_TYPE && !part)
				part = pc_ptable_bd(ptbl, i);
		}
		pc_ptable_free(ptbl);
	}
	else
	{
		printf("Using whole disk.\n");
		part = bd;
	}
	
	if(!part)
	{
		printf("No KudOS partition found!\n");
		exit();
	}
	
	printf("BD block size is %d, block count is %d\n", CALL(bd, get_blocksize), CALL(bd, get_numblocks));
	printf("PART block size is %d, block count is %d\n", CALL(part, get_blocksize), CALL(part, get_numblocks));
	
	for(i = 0; i != 5; i++)
	{
		bdesc_t * bdesc;
		
		printf("Block %d sum", i);
		
		bdesc = CALL(bd, read_block, i);
		bdesc_retain(&bdesc);
		printf(": BD 0x%08x", bdesc_sum(bdesc));
		bdesc_release(&bdesc);
		
		bdesc = CALL(part, read_block, i);
		bdesc_retain(&bdesc);
		printf(", PART 0x%08x", bdesc_sum(bdesc));
		bdesc_release(&bdesc);
		
		printf("\n");
	}
	
	DESTROY(part);
	DESTROY(bd);
}
