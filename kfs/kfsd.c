#include <inc/lib.h>
#include <kfs/ide_pio_bd.h>

void umain(int argc, char * argv[])
{
	BD_t * bd;
	
	if(sys_grant_io(0))
	{
		printf("Failed to get I/O priveleges.\n");
		return;
	}
	
	bd = ide_pio_bd(0);
	
	printf("BD block size is %d, block count is %d\n", CALL(bd, get_blocksize), CALL(bd, get_numblocks));
	
	DESTROY(bd);
}
