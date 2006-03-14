#ifndef __KUDOS_LIB_STDLIB_H
#define __KUDOS_LIB_STDLIB_H

#if defined(KUDOS)
#include <inc/stdlib.h>

#elif defined(UNIXUSER)
#include <stdlib.h>

#elif defined(__KERNEL__)

#include <linux/slab.h>
// For now use kmalloc(GFP_KERNEL), but this will need to change
#define malloc(size) kmalloc(size, GFP_KERNEL)
#define calloc(nmemb, size) kcalloc(nmemb, size, GFP_KERNEL)
#define free(x) kfree(x)

// Sort in ascending order. compar should return a value less than,
// equal to, or greater than zero if 'a' is less than, equal to, or
// greater than 'b', respectively.
void qsort(void * base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));

#else
#error Unknown target system
#endif

#endif
