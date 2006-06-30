#ifndef __KUDOS_LIB_CTYPE_H
#define __KUDOS_LIB_CTYPE_H

#if defined(KUDOS)
#include <inc/ctype.h>

#elif defined(UNIXUSER)
#include <ctype.h>

#elif defined(__KERNEL__)
#include <linux/ctype.h>

#else
#error Unknown target system
#endif

#endif
