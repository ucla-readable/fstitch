#include <kfs/lfs.h>
#include <kfs/cfs.h>
#include <kfs/kfsd.h>
#include <kfs/uhfs.h>

void uhfs_shutdown(void * arg)
{
}

int uhfs_init(int argc, char * argv[])
{
	int r;
	if ((r = kfsd_register_shutdown_module(uhfs_shutdown, NULL)) < 0)
		return r;
	return 0;
}


