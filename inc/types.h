/* Original provenance: */
/*	$OpenBSD: types.h,v 1.12 1997/11/30 18:50:18 millert Exp $	*/
/*	$NetBSD: types.h,v 1.29 1996/11/15 22:48:25 jtc Exp $	*/

#ifndef KUDOS_INC_TYPES_H
#define KUDOS_INC_TYPES_H

#ifndef NULL
#define NULL ((void *) 0)
#endif

/* Represents true-or-false values */
typedef unsigned char bool;

/* Explicitly-sized versions of integer types */
typedef __signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

/* Registers are 32 bits long. */
typedef int32_t register_t;

/* Pointers and addresses are 32 bits long.
 * We use pointer types to represent virtual addresses, uintptr_t to represent
 * the numerical values of virtual addresss, and physaddr_t to represent a
 * physical address. */
typedef int32_t intptr_t;
typedef uint32_t uintptr_t;
typedef uint32_t physaddr_t;

/* Segment indexes are 16 bits long. */
typedef uint16_t segment_t;

/* Page numbers are 32 bits long. */
typedef uint32_t ppn_t;

/* size_t is used for memory object sizes. */
typedef uint32_t size_t;
/* ssize_t is a signed version of ssize_t, used in case there might be an
   error return. */
typedef int32_t ssize_t;

/* off_t is used for file offsets and lengths. */
/* NOTE: unix-user's off_t is int64_t */
typedef int32_t off_t;

/* static assert, for compile-time assertion checking */
#define static_assert(x)	switch (x) case 0: case (x):

#define offsetof(type, member)  ((size_t) (&((type*)0)->member))

#endif /* !KUDOS_INC_TYPES_H */
