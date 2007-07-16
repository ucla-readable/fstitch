#include <lib/platform.h>
#include <lib/vector.h>
#include <lib/hash_map.h>
#include <lib/pool.h>

#include <kfs/kfsd.h>

#define HASH_MAP_DEBUG 0

#if HASH_MAP_DEBUG
#include <lib/stdio.h>
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


//
// Implement hash_map.h using a chaining hash table.

// Since we are storing only a pointer in each entry it might make more
// sense to use open addressing with the same amount of memory used than
// chaining, since each chain entry needs two ptrs for the chain and each
// bucket uses one pointer to point to the chain. TAOCP page 545 lightly
// discusses this.


struct chain_elt {
	hash_map_elt_t elt;
	struct chain_elt * next;
	struct chain_elt * prev;
};

static chain_elt_t * chain_elt_create(void);
static void          chain_elt_destroy(chain_elt_t * elt);
static __inline chain_elt_t * chain_search_key(const chain_elt_t * head, const void * k) __attribute__((always_inline));


struct hash_map {
	size_t size;
	bool auto_resize;
	vector_t * tbl;
#if HASH_MAP_IT_MOD_DEBUG
	size_t version; // Incremented for every change
	size_t loose_version; // Incremented for inserts and resizes (not removes)
#endif
};


//
// The hashing function.
// For now only one hashing function is needed; if hash_map's usage grows
// beyond that of pointers hash_map should be enhanced to allow other hash
// functions.
// Essentially, GNU C++ STL 3.4's hash_fun and hashtable.

// Note: assumes long is at least 32 bits.
enum { num_primes = 28 };

static const unsigned long prime_list[num_primes] =
{
  53ul,         97ul,         193ul,       389ul,       769ul,
  1543ul,       3079ul,       6151ul,      12289ul,     24593ul,
  49157ul,      98317ul,      196613ul,    393241ul,    786433ul,
  1572869ul,    3145739ul,    6291469ul,   12582917ul,  25165843ul,
  50331653ul,   100663319ul,  201326611ul, 402653189ul, 805306457ul,
  1610612741ul, 3221225473ul, 4294967291ul
};

static inline unsigned long next_size(size_t n)
{
	const unsigned long * first = prime_list;
	const unsigned long * last = prime_list + (int) num_primes;
	const unsigned long * pos = first;
	for (pos = first; *pos < n && pos != last; pos++) ;
	return pos == last ? *(last - 1) : *pos;
}

inline static size_t hash_ptr(const void * k, size_t tbl_size)
{
	return ((size_t) k) % tbl_size;
}

// Not yet in use, but here in case we later want it
inline static size_t hash_str(const char * s, size_t tbl_size)
{
    unsigned long h = 0;
    for ( ; *s; ++s)
		h = 5*h + *s;
    return h % tbl_size;
}

//
// Inline implementations

static __inline chain_elt_t * chain_search_key(const chain_elt_t * head, const void * k)
{
	while (head)
	{
		if (head->elt.key == k)
			return (chain_elt_t *) head;
		head = head->next;
	}

	return NULL;
}

//
// Construction/destruction

hash_map_t * hash_map_create(void)
{
	return hash_map_create_size(1, 1);
}

hash_map_t * hash_map_create_size(size_t n, bool auto_resize)
{
	hash_map_t * hm;
	if (!n)
		return NULL;

	hm = malloc(sizeof(*hm));
	if (!hm)
		return NULL;

	hm->size = 0;
	hm->auto_resize = auto_resize;
	hm->tbl = vector_create_size(next_size(n));
	if (!hm->tbl)
	{
		free(hm);
		return NULL;
	}
#if HASH_MAP_IT_MOD_DEBUG
	hm->version = 0;
	hm->loose_version = 0;
#endif

	return hm;
}

hash_map_t * hash_map_copy(const hash_map_t * hm)
{
	hash_map_t * hm_copy;
	size_t i;
	chain_elt_t * elt;
	int r;

	// Create new hash table
	hm_copy = hash_map_create_size(hm->size, hm->auto_resize);
	if (!hm_copy)
		return NULL;

	// Copy elements (rehashing them; we could do this more quickly)
	for (i=0; i < vector_size(hm->tbl); i++)
	{
		elt = vector_elt(hm->tbl, i);
		while (elt)
		{
			if ((r = hash_map_insert(hm_copy, elt->elt.key, elt->elt.val)) < 0)
			{
				hash_map_destroy(hm_copy);
				return NULL;
			}
			elt = elt->next;
		}
	}

	return hm_copy;
}

