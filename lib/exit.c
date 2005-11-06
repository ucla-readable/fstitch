#include <inc/lib.h>
#include <stdlib.h>

void
exit(int status)
{
	// KudOS does not have env status values
	USED(status);

	close_all();
	sys_env_destroy(0);
}

