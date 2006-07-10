#ifndef __KUDOS_LIB_STRINGS_H
#define __KUDOS_LIB_STRINGS_H

#if defined(UNIXUSER)
#include <strings.h>

#elif defined(__KERNEL__) || defined(KUDOS)
int strcasecmp(const char * s1, const char * s2);

#else
#error Unknown target system
#endif

#endif