void hash_map_destroy(hash_map_t * hm)
{
	hash_map_clear(hm);
	vector_destroy(hm->tbl);
	hm->tbl = NULL;
	free(hm);
}


//
// General

size_t hash_map_size(const hash_map_t * hm)
{
	return hm->size;
}

bool hash_map_empty(const hash_map_t * hm)
{
	return (hm->size == 0);
}

int hash_map_insert(hash_map_t * hm, void * k, void * v)
{
	Dprintf("%s(%p, %p, %p)\n", __FUNCTION__, hm, k, v);
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);

	if (!head)
	{
		head = chain_elt_create();
		if (!head)
			return -ENOMEM;
	}
	else
	{
		// See if k is already in the chain, simply update its value if so.
		chain_elt_t * existing_elt;
		chain_elt_t * new_head;
		if ((existing_elt = chain_search_key(head, k)))
		{
			existing_elt->elt.val = v;
#if HASH_MAP_IT_MOD_DEBUG
			hm->version++;
			hm->loose_version++;
#endif
			return 1;
		}

		// k isn't already in the chain, add it.

		new_head = chain_elt_create();
		if (!new_head)
			return -ENOMEM;

		new_head->next = head;
		head->prev = new_head;
		head = new_head;
	}

	vector_elt_set(hm->tbl, elt_num, head);
	hm->size++;
	head->elt.key = k;
	head->elt.val = v;
#if HASH_MAP_IT_MOD_DEBUG
	hm->version++;
	hm->loose_version++;
#endif

	if (hm->auto_resize && next_size(hash_map_size(hm)) > hash_map_bucket_count(hm))
	{
		// (safe to ignore failure)
		(void) hash_map_resize(hm, hash_map_size(hm));
	}

	return 0;
}

// Insert an elt into hm. elt must not already exist in hm.
// This allows movement of an elt from one hm to another;
// thus no malloc()/free() overhead and the elt maintains its memory location.
static void insert_chain_elt(hash_map_t * hm, chain_elt_t * elt)
{
	Dprintf("%s(%p, %p)\n", __FUNCTION__, hm, elt);
	const size_t elt_num = hash_ptr(elt->elt.key, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);

	if (head)
	{
		// Assume !chain_search_key(head, elt->elt.key)
		elt->next = head;
		head->prev = elt;
	}

	vector_elt_set(hm->tbl, elt_num, elt);
	hm->size++;
#if HASH_MAP_IT_MOD_DEBUG
	hm->version++;
	hm->loose_version++;
#endif
}

// Erase the key-value pair for k from hm, return the element.
static chain_elt_t * erase_chain_elt(hash_map_t * hm, const void * k)
{
	Dprintf("%s(%p, %p)\n", __FUNCTION__, hm, k);
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);
	chain_elt_t * k_chain;

	if (!head)
		return NULL;

	k_chain = chain_search_key(head, k);
	if (!k_chain)
		return NULL;

	if (k_chain->prev)
		k_chain->prev->next = k_chain->next;
	else
		vector_elt_set(hm->tbl, elt_num, k_chain->next);
	if (k_chain->next)
		k_chain->next->prev = k_chain->prev;

	k_chain->next = NULL;
	k_chain->prev = NULL;

	hm->size--;
#if HASH_MAP_IT_MOD_DEBUG
	hm->version++;
	/* do not update hm->loose_version */
#endif

	return k_chain;
}

void * hash_map_erase(hash_map_t * hm, const void * k)
{
	Dprintf("%s(%p, %p)\n", __FUNCTION__, hm, k);
	chain_elt_t * k_chain;
	void * v;

	k_chain = erase_chain_elt(hm, k);
	if (!k_chain)
		return NULL;

	v = k_chain->elt.val;
	chain_elt_destroy(k_chain);

#if 0
	// Auto-shrink support is untested; we might enable this later should
	// we find it may be helpful. This is not enabled because code that
	// calls hash_map_erase() on every element to destroy the map
	// would pay a time and max space penalty.
	size_t ns = next_size(hash_map_size(hm));
	if (hm->auto_resize && (next_size(ns + 1) < hash_map_bucket_count(hm)))
	{
		// (safe to ignore failure)
		(void) hash_map_resize(hm, ns);
	}
#endif

	return v;
}

