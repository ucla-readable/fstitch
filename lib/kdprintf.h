#ifndef KUDOS_LIB_KDPRINTF_H
#define KUDOS_LIB_KDPRINTF_H

#if defined(KUDOS)
#include <inc/stdio.h>

#elif defined(UNIXUSER)
#define _GNU_SOURCE
#include <stdio.h> // [v]dprintf
#include <stdarg.h> // va_list et al
#include <unistd.h> // STD[IN|OUT|ERR]_FILENO
//#if defined(dprintf)

// FIXME: we can't reinclude stdio.h if it was already included
extern int dprintf(int, const char*, ...);
extern int vdprintf(int, const char*, va_list);

static __inline int kdprintf(int fd, const char*, ...) __attribute__((always_inline));
static __inline int kdprintf(int fd, const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	int r = vdprintf(fd, format, ap);
	va_end(ap);
	return r;
}
static __inline int vkdprintf(int fd, const char*, va_list) __attribute__((always_inline));
static __inline int vkdprintf(int fd, const char *format, va_list ap)
{
	return vdprintf(fd, format, ap);
}
//#else
//#error Your os needs [v]kdprintf implementations
//#endif

#else
#error Unknown target system
#endif

#endif /* !KUDOS_INC_STDIO_H */
