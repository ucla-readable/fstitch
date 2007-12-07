/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <errno.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stddef.h>
#include <fcntl.h>
#include <fscore/kernel_patchgroup_ioctl.h>
#include <fscore/patchgroup.h>

#define PATCHGROUP_FILE "/dev/"PATCHGROUP_DEVICE

/* Set to emulate patchgroup operations for patchgroup development on systems
 * without patchgroup support */
#define PATCHGROUP_EMULATE 0

#define PATCHGROUP_DEBUG 0
#if PATCHGROUP_DEBUG
#include <stdio.h>
#define Dprintf(fmt, ...) fprintf(stderr, fmt, __VA_ARGS__)
#else
#define Dprintf(fmt, ...)
#endif
#define PREFIX "## "

static int pass_request(int command, patchgroup_id_t a, patchgroup_id_t b, int flags, const char * str)
{
#if PATCHGROUP_EMULATE
	static patchgroup_id_t next_patchgroup = 0;

	if (command == PATCHGROUP_IOCTL_CREATE)
		return next_patchgroup++;
	if (a < 0 || next_patchgroup <= a)
		return -EINVAL;
	if (command == PATCHGROUP_IOCTL_ADD_DEPEND && (b < 0 || next_patchgroup <= b))
		return -EINVAL;
	return 0;
#else
	static int dev_fd = -1;
	patchgroup_ioctl_cmd_t cmd_args =
	    { .patchgroup_a = a, .patchgroup_b = b, .flags = flags, .str = str };
	int r;

	if (dev_fd < 0)
	{
		dev_fd = open(PATCHGROUP_FILE, O_RDONLY);
		if (dev_fd < 0)
		{
			perror("open(\""PATCHGROUP_FILE"\")");
			return -errno;
		}
	}

	r = ioctl(dev_fd, command, &cmd_args);
	if (r < 0)
		return -errno;
	return r;
#endif
}

patchgroup_id_t patchgroup_create(int flags)
{
	Dprintf("%s%s()", PREFIX, __FUNCTION__);
	patchgroup_id_t id = pass_request(PATCHGROUP_IOCTL_CREATE, -1, -1, flags, NULL);
	Dprintf(" = %d\n", id);
	return id;
}

int patchgroup_sync(patchgroup_id_t patchgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, patchgroup);
	return pass_request(PATCHGROUP_IOCTL_SYNC, patchgroup, -1, -1, NULL);
}

int patchgroup_add_depend(patchgroup_id_t after, patchgroup_id_t before)
{
	Dprintf("%s%s(%d, %d)\n", PREFIX, __FUNCTION__, after, before);
	return pass_request(PATCHGROUP_IOCTL_ADD_DEPEND, after, before, -1, NULL);
}

int patchgroup_engage(patchgroup_id_t patchgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, patchgroup);
	return pass_request(PATCHGROUP_IOCTL_ENGAGE, patchgroup, -1, -1, NULL);
}

int patchgroup_disengage(patchgroup_id_t patchgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, patchgroup);
	return pass_request(PATCHGROUP_IOCTL_DISENGAGE, patchgroup, -1, -1, NULL);
}

int patchgroup_release(patchgroup_id_t patchgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, patchgroup);
	return pass_request(PATCHGROUP_IOCTL_RELEASE, patchgroup, -1, -1, NULL);
}

int patchgroup_abandon(patchgroup_id_t patchgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, patchgroup);
	return pass_request(PATCHGROUP_IOCTL_ABANDON, patchgroup, -1, -1, NULL);
}


int patchgroup_create_engage(patchgroup_id_t previous, ...)
{
	patchgroup_id_t new;
	va_list ap;
	int r;

	if ((new = patchgroup_create(0)) < 0)
		return new;
	if (previous >= 0)
	{
		patchgroup_id_t prev;

		if ((r = patchgroup_add_depend(new, previous)) < 0)
			goto error_abandon;

		va_start(ap, previous);
		while ((prev = va_arg(ap, patchgroup_id_t)) >= 0)
		if ((r = patchgroup_add_depend(new, prev)) < 0)
		{
			va_end(ap);
			goto error_abandon;
		}
		va_end(ap);
	}
		
	if ((r = patchgroup_release(new)) < 0)
		goto error_abandon;
	if ((r = patchgroup_engage(new)) < 0)
		goto error_abandon;
	return new;

  error_abandon:
	(void) patchgroup_abandon(new);
	return r;
}

int patchgroup_linear(patchgroup_id_t previous)
{
	patchgroup_id_t new;
	int r;

	if ((new = patchgroup_create(0)) < 0)
		return new;
	if (previous >= 0)
		if ((r = patchgroup_add_depend(new, previous)) < 0)
			goto error_abandon;
	if ((r = patchgroup_release(new)) < 0)
		goto error_abandon;
	if ((r = patchgroup_engage(new)) < 0)
		goto error_abandon;
	if (previous >= 0)
		if ((r = patchgroup_abandon(previous)) < 0)
			goto error_abandon;
	return new;

  error_abandon:
	(void) patchgroup_abandon(new);
	return r;
}


int patchgroup_label(patchgroup_id_t patchgroup, const char * label)
{
	Dprintf("%s%s(%d, \"%s\")\n", PREFIX, __FUNCTION__, patchgroup, label);
	return pass_request(PATCHGROUP_IOCTL_LABEL, patchgroup, -1, -1, label);
}


int txn_start(const char * path)
{
	Dprintf("%s%s(\"%s\")\n", PREFIX, __FUNCTION__, path);
	return pass_request(PATCHGROUP_IOCTL_TXN_START, -1, -1, -1, path);
}

int txn_finish(void)
{
	Dprintf("%s%s()\n", PREFIX, __FUNCTION__);
	return pass_request(PATCHGROUP_IOCTL_TXN_FINISH, -1, -1, -1, NULL);
}

int txn_abort(void)
{
	Dprintf("%s%s()\n", PREFIX, __FUNCTION__);
	return pass_request(PATCHGROUP_IOCTL_TXN_ABORT, -1, -1, -1, NULL);
}
