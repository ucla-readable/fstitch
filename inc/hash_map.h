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
// Returns 0 or 1 on success, or -E_NO_MEM.
int    hash_map_insert(hash_map_t * hm, void * k, void * v);
// Remove the given key-val pair, does not destory key or val.
// Returns k's value on success, NULL if k is not in the hash_map.
void * hash_map_erase(hash_map_t * hm, const void * k);
// Change the mapping from oldk->val to be newk->val.
// Returns 0 on success, -E_FILE_EXISTS if newk exists, or -E_NOT_FOUND if oldk does not exist.
int    hash_map_change_key(hash_map_t * hm, void * oldk, void * newk);
// Remove all key-val pairs, does not destroy keys or vals.
void   hash_map_clear(hash_map_t * hm);
// Return the val associated with k.
void * hash_map_find_val(const hash_map_t * hm, const void * k);
// Return the key and val associated with k.
hash_map_elt_t hash_map_find_elt(const hash_map_t * hm, const void * k);

// Return the number of buckets currently allocated.
size_t hash_map_bucket_count(const hash_map_t * hm);
// Resize the number of buckets to n.
// Returns 0 on success, 1 on no resize needed, or -E_NO_MEM.
int    hash_map_resize(hash_map_t * hm, size_t n);


// Iteration

struct hash_map_it;
typedef struct hash_map_it hash_map_it_t;

hash_map_it_t * hash_map_it_create();
void hash_map_it_destroy(hash_map_it_t * it);
// Iterate through the hash map values using hm_it.
// - Returns NULL when the end of the hash map is reached.
// - Behavior is undefined begin iterating, modify hm, and then continue
//   iterating using the old hm_it.
void * hash_map_val_next(hash_map_t * hm, hash_map_it_t * hm_it);

#endif /* !KUDOS_INC_HASH_MAP_H */
