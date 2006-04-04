#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <kfs/kernel_opgroup_ioctl.h>
#include <kfs/opgroup.h>

#define OPGROUP_FILE "/dev/"OPGROUP_DEVICE

#define OPGROUP_DEBUG 0
#if OPGROUP_DEBUG
#include <stdio.h>
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...) 
#endif
#define PREFIX "## "


static int pass_request(int command, opgroup_id_t a, opgroup_id_t b, int flags)
{
	static int dev_fd = -1;
	opgroup_ioctl_cmd_t cmd_args = { .opgroup_a = a, .opgroup_b = b, .flags = flags };
	int r;

	if (dev_fd < 0)
	{
		dev_fd = open(OPGROUP_FILE, O_RDONLY);
		if (dev_fd < 0)
		{
			perror("open(\""OPGROUP_FILE"\")");
			return -errno;
		}
	}

	r = ioctl(dev_fd, command, &cmd_args);
	if (r < 0)
		return -errno;
	return r;
}


opgroup_id_t opgroup_create(int flags)
{
	Dprintf("%s%s()", PREFIX, __FUNCTION__);
	opgroup_id_t id = pass_request(OPGROUP_IOCTL_CREATE, -1, -1, flags);
	Dprintf(" = %d\n", id);
	return id;
}

int opgroup_sync(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_SYNC, opgroup, -1, -1);
}

int opgroup_add_depend(opgroup_id_t dependent, opgroup_id_t dependency)
{
	Dprintf("%s%s(%d, %d)\n", PREFIX, __FUNCTION__, dependent, dependency);
	return pass_request(OPGROUP_IOCTL_ADD_DEPEND, dependent, dependency, -1);
}

int opgroup_engage(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_ENGAGE, opgroup, -1, -1);
}

int opgroup_disengage(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_DISENGAGE, opgroup, -1, -1);
}

int opgroup_release(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_RELEASE, opgroup, -1, -1);
}

int opgroup_abandon(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_ABANDON, opgroup, -1, -1);
}
