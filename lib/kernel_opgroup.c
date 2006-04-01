#include <errno.h>
#include <kfs/opgroup.h>

#define OPGROUP_DEBUG 0
#if OPGROUP_DEBUG
#include <stdio.h>
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...) 
#endif
#define PREFIX "## "

static int next = 1;

opgroup_id_t opgroup_create(int flags)
{
	Dprintf("%s%s() = %d\n", PREFIX, __FUNCTION__, next);
	return next++;

}

int opgroup_add_depend(opgroup_id_t dependent, opgroup_id_t dependency)
{
	Dprintf("%s%s(%d, %d)\n", PREFIX, __FUNCTION__, dependent, dependency);
	if (dependent >= next || dependency >= next)
		return -EINVAL;
	return 0;
}

int opgroup_engage(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}

int opgroup_disengage(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}

int opgroup_release(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}

int opgroup_abandon(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}
