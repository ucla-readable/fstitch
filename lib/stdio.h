#ifndef KUDOS_LIB_STDIO_H
#define KUDOS_LIB_STDIO_H

#if defined(KUDOS)
#include <inc/stdio.h>
#include <lib/kdprintf.h>
#elif defined(UNIXUSER)
#include <lib/kdprintf.h>
#include <stdio.h>
#elif defined(__KERNEL__)
#include <lib/kdprintf.h>
#define printf(format, ...) kdprintf(STDOUT_FILENO, format, ## __VA_ARGS__)
#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_STDIO_H */