int hash_map_change_key(hash_map_t * hm, void * oldk, void * newk)
{
	Dprintf("%s(%p, %p, %p)\n", __FUNCTION__, hm, oldk, newk);
	chain_elt_t * head;
	chain_elt_t * elt;

	// Check that newk isn't already in use

	const size_t newk_elt_num = hash_ptr(newk, vector_size(hm->tbl));
	head = vector_elt(hm->tbl, newk_elt_num);
	if (head && chain_search_key(head, newk))
		return -EEXIST;

	// Find oldk

	const size_t oldk_elt_num = hash_ptr(oldk, vector_size(hm->tbl));
	head = vector_elt(hm->tbl, oldk_elt_num);
	if (!head)
		return -ENOENT;

	head = chain_search_key(head, oldk);
	if (!head)
		return -ENOENT;

	// The hashmap has oldk, move elt to its new home

	elt = head;
	if (elt->prev)
		elt->prev->next = elt->next;
	else
		vector_elt_set(hm->tbl, oldk_elt_num, elt->next);
	if (elt->next)
		elt->next->prev = elt->prev;

	elt->elt.key = newk;
	elt->prev = NULL;
	elt->next = NULL;

	head = vector_elt(hm->tbl, newk_elt_num);
	if (head)
	{
		elt->next = head;
		head->prev = elt;
	}
	vector_elt_set(hm->tbl, newk_elt_num, elt);
#if HASH_MAP_IT_MOD_DEBUG
	hm->version++;
	hm->loose_version++;
#endif

	return 0;
}

void hash_map_clear(hash_map_t * hm)
{
	Dprintf("%s(%p)\n", __FUNCTION__, hm);
	size_t i;

	for (i=0; i < vector_size(hm->tbl); i++)
	{
		chain_elt_t * head = vector_elt(hm->tbl, i);
		chain_elt_t * next;
		while (head)
		{
			next = head->next;
			chain_elt_destroy(head);
			head = next;
		}
		vector_elt_set(hm->tbl, i, NULL);
	}

	hm->size = 0;
#if HASH_MAP_IT_MOD_DEBUG
	hm->version++;
	hm->loose_version++;
#endif
}

static __inline hash_map_elt_t * hash_map_find_internal(const hash_map_t * hm, const void * k) __attribute__((always_inline));
static __inline hash_map_elt_t * hash_map_find_internal(const hash_map_t * hm, const void * k)
{
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);
	chain_elt_t * k_chain;

	if (!head)
		return NULL;

	k_chain = chain_search_key(head, k);
	if (!k_chain)
		return NULL;

	return &k_chain->elt;
}

void * hash_map_find_val(const hash_map_t * hm, const void * k)
{
	hash_map_elt_t * hme = hash_map_find_internal(hm, k);
	if (!hme)
	{
		return NULL;
	}
	return hme->val;
}

hash_map_elt_t * hash_map_find_eltp(const hash_map_t * hm, const void * k)
{
	return hash_map_find_internal(hm, k);
}

hash_map_elt_t hash_map_find_elt(const hash_map_t * hm, const void * k)
{
	hash_map_elt_t * hme = hash_map_find_internal(hm, k);
	if (!hme)
	{
		hash_map_elt_t not_found = { .key = NULL, .val = NULL };
		return not_found;
	}
	return *hme;
}


//
// Resizing

size_t hash_map_bucket_count(const hash_map_t * hm)
{
	return vector_size(hm->tbl);
}

int hash_map_resize(hash_map_t * hm, size_t n)
{
	hash_map_t * new_hm;
	size_t i;
	n = next_size(n);

	// Avoid unnecessary work when there is no change in the number of buckets
	// and avoid making the hash table smaller than this implementation desires
	if (n == hash_map_bucket_count(hm))
		return 1;

	// Possible speedup if we could use one:
	// http://sources.redhat.com/ml/guile/1998-10/msg00864.html

	// Create new hash table
	new_hm = hash_map_create_size(n, hm->auto_resize);
	if (!new_hm)
		return -ENOMEM;

	// Rehash elements
	for (i=0; i < vector_size(hm->tbl); i++)
	{
		chain_elt_t * elt = vector_elt(hm->tbl, i);
		while (elt)
		{
			chain_elt_t * next_elt = elt->next;
			chain_elt_t * found = erase_chain_elt(hm, elt->elt.key);
			assert(found); // we are rehashing; elt.key is in the source map
			insert_chain_elt(new_hm, elt);
			elt = next_elt;
		}
	}

	// Expire the old hash table and move in the new
	hash_map_clear(hm);
	vector_destroy(hm->tbl);
	hm->size = new_hm->size;
	hm->tbl  = new_hm->tbl;
	free(new_hm);
#if HASH_MAP_IT_MOD_DEBUG
	hm->version++;
	hm->loose_version++;
#endif

	return 0;
}


