#ifndef KUDOS_LIB_TYPES_H
#define KUDOS_LIB_TYPES_H

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

#if defined(KUDOS)
#include <inc/types.h>
#elif defined(UNIXUSER) || defined(KUTIL)
#include <sys/types.h>
#include <stdint.h>
/* Represents true-or-false values */
typedef unsigned char bool;
#elif defined(__KERNEL__)
#include <linux/types.h>
/* Represents true-or-false values */
typedef unsigned char bool;
#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_TYPES_H */
