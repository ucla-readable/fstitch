// XXX change this to use printfmt properly,
// with no string length limitation.

#if defined(KUDOS)

#include <inc/lib.h>

int
kdprintf(int fd, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return write(fd, buf, strlen(buf));
}
 
// The same as kdprintf(), but if fd 1 is not mapped prints to console
int
printf(const char *fmt, ...)
{
 	va_list ap;
	struct Fd* stdout;
 
 	va_start(ap, fmt);
	if(0 <= fd_lookup(1, &stdout)) {
		// fd 1 is mapped, do as kdprintf() does
		char buf[256];

		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		return write(1, buf, strlen(buf));
	} else {
		// fd 1 is not mapped, print to the console just as printf_c() does
		int cnt;

		cnt = vprintf(fmt, ap);
		va_end(ap);
		return cnt;
	}
}

#elif defined(UNIXUSER)

#if defined(__linux__)
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int
kdprintf(int fd, const char *fmt, ...)
{
	char *buf;
	int r;
	va_list ap;

	va_start(ap, fmt);
	vasprintf(&buf, fmt, ap);
	assert(buf);
	va_end(ap);
	r = write(fd, buf, strlen(buf));
	free(buf);
	return r;
}

#else
#error Unknown target system
#endif
