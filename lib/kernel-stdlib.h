/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_LIB_STDLIB_H
#define __FSTITCH_LIB_STDLIB_H

#define MALLOC_ACCOUNT 0

#include <linux/slab.h>
// for non-huge regions only
static inline void * malloc(size_t size)
{
#if MALLOC_ACCOUNT
	extern unsigned long long malloc_total;
	malloc_total += size;
#endif
	return kmalloc(size, GFP_KERNEL);
}

static inline void * calloc(size_t nmemb, size_t size)
{
#if MALLOC_ACCOUNT
	extern unsigned long long malloc_total;
	malloc_total += nmemb * size;
#endif
	return kcalloc(nmemb, size, GFP_KERNEL);
}

static inline void free(const void * x)
{
	kfree(x);
}

long strtol(const char * str, char ** end, int base);


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
#if MALLOC_ACCOUNT
	extern unsigned long long malloc_total;
	malloc_total += size;
#endif
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
#if MALLOC_ACCOUNT
	extern unsigned long long malloc_total;
	malloc_total += total;
#endif
	return p;
}

/* TODO: should/can we optimize? One way: 2.6.22 has krealloc(). */
static __inline void * srealloc(void * p, size_t p_size, size_t new_size)
{
	void * q = smalloc(new_size);
	if(!q)
		return NULL;
	if(p)
		memcpy(q, p, p_size);
	sfree(p, p_size);
#if MALLOC_ACCOUNT
	extern unsigned long long malloc_total;
	malloc_total += new_size - p_size;
#endif
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
