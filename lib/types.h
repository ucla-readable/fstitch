#ifndef KUDOS_LIB_TYPES_H
#define KUDOS_LIB_TYPES_H

#if defined(KUDOS)
#include <inc/types.h>
#elif defined(UNIXUSER) || defined(KUTIL)
#include <sys/types.h>
#include <stdint.h>
/* Represents true-or-false values */
typedef unsigned char bool;
#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_TYPES_H */
