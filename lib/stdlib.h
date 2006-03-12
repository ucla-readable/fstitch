#ifndef __KUDOS_LIB_STDLIB_H
#define __KUDOS_LIB_STDLIB_H

#if defined(KUDOS)
#include <inc/stdlib.h>

#elif defined(UNIXUSER)
#include <stdlib.h>

#elif defined(__KERNEL__)
#include <linux/slab.h>
// For now use kmalloc(GFP_KERNEL), but this will need to change
#define malloc(x) kmalloc(x, GFP_KERNEL)
#define free(x) kfree(x)

// TODO: qsort (could use xfs support's)

#else
#error Unknown target system
#endif

#endif
