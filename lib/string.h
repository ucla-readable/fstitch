#ifndef __KUDOS_LIB_STRING_H
#define __KUDOS_LIB_STRING_H

#if defined(KUDOS)
#include <inc/string.h>

#elif defined(UNIXUSER)
#include <string.h>

#elif defined(__KERNEL__)
#include <lib/stdlib.h>
#include <linux/string.h>

static __inline char * strdup(const char * s)
{
	size_t len = strlen(s) + 1;
	char * c = malloc(len);
	if (!c)
		return NULL;
	strcpy(c, s);
	return c;
}

#else
#error Unknown target system
#endif

#endif
