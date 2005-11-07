#ifndef KUDOS_LIB_KDPRINTF_H
#define KUDOS_LIB_KDPRINTF_H

#if defined(KUDOS)
#include <inc/stdio.h>

#elif defined(UNIXUSER)
#include <stdio.h> // [v]printf
#include <stdarg.h> // va_list et al
#include <unistd.h> // STD[IN|OUT|ERR]_FILENO

int	kdprintf(int fd, const char*, ...);
#else
#error Unknown target system
#endif

#endif /* !KUDOS_INC_STDIO_H */
