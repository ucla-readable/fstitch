#ifndef __KUDOS_LIB_STRING_H
#define __KUDOS_LIB_STRING_H

#if defined(KUDOS)
#include <inc/string.h>

#elif defined(UNIXUSER)
#include <string.h>

#elif defined(__KERNEL__)
#include <linux/string.h>

#else
#error Unknown target system
#endif

#endif
