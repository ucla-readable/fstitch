/* This file contains the magic DEP MAN! */

#include <inc/stdio.h>

#include <kfs/depman.h>
#include <kfs/bdesc.h>

int depman_forward_chdesc(bdesc_t * from, bdesc_t * to)
{
	printf("DEP MAN: bdesc 0x%08x -> 0x%08x\n", from, to);
	return 0;
}
