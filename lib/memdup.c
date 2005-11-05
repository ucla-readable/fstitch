#include <string.h>
#include <malloc.h>
#include <lib/memdup.h>

#ifndef KUDOS_KERNEL
void *
memdup(const void *src, size_t len)
{
	void * copy = malloc(len);
	if(copy)
		memcpy(copy, src, len);
	return copy;
}
#endif
