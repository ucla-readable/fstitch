#ifndef __KUDOS_LIB_STDLIB_H
#define __KUDOS_LIB_STDLIB_H

#include <linux/slab.h>
// for non-huge regions only
#define malloc(size) kmalloc(size, GFP_KERNEL)
#define calloc(nmemb, size) kcalloc(nmemb, size, GFP_KERNEL)
#define free(x) kfree(x)

// Sort in ascending order. compar should return a value less than,
// equal to, or greater than zero if 'a' is less than, equal to, or
// greater than 'b', respectively.
void qsort(void * base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));


// "size malloc" or "smal-oc".
// malloc implementation may easily depend on allocation size.

static __inline void * smalloc(size_t size) __attribute__((always_inline));
static __inline void * scalloc(size_t nmemb, size_t size) __attribute__((always_inline));
static __inline void * srealloc(void * p, size_t p_size, size_t new_size) __attribute__((always_inline));
static __inline void sfree(void * p, size_t size) __attribute__((always_inline));


#include <linux/vmalloc.h>

// Use kmalloc iff size is leq KMALLOC_MAX; must be <= kmalloc's max size
#define KMALLOC_MAX (128 * 1024)

static __inline void * smalloc(size_t size)
{
	if (size <= KMALLOC_MAX)
		return kmalloc(size, GFP_KERNEL);
	return vmalloc(size);
}

static __inline void * scalloc(size_t nmemb, size_t size)
{
	size_t total = nmemb * size;
	void * p;

	if (total <= KMALLOC_MAX)
		return kcalloc(nmemb, size, GFP_KERNEL);

	if (unlikely(nmemb != 0 && size > INT_MAX / nmemb))
		return NULL;
	p = vmalloc(total);
	if (unlikely(!p))
		return NULL;
	memset(p, 0, total);
	return p;
}

/* TODO: should/can we optimize? */
static __inline void * srealloc(void * p, size_t p_size, size_t new_size)
{
	void * q = smalloc(new_size);
	if(!q)
		return NULL;
	if(p)
		memcpy(q, p, p_size);
	sfree(p, p_size);
	return q;
}

static __inline void sfree(void * p, size_t size)
{
	if (size <= KMALLOC_MAX)
		kfree(p);
	else
		vfree(p);
}

#endif
