#include <lib/platform.h>

#include <fscore/bd.h>
#include <fscore/modman.h>
#include <fscore/sync.h>
#include <fscore/modman.h>

int fstitch_sync(void)
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
