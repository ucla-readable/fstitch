/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <lib/patchgroup_trace.h>
#include <fscore/kernel_patchgroup_ioctl.h>
#include <fscore/patchgroup.h>

#define PATCHGROUP_FILE "/dev/"PATCHGROUP_DEVICE

/* Set to emulate patchgroup operations for patchgroup development on systems
 * without patchgroup support */
#define PATCHGROUP_EMULATE 0

/* Set to enable producing a trace file with all patchgroup operations for
 * making graphs of application patchgroups when the environment variable
 * given by PATCHGROUP_TRACE_ENV below is set to a file name. */
#define PATCHGROUP_TRACE 1
#define PATCHGROUP_TRACE_ENV "PATCHGROUP_TRACE"
#define PATCHGROUP_TRACE_APPEND_ENV PATCHGROUP_TRACE_ENV"_APPEND"

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

#if PATCHGROUP_TRACE
static int trace_fd = -1;
static int trace_init = 0;

static void init_trace(void)
{
	char * file = getenv(PATCHGROUP_TRACE_ENV);
	char * append = getenv(PATCHGROUP_TRACE_APPEND_ENV);
	int flags = O_WRONLY | O_APPEND;
	trace_init = 1;
	if (!file || !*file)
		return;
	if (!append || !strcmp(append, "0") || !strcmp(append, "false") || !strcmp(append, "no"))
	{
		unlink(file);
		flags |= O_CREAT | O_EXCL;
		/* so that child processes will append to the current log */
		putenv(PATCHGROUP_TRACE_APPEND_ENV"=1");
	}
	trace_fd = open(file, flags, 0644);
	if (trace_fd >= 0)
	{
		struct pgt_header trace;
		trace.magic = PGT_MAGIC;
		trace.version = PGT_VERSION;
		write(trace_fd, &trace, sizeof(trace));
	}
}
#endif

patchgroup_id_t patchgroup_create(int flags)
{
	Dprintf("%s%s()", PREFIX, __FUNCTION__);
	patchgroup_id_t id = pass_request(PATCHGROUP_IOCTL_CREATE, -1, -1, flags, NULL);
	Dprintf(" = %d\n", id);
#if PATCHGROUP_TRACE
	if (!trace_init)
		init_trace();
	if (trace_fd >= 0 && id > 0)
	{
		struct pgt_create trace;
		trace.all.type = PATCHGROUP_IOCTL_CREATE;
		trace.all.pid = getpid();
		trace.all.time = time(NULL);
		trace.id = id;
		write(trace_fd, &trace, sizeof(trace));
	}
#endif
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
	int r = pass_request(PATCHGROUP_IOCTL_ADD_DEPEND, after, before, -1, NULL);
#if PATCHGROUP_TRACE
	if (!trace_init)
		init_trace();
	if (trace_fd >= 0 && r >= 0)
	{
		struct pgt_add_depend trace;
		trace.all.type = PATCHGROUP_IOCTL_ADD_DEPEND;
		trace.all.pid = getpid();
		trace.all.time = time(NULL);
		trace.after = after;
		trace.before = before;
		write(trace_fd, &trace, sizeof(trace));
	}
#endif
	return r;
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
	int r = pass_request(PATCHGROUP_IOCTL_RELEASE, patchgroup, -1, -1, NULL);
#if PATCHGROUP_TRACE
	if (!trace_init)
		init_trace();
	if (trace_fd >= 0 && r >= 0)
	{
		struct pgt_release trace;
		trace.all.type = PATCHGROUP_IOCTL_RELEASE;
		trace.all.pid = getpid();
		trace.all.time = time(NULL);
		trace.id = patchgroup;
		write(trace_fd, &trace, sizeof(trace));
	}
#endif
	return r;
}

int patchgroup_abandon(patchgroup_id_t patchgroup)
{
	Dprintf("%s%s(%d)\n", PREFIX, __FUNCTION__, patchgroup);
	int r = pass_request(PATCHGROUP_IOCTL_ABANDON, patchgroup, -1, -1, NULL);
#if PATCHGROUP_TRACE
	if (!trace_init)
		init_trace();
	if (trace_fd >= 0 && r >= 0)
	{
		struct pgt_abandon trace;
		trace.all.type = PATCHGROUP_IOCTL_ABANDON;
		trace.all.pid = getpid();
		trace.all.time = time(NULL);
		trace.id = patchgroup;
		write(trace_fd, &trace, sizeof(trace));
	}
#endif
	return r;
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
	int r = pass_request(PATCHGROUP_IOCTL_LABEL, patchgroup, -1, -1, label);
#if PATCHGROUP_TRACE
	if (!trace_init)
		init_trace();
	if (trace_fd >= 0 && r >= 0)
	{
		struct iovec iov[2];
		struct pgt_label trace;
		trace.all.type = -1;
		trace.all.pid = getpid();
		trace.all.time = time(NULL);
		trace.id = patchgroup;
		trace.label_len = strlen(label);
		iov[0].iov_base = &trace;
		iov[0].iov_len = sizeof(trace);
		iov[1].iov_base = (char *) label;
		iov[1].iov_len = trace.label_len;
		writev(trace_fd, iov, 2);
	}
#endif
	return r;
}
