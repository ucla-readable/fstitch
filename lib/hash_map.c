#include <assert.h>
#include <inc/error.h>
#include <inc/config.h>
#include <stdlib.h>
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

// No strong reason for 16, but do ensure this value should plays nicely
// with the hash function.
#define INIT_NUM_BUCKETS 16
#define MIN_NUM_BUCKETS  INIT_NUM_BUCKETS

// Expected time for successful and unsuccessful searches is Theta(1 + load),
// given chaining hash tables and a uniform hash, CLR theorems 11.1 and 11.2.
// 2.0 and 0.5 seem good, that is all.
#define AUTO_GROW_LOAD   2.0
#define AUTO_SHRINK_LOAD 0.5

static size_t hash_ptr(const void * k, size_t tbl_size);


//
// The hashing function.
// For now only one hashing function is needed, if hash_map's usage grows
// beyond that of pointers hash_map should be enhanced to allow other hash
// functions.

// A rotating hash, http://burtleburtle.net/bob/hash/doobs.html.
// NOTE: rotating hashes don't hash well, this could be improved.
static size_t hash_ptr(const void * k, size_t tbl_size)
{
	const uint8_t * const key = (uint8_t*) &k;
	size_t hash = sizeof(k);
	size_t i;

	for (i=0; i < sizeof(k); i++)
		hash = (hash<<4) ^ (hash>>28) ^ key[i];

	return (hash % tbl_size);
}

// Instead of the rotating hash above we could use the multiplication method
// (CLR, 11.3.2). Implement math.c using assembly, then try this out.
#if 0
// A is s/2^32, close to Knuth's suggested (sqrt(5)-1)/2.
static const double hash_ptr_A = (double)2654435769 / (double)0xffffffff; 

static size_t hash_ptr(const void * k, size_t tbl_size)
{	
	const uint32_t key = (uint32_t) k;
	return (size_t) floor(key * modf(key * hash_ptr_A, NULL));
}
#endif


//
// Construction/destruction

hash_map_t * hash_map_create(void)
{
	return hash_map_create_size(INIT_NUM_BUCKETS, 1);
}

hash_map_t * hash_map_create_size(size_t n, bool auto_resize)
{
	// hash_map uses floating point for auto rehashing
	if (auto_resize)
		assert(ENABLE_ENV_FP);

	if (!n)
		return NULL;

	hash_map_t * hm = malloc(sizeof(*hm));
	if (!hm)
		return NULL;

	hm->size = 0;
	hm->auto_resize = auto_resize;
	hm->tbl = vector_create_size(n);
	if (!hm->tbl)
	{
		free(hm);
		return NULL;
	}

	return hm;
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

	if (hm->auto_resize && AUTO_GROW_LOAD <= (double)hash_map_size(hm) / (double)hash_map_bucket_count(hm))
	{
		// (safe to ignore failure)
		(void) hash_map_resize(hm, 2*hash_map_bucket_count(hm));
	}

	return 0;
}

void * hash_map_erase(hash_map_t * hm, const void * k)
{
	Dprintf("%s(0x%08x, 0x%08x)\n", __FUNCTION__, hm, k);
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);
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

	if (hm->auto_resize && (double)hash_map_size(hm) / (double)hash_map_bucket_count(hm) <= AUTO_SHRINK_LOAD)
	{
		// (safe to ignore failure)
		(void) hash_map_resize(hm, hash_map_bucket_count(hm)/2);
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

	// Avoid unnecessary work when there is no change in the number of buckets
	// and avoid making the hash table smaller than this implementation desires
	if (n == hash_map_bucket_count(hm) || n <= MIN_NUM_BUCKETS)
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
