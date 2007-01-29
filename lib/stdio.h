#ifndef KUDOS_LIB_STDIO_H
#define KUDOS_LIB_STDIO_H

#include <lib/kdprintf.h>
#define printf(format, ...) kdprintf(STDOUT_FILENO, format, ## __VA_ARGS__)

#endif /* !KUDOS_LIB_STDIO_H */
