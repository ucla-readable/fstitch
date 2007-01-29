#ifndef __KUDOS_LIB_STRING_H
#define __KUDOS_LIB_STRING_H

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

#endif
