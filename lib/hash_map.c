#include <inc/error.h>
#include <lib/stdlib.h>
#include <lib/vector.h>
#include <lib/hash_map.h>

#define HASH_MAP_DEBUG 0

#if HASH_MAP_DEBUG
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
static chain_elt_t * chain_search_key(const chain_elt_t * head, const void * k);


struct hash_map {
	size_t size;
	bool auto_resize;
	vector_t * tbl;
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
	for (pos = first; *(pos + 1) < n && pos != last; pos++) ;
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
// Construction/destruction

hash_map_t * hash_map_create(void)
{
	return hash_map_create_size(1, 1);
}

hash_map_t * hash_map_create_size(size_t n, bool auto_resize)
{
	if (!n)
		return NULL;

	hash_map_t * hm = malloc(sizeof(*hm));
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
	Dprintf("%s(0x%08x, 0x%08x, 0x%08x)\n", __FUNCTION__, hm, k, v);
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);
	size_t ns;

	if (!head)
	{
		head = chain_elt_create();
		if (!head)
			return -E_NO_MEM;
	}
	else
	{
		// See if k is already in the chain, simply update its value if so.
		chain_elt_t * existing_elt;
		if ((existing_elt = chain_search_key(head, k)))
		{
			existing_elt->elt.val = v;
			return 1;
		}

		// k isn't already in the chain, add it.

		chain_elt_t * new_head = chain_elt_create();
		if (!new_head)
			return -E_NO_MEM;

		new_head->next = head;
		head->prev = new_head;
		head = new_head;
	}

	vector_elt_set(hm->tbl, elt_num, head);

	head->elt.key = k;
	head->elt.val = v;
	hm->size++;

	if (hm->auto_resize && (ns = next_size(hash_map_bucket_count(hm))) > hash_map_bucket_count(hm))
	{
		// (safe to ignore failure)
		(void) hash_map_resize(hm, ns);
	}

	return 0;
}

void * hash_map_erase(hash_map_t * hm, const void * k)
{
	Dprintf("%s(0x%08x, 0x%08x)\n", __FUNCTION__, hm, k);
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);
	size_t ns;
	void * v;

	if (!head)
		return NULL;

	chain_elt_t * k_chain = chain_search_key(head, k);
	if (!k_chain)
		return NULL;

	if (k_chain->prev)
		k_chain->prev->next = k_chain->next;
	else
		vector_elt_set(hm->tbl, elt_num, k_chain->next);
	if (k_chain->next)
		k_chain->next->prev = k_chain->prev;

	v = k_chain->elt.val;

	chain_elt_destroy(k_chain);
	hm->size--;

	
	if (hm->auto_resize && (ns = next_size(hash_map_bucket_count(hm))) < hash_map_bucket_count(hm))
	{
		// (safe to ignore failure)
		(void) hash_map_resize(hm, ns);
	}

	return v;
}

int hash_map_change_key(hash_map_t * hm, void * oldk, void * newk)
{
	Dprintf("%s(0x%08x, 0x%08x, 0x%08x)\n", __FUNCTION__, hm, oldk, newk);
	chain_elt_t * head;
	chain_elt_t * elt;

	// Check that newk isn't already in use

	const size_t newk_elt_num = hash_ptr(newk, vector_size(hm->tbl));
	head = vector_elt(hm->tbl, newk_elt_num);
	if (head && chain_search_key(head, newk))
		return -E_FILE_EXISTS;

	// Find oldk

	const size_t oldk_elt_num = hash_ptr(oldk, vector_size(hm->tbl));
	head = vector_elt(hm->tbl, oldk_elt_num);
	if (!head)
		return -E_NOT_FOUND;

	head = chain_search_key(head, oldk);
	if (!head)
		return -E_NOT_FOUND;

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

	return 0;
}

void hash_map_clear(hash_map_t * hm)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, hm);
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
}

void * hash_map_find_val(const hash_map_t * hm, const void * k)
{
	hash_map_elt_t hme = hash_map_find_elt(hm, k);
	return hme.val;
}

hash_map_elt_t hash_map_find_elt(const hash_map_t * hm, const void * k)
{
	hash_map_elt_t hme;
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);

	hme.key = NULL;
	hme.val = NULL;

	if (!head)
		return hme;

	chain_elt_t * k_chain = chain_search_key(head, k);
	if (!k_chain)
		return hme;

	hme = k_chain->elt;

	return hme;
}


//
// Resizing

size_t hash_map_bucket_count(const hash_map_t * hm)
{
	return vector_size(hm->tbl);
}

int hash_map_resize(hash_map_t * hm, size_t n)
{
	int r;

	n = next_size(n);

	// Avoid unnecessary work when there is no change in the number of buckets
	// and avoid making the hash table smaller than this implementation desires
	if (n == hash_map_bucket_count(hm))
		return 1;

	// Possible speedup if we could use one:
	// http://sources.redhat.com/ml/guile/1998-10/msg00864.html

	// Create new hash table
	hash_map_t * new_hm = hash_map_create_size(n, hm->auto_resize);
	if (!new_hm)
		return -E_NO_MEM;

	// Rehash elements
	size_t i;
	chain_elt_t * elt;
	for (i=0; i < vector_size(hm->tbl); i++)
	{
		elt = vector_elt(hm->tbl, i);
		while (elt)
		{
			if ((r = hash_map_insert(new_hm, elt->elt.key, elt->elt.val)) < 0)
			{
				hash_map_destroy(new_hm);
				return r;
			}
			elt = elt->next;
		}
	}

	// Expire the old hash table and move in the new
	hash_map_clear(hm);
	hm->size = new_hm->size;
	hm->tbl  = new_hm->tbl;
	free(new_hm);

	return 0;
}



//
// Iteration

void hash_map_it_init(hash_map_it_t * it, hash_map_t * hm)
{
	it->hm = hm;
	it->bucket = 0;
	it->elt = NULL;
}

void * hash_map_val_next(hash_map_it_t * it)
{
	size_t i;
	chain_elt_t * head;

	if (!it->bucket && !it->elt)
	{
		// New iterator

		if (!it->hm)
			return NULL;

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
			return NULL; // no elts in the hash map
		return it->elt->elt.val;
	}

	// If there are more elts in this chain, return the next
	if (it->elt->next)
	{
		it->elt = it->elt->next;
		return it->elt->elt.val;
	}

	// Find the next bucket with an elt
	for (i=it->bucket+1; i < vector_size(it->hm->tbl); i++)
	{
		head = vector_elt(it->hm->tbl, i);
		if (head)
		{
			it->bucket = i;
			it->elt = head;
			return it->elt->elt.val;
		}
	}

	return NULL;
}



//
// Chains

static chain_elt_t * chain_elt_create(void)
{
	chain_elt_t * elt = malloc(sizeof(*elt));
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
	free(elt);
}

static chain_elt_t * chain_search_key(const chain_elt_t * head, const void * k)
{
	while (head)
	{
		if (head->elt.key == k)
			return (chain_elt_t *) head;
		head = head->next;
	}

	return NULL;
}
