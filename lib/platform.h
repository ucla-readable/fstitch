#ifndef __KUDOS_KFS_PLATFORM_H
#define __KUDOS_KFS_PLATFORM_H

#ifdef __KERNEL__

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
#include <linux/config.h>
#endif

#include <linux/ctype.h>
#include <linux/fcntl.h>
#include <linux/errno.h>

/* stdio.h */
#define printf(format, ...) printk(format, ## __VA_ARGS__)
/* assume that fprintf is only used for stderr */
#define fprintf(stream, format, ...) printk(KERN_EMERG format, ## __VA_ARGS__)

#include <linux/string.h>

#include <lib/kernel-stdlib.h>
#include <lib/kernel-stdarg.h>
#include <lib/kernel-assert.h>

static __inline char * strdup(const char * s)
{
	size_t len = strlen(s) + 1;
	char * c = malloc(len);
	if(!c)
		return NULL;
	strcpy(c, s);
	return c;
}

static __inline int strcasecmp(const char * s1, const char * s2)
{
	while(tolower(*s1) == tolower(*s2++))
		if(*s1++ == '\0')
			return 0;
	return tolower(*s1) - tolower(*--s2);
}

#include <linux/types.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
typedef unsigned char bool;
#endif

#elif defined(UNIXUSER)

#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include <stdio.h>

#include <string.h>

#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <stdint.h>

typedef unsigned char bool;

/* Duplicate some things we do in the Linux kernel */
#define kpanic(x...) do { fprintf(stderr, x); abort(); } while(0)
#define smalloc(x) malloc(x)
#define scalloc(x, y) calloc(x, y)
#define srealloc(x, y, z) realloc(x, z)
#define sfree(x, y) free(x)

/* Duplicate some Linux kernel things */
#define PAGE_SIZE 4096

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#undef offsetof
#ifdef __compiler_offsetof
#define offsetof(TYPE,MEMBER) __compiler_offsetof(TYPE,MEMBER)
#else
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#define container_of(ptr, type, member) ({typeof(((type *) 0)->member) * __mptr = (ptr); (type *) (unsigned long) ((char *) __mptr - offsetof(type,member));})

#endif /* __KERNEL__, UNIXUSER */

/* Efficient min and max operations */ 
#define MIN(_a, _b)	\
	({		\
		typeof(_a) __a = (_a);	\
		typeof(_b) __b = (_b);	\
		__a <= __b ? __a : __b;	\
	})

#define MAX(_a, _b)	\
	({		\
		typeof(_a) __a = (_a);	\
		typeof(_b) __b = (_b);	\
		__a >= __b ? __a : __b;	\
	})

/* 32-bit integer rounding; only works for n = power of two */
#define ROUNDUP32(a, n)		\
	({ uint32_t __n = (n);  (((uint32_t) (a) + __n - 1) & ~(__n - 1)); })
#define ROUND32(a, n)		ROUNDUP32((a), (n))
#define ROUNDDOWN32(a, n)	(((uint32_t) (a)) & ~((n) - 1))

// static_assert(x) will generate a compile-time error if 'x' is false.
#define static_assert(x) switch (x) case 0: case (x):

#endif /* __KUDOS_KFS_PLATFORM_H */
