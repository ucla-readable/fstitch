/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_INC_ASSERT_H
#define KUDOS_INC_ASSERT_H

#include <stdio.h>
#include <lib/panic.h>

#define assert(x)		\
	do { if (!(x)) panic("assertion failed: %s", #x); } while (0)

#endif /* !KUDOS_INC_ASSERT_H */
