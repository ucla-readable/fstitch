#include <kfs/cfs.h>
#include <inc/serial_cfs.h>

static CFS_t * frontend_cfs = NULL;

int register_frontend_cfs(CFS_t * cfs)
{
	frontend_cfs = cfs;
	return 0;
}

void cfsipc()
{
	kfsd_shutdown();
}
