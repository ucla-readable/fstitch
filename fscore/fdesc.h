/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_FDESC_H
#define __FSTITCH_FSCORE_FDESC_H

#include <fscore/inode.h>

struct fdesc;
typedef struct fdesc fdesc_t;

struct fdesc_common;
typedef struct fdesc_common fdesc_common_t;

struct fdesc_common {
	inode_t parent; // only valid for fdescs corresponding to directories
};

/* This structure is meant to be "subclassed" by defining new structures with
 * the same first element and casting between them and this type. The subclasses
 * may be further extended by wrapping them in a new inner object, and copying
 * the pointer to the common members (which are shared). This is not quite like
 * subclassing - the idea is to allow each module to have its own local members
 * on the "same" fdesc. */
struct fdesc {
	fdesc_common_t * common;
};

/* Recommendation: modules which actually allocate a new fdesc might consider
 * allocating the fdesc_common statically inside a "subclass" of fdesc, and
 * setting the common pointer to point inside themselves. This saves a call to
 * malloc(). Modules which wrap an fdesc should store a pointer to the wrapped
 * fdesc in their subclass of fdesc, as well as their local data. */

#endif /* __FSTITCH_FSCORE_FDESC_H */
