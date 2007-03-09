#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stddef.h>
#include <fcntl.h>
#include <kfs/kernel_opgroup_ioctl.h>
#include <kfs/opgroup.h>

#define OPGROUP_FILE "/dev/"OPGROUP_DEVICE

/* Set to emulate opgroup operations for opgroup development on systems
 * without opgroup support */
#define OPGROUP_EMULATE 1

#define OPGROUP_DEBUG 1
#if OPGROUP_DEBUG
#include <stdio.h>
#define Dprintf(x...) fprintf(stderr, x)
#else
#define Dprintf(x...) 
#endif
#define PREFIX "## "

static int pass_request(int command, opgroup_id_t a, opgroup_id_t b, int flags, const char * str)
{
#if OPGROUP_EMULATE
	static opgroup_id_t next_opgroup = 0;

	if (command == OPGROUP_IOCTL_CREATE)
		return next_opgroup++;
	if (a < 0 || next_opgroup <= a)
		return -EINVAL;
	if (command == OPGROUP_IOCTL_ADD_DEPEND && (b < 0 || next_opgroup <= b))
		return -EINVAL;
	return 0;
#else
	static int dev_fd = -1;
	opgroup_ioctl_cmd_t cmd_args =
	    { .opgroup_a = a, .opgroup_b = b, .flags = flags, .str = str };
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
#endif
}

opgroup_id_t opgroup_create(int flags)
{
	Dprintf("%s%s()", PREFIX, __FUNCTION__);
	opgroup_id_t id = pass_request(OPGROUP_IOCTL_CREATE, -1, -1, flags, NULL);
	Dprintf(" = %d\n", id);
	return id;
}

int opgroup_sync(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_SYNC, opgroup, -1, -1, NULL);
}

int opgroup_add_depend(opgroup_id_t after, opgroup_id_t before)
{
	Dprintf("%s%s(%d, %d)\n", PREFIX, __FUNCTION__, after, before);
	return pass_request(OPGROUP_IOCTL_ADD_DEPEND, after, before, -1, NULL);
}

int opgroup_engage(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_ENGAGE, opgroup, -1, -1, NULL);
}

int opgroup_disengage(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_DISENGAGE, opgroup, -1, -1, NULL);
}

int opgroup_release(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_RELEASE, opgroup, -1, -1, NULL);
}

int opgroup_abandon(opgroup_id_t opgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, opgroup);
	return pass_request(OPGROUP_IOCTL_ABANDON, opgroup, -1, -1, NULL);
}


int opgroup_create_engage(opgroup_id_t previous, ...)
{
	opgroup_id_t new;
	va_list ap;
	int r;

	if ((new = opgroup_create(0)) < 0)
		return new;
	if (previous >= 0)
	{
		opgroup_id_t prev;

		if ((r = opgroup_add_depend(new, previous)) < 0)
			goto error_abandon;

		va_start(ap, previous);
		while ((prev = va_arg(ap, opgroup_id_t)) >= 0)
		if ((r = opgroup_add_depend(new, prev)) < 0)
		{
			va_end(ap);
			goto error_abandon;
		}
		va_end(ap);
	}
		
	if ((r = opgroup_release(new)) < 0)
		goto error_abandon;
	if ((r = opgroup_engage(new)) < 0)
		goto error_abandon;
	return new;

  error_abandon:
	(void) opgroup_abandon(new);
	return r;
}

int opgroup_linear(opgroup_id_t previous)
{
	opgroup_id_t new;
	int r;

	if ((new = opgroup_create(0)) < 0)
		return new;
	if (previous >= 0)
	{
		if ((r = opgroup_add_depend(new, previous)) < 0)
			goto error_abandon;
		if ((r = opgroup_abandon(previous)) < 0)
			goto error_abandon;
		previous = -1;
	}
	if ((r = opgroup_release(new)) < 0)
		goto error_abandon; /* Bad: can't unabandon previous */
	if ((r = opgroup_engage(new)) < 0)
		goto error_abandon; /* Bad: can't unabandon previous */
	return new;

  error_abandon:
	(void) opgroup_abandon(new);
	return r;
}


int opgroup_label(opgroup_id_t opgroup, const char * label)
{
	Dprintf("%s%s(%d, \"%s\")\n", PREFIX, __FUNCTION__, opgroup, label);
	return pass_request(OPGROUP_IOCTL_LABEL, opgroup, -1, -1, label);
}
