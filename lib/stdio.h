#ifndef KUDOS_LIB_STDIO_H
#define KUDOS_LIB_STDIO_H

#if defined(KUDOS)
#include <inc/stdio.h>
#include <lib/kdprintf.h>
#elif defined(UNIXUSER)
#include <lib/kdprintf.h>
#include <stdio.h>
#elif defined(__KERNEL__)
#warning Write Linux kernel support
#include <lib/kdprintf.h>
#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_STDIO_H */
