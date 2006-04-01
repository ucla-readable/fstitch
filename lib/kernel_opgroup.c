#include <errno.h>
#include <kfs/opgroup.h>

static int next = 1;

opgroup_id_t opgroup_create(int flags)
{
	return next++;
}

int opgroup_add_depend(opgroup_id_t dependent, opgroup_id_t dependency)
{
	if (dependent >= next || dependency >= next)
		return -EINVAL;
	return 0;
}

int opgroup_engage(opgroup_id_t opgroup)
{
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}

int opgroup_disengage(opgroup_id_t opgroup)
{
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}

int opgroup_release(opgroup_id_t opgroup)
{
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}

int opgroup_abandon(opgroup_id_t opgroup)
{
	if (opgroup >= next)
		return -EINVAL;
	return 0;
}
