#ifndef KUDOS_LIB_KDPRINTF_H
#define KUDOS_LIB_KDPRINTF_H

#if defined(KUDOS)
#include <inc/stdio.h>

#elif defined(UNIXUSER)
#include <stdio.h> // [v]printf
#include <stdarg.h> // va_list et al
#include <unistd.h> // STD[IN|OUT|ERR]_FILENO

int	kdprintf(int fd, const char*, ...);

#elif defined(__KERNEL__)
#include <linux/kernel.h>
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int kdprintf(int fd, const char * fmt, ...);

// TODO: macro define kdprintf() to alter printk's log level
#if 0
#define kdprintf(fd, fmt, ...)
({
	if (fd == STDOUT_FILENO) printk(KERN_INFO fmt, ## __VA_ARGS__);
	else if (fd == STDERR_FILENO) printk(KERN_ERR fmt, ## __VA_ARGS__);
	else printk(KERN_ERR "(UNKNOWN FD) " fmt, ## __VA_ARGS__);
})
#endif

#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_KDPRINTF_H */
