#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/modman.h>
#include <kfs/sync.h>
#include <kfs/modman.h>

int kfs_sync(void)
{
	for(;;)
	{
		BD_t * bd;
		modman_it_t it;
		int r = FLUSH_EMPTY;
		
		modman_it_init_bd(&it);
		while((bd = modman_it_next_bd(&it)))
			r |= CALL(bd, flush, FLUSH_DEVICE, NULL);
		
		if(r == FLUSH_EMPTY)
			return 0;
		if(r == FLUSH_NONE)
			return -EBUSY;
	}
}
