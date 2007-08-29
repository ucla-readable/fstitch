/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef FSTITCH_INC_HASH_SET_H
#define FSTITCH_INC_HASH_SET_H

#include <lib/hash_map.h>

struct hash_set;
typedef struct hash_set hash_set_t;

// Create a hash_set.
hash_set_t * hash_set_create(void);
// Create a hash_set, reserve space for n entries, allow/don't auto resizing.
hash_set_t * hash_set_create_size(size_t n, bool auto_resize);
// Destroy a hash_set, does not elements.
void         hash_set_destroy(hash_set_t * hs);

// Return number of items in the hash_set.
size_t hash_set_size(const hash_set_t * hs);
// Return whether hash_set is empty.
bool   hash_set_empty(const hash_set_t * hs);
// Insert the given element.
// Returns 0 or 1 on success, or -ENOMEM.
int    hash_set_insert(hash_set_t * hs, void * elt);
// Remove the given element, does not destory elt.
// Returns k's value on success, NULL if k is not in the hash_set.
void * hash_set_erase(hash_set_t * hs, const void * elt);
// Remove all elements, does not destroy elements.
void   hash_set_clear(hash_set_t * hs);
// Return whether the hash_set contains elt.
bool   hash_set_exists(const hash_set_t * hs, const void * elt);

// Return the number of buckets currently allocated.
size_t hash_set_bucket_count(const hash_set_t * hs);
// Resize the number of buckets to n.
// Returns 0 on success, 1 on no resize needed, or -ENOMEM.
int    hash_set_resize(hash_set_t * hs, size_t n);


// Iteration

typedef hash_map_it_t hash_set_it_t;

void hash_set_it_init(hash_set_it_t * it, hash_set_t * hs);
// Iterate through the hash set values using hs_it.
// - Returns NULL when the end of the hash set is reached.
// - Behavior is undefined begin iterating, modify hs, and then continue
//   iterating using the old hs_it.
void * hash_set_next(hash_set_it_t * it);

#endif /* !FSTITCH_INC_HASH_SET_H */
