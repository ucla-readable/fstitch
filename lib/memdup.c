#include <lib/string.h>
#include <lib/stdlib.h>
#include <lib/memdup.h>

void *
memdup(const void *src, size_t len)
{
	void * copy = malloc(len);
	if(copy)
		memcpy(copy, src, len);
	return copy;
}
