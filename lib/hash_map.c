#include <inc/malloc.h>
#include <inc/vector.h>
#include <inc/hash_map.h>

//
// Implement hash_map.h using a chaining hash table.

struct chain_elt {
	hash_map_elt_t elt;
	struct chain_elt * next;
	struct chain_elt * prev;
};
typedef struct chain_elt chain_elt_t;

static chain_elt_t * chain_elt_create();
static void          chain_elt_destroy(chain_elt_t * elt);
static chain_elt_t * chain_search_key(const chain_elt_t * head, const void * k);


struct hash_map {
	size_t size;
	vector_t * tbl;
};

#define INIT_NUM_ELTS 16

static size_t hash_ptr(const void * k, size_t tbl_size);

#include <inc/assert.h>

// A rotating hash, http://burtleburtle.net/bob/hash/doobs.html.
// NOTE: rotating hashes don't hash well, this fn could be improved.
static size_t hash_ptr(const void * k, size_t tbl_size)
{
	const uint8_t const * key = (uint8_t*) k;
	size_t hash = sizeof(k);
	size_t i;

	for (i=0; i < sizeof(k); i++)
		hash = (hash<<4) ^ (hash>>28) ^ key[i];

	return (hash % tbl_size);
}

// Instead of the rotating hash above we could use the multiplcation method
// (CLR, 11.3.2). Implement math.c using assembly, then try this out.
#if 0
static const double hash_ptr_A = 0.6180339887; //sqrt(5) - 1;

static size_t hash_ptr(const void * k, size_t tbl_size)
{	
	const uint32_t key = (uint32_t) k;
	return (size_t) floor(key * modf(key * hash_ptr_A, NULL));
}
#endif


//
// Construction/destruction

hash_map_t * hash_map_create()
{
	return hash_map_create_size(INIT_NUM_ELTS);
}

hash_map_t * hash_map_create_size(size_t n)
{
	hash_map_t * hm = malloc(sizeof(*hm));
	if (!hm)
		return NULL;

	hm->size = 0;
	if (! (hm->tbl = vector_create_size(n)) )
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

bool hash_map_insert(hash_map_t * hm, void * k, void * v)
{
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);

	if (!head)
	{
		if (! (head = chain_elt_create()) )
			return 0;
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
			return 0;

		new_head->next = head;
		head->prev = new_head;
		head = new_head;
	}

	vector_elt_set(hm->tbl, elt_num, head);

	head->elt.key = k;
	head->elt.val = v;
	hm->size++;

	return 1;
}

bool hash_map_erase(hash_map_t * hm, const void * k)
{
	const size_t elt_num = hash_ptr(k, vector_size(hm->tbl));
	assert(elt_num < vector_size(hm->tbl));
	chain_elt_t * head = vector_elt(hm->tbl, elt_num);

	if (!head)
		return 0;

	chain_elt_t * k_chain = chain_search_key(head, k);
	if (!k_chain)
		return 0;

	if (k_chain->prev)
		k_chain->prev->next = k_chain->next;
	if (k_chain->next)
		k_chain->next->prev = k_chain->prev;

	chain_elt_destroy(k_chain);
	hm->size--;

	return 1;
}

void hash_map_clear(hash_map_t * hm)
{
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


//
// Element access

// Implement if useful
/*
hash_map_elt_t hash_map_elt_begin(hash_map_t * hm);
hash_map_elt_t hash_map_elt_end(hash_map_t * hm);
hash_map_elt_t hash_map_elt_next(hash_map_t * hm, hash_map_elt_t elt);
*/


//
// Chains

static chain_elt_t * chain_elt_create()
{
	chain_elt_t * c = malloc(sizeof(*c));
	c->elt.key = NULL;
	c->elt.val = NULL;
	c->next = NULL;
	c->prev = NULL;
	return c;
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