//
// Iteration (current)

hash_map_it2_t hash_map_it2_create(hash_map_t * hm)
{
	hash_map_it2_t it;
	size_t i;

	it.key = NULL;
	it.val = NULL;
	it.internal.hm = hm;
	it.internal.next_bucket = 0;
	it.internal.next_elt = NULL;
#if HASH_MAP_IT_MOD_DEBUG
	it.internal.loose_version = hm->loose_version;
#endif

	if (!hm)
		return it;

	// Find the first entry and store it as next
	for (i = 0; i < vector_size(hm->tbl); i++)
	{
		chain_elt_t * head = vector_elt(hm->tbl, i);
		if (head)
		{
			it.internal.next_bucket = i;
			it.internal.next_elt = head;
			break;
		}
	}

	return it;
}

bool hash_map_it2_next(hash_map_it2_t * it)
{
	size_t i;

#if HASH_MAP_IT_MOD_DEBUG
	assert(!it->internal.hm || it->internal.loose_version == it->internal.hm->loose_version);
#endif

	if (!it->internal.next_elt)
		return 0;

	it->key = it->internal.next_elt->elt.key;
	it->val = it->internal.next_elt->elt.val;

	// If there are more elts in this chain, use the next elt
	if (it->internal.next_elt->next)
	{
		it->internal.next_elt = it->internal.next_elt->next;
		return 1;
	}

	// Find the next bucket with an elt
	for (i = it->internal.next_bucket + 1; i < vector_size(it->internal.hm->tbl); i++)
	{
		chain_elt_t * head = vector_elt(it->internal.hm->tbl, i);
		if (head)
		{
			it->internal.next_bucket = i;
			it->internal.next_elt = head;
			return 1;
		}
	}

	// The current entry is the last
	it->internal.next_elt = NULL;
	return 1;
}


//
// Iteration (deprecated)

void hash_map_it_init(hash_map_it_t * it, hash_map_t * hm)
{
	it->hm = hm;
	it->bucket = 0;
	it->elt = NULL;
#if HASH_MAP_IT_MOD_DEBUG
	it->version = hm->version;
#endif
}

hash_map_elt_t hash_map_elt_next(hash_map_it_t * it)
{
	hash_map_elt_t no_elt = { .key = NULL, .val = NULL };
	chain_elt_t * head;
	size_t i;

#if HASH_MAP_IT_MOD_DEBUG
	assert(it->version == it->hm->version);
#endif

	if (!it->bucket && !it->elt)
	{
		// New iterator

		if (!it->hm)
			return no_elt;

		// Set it to the first elt
		for (i=0; i < vector_size(it->hm->tbl); i++)
		{
			head = vector_elt(it->hm->tbl, i);
			if (head)
			{
				it->bucket = i;
				it->elt = head;
				break;
			}
		}

		if (!it->elt)
			return no_elt; // no elts in the hash map
		return it->elt->elt;
	}

	// If there are more elts in this chain, return the next
	if (it->elt->next)
	{
		it->elt = it->elt->next;
		return it->elt->elt;
	}

	// Find the next bucket with an elt
	for (i=it->bucket+1; i < vector_size(it->hm->tbl); i++)
	{
		head = vector_elt(it->hm->tbl, i);
		if (head)
		{
			it->bucket = i;
			it->elt = head;
			return it->elt->elt;
		}
	}

	return no_elt;
}

void * hash_map_val_next(hash_map_it_t * it)
{
	return hash_map_elt_next(it).val;
}



//
// Chains

DECLARE_POOL(chain_elt, chain_elt_t);

static void chain_elt_pool_free_all(void * ignore)
{
	chain_elt_free_all();
}

static chain_elt_t * chain_elt_create(void)
{
	chain_elt_t * elt = chain_elt_alloc();
	elt->elt.key = NULL;
	elt->elt.val = NULL;
	elt->next = NULL;
	elt->prev = NULL;
	return elt;
}

static void chain_elt_destroy(chain_elt_t * elt)
{
	elt->elt.key = NULL;
	elt->elt.val = NULL;
	elt->prev = NULL;
	elt->next = NULL;
	chain_elt_free(elt);
}

int hash_map_init(void)
{
	return kfsd_register_shutdown_module(chain_elt_pool_free_all, NULL, SHUTDOWN_POSTMODULES);
}
