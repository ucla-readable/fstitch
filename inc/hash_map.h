#ifndef KUDOS_INC_HASH_MAP_H
#define KUDOS_INC_HASH_MAP_H

#include <inc/types.h>

struct hash_map_elt {
	void * key;
	void * val;
};
typedef struct hash_map_elt hash_map_elt_t;

struct hash_map;
typedef struct hash_map hash_map_t;

// Create a hash_map.
hash_map_t * hash_map_create(void);
// Create a hash_map, reserve space for n entries, allow/don't auto resizing.
hash_map_t * hash_map_create_size(size_t n, bool auto_resize);
// Destroy a hash_map, does not destroy keys or vals.
void         hash_map_destroy(hash_map_t * hm);

// Return number of items in the hash_map.
size_t hash_map_size(const hash_map_t * hm);
// Return whether hash_map is empty.
bool   hash_map_empty(const hash_map_t * hm);
// Insert the given key-val pair, updating k's v if k exists.
bool   hash_map_insert(hash_map_t * hm, void * k, void * v);
// Remove the given key-val pair, does not destory key or val.
bool   hash_map_erase(hash_map_t * hm, const void * k);
// Change the mapping from oldk->val to be newk->val.
bool   hash_map_change_key(hash_map_t * hm, void * oldk, void * newk);
// Remove all key-val pairs, does not destroy keys or vals.
void   hash_map_clear(hash_map_t * hm);
// Return the val associated with k.
void * hash_map_find_val(const hash_map_t * hm, const void * k);
// Return hte key and val associated with k.
hash_map_elt_t hash_map_find_elt(const hash_map_t * hm, const void * k);

// Return the number of buckets currently allocated.
size_t hash_map_bucket_count(const hash_map_t * hm);
// Increase the number of buckets to at least n.
bool   hash_map_resize(hash_map_t * hm, size_t n);

// Implement if useful
/*
hash_map_elt_t hash_map_elt_begin(hash_map_t * hm);
hash_map_elt_t hash_map_elt_end(hash_map_t * hm);
hash_map_elt_t hash_map_elt_next(hash_map_t * hm, hash_map_elt_t elt);
*/

#endif /* !KUDOS_INC_HASH_MAP_H */
