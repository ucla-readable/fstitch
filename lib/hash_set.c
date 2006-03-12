#if !defined(__KERNEL__)
#include <assert.h>
#else
#warning assert not yet implemented
#define assert(x) do { } while(0)
#endif
#include <lib/stdlib.h>
#include <lib/hash_map.h>
#include <lib/hash_set.h>

//
// Implement hash_set.h using hash_map (to quickly implement this).


struct hash_set {
	hash_map_t * hm;
};


//
// Construction/destruction

hash_set_t * hash_set_create(void)
{
	hash_set_t * hs = malloc(sizeof(*hs));
	if (!hs)
		return NULL;

	hs->hm = hash_map_create();
	if (!hs->hm)
	{
		free(hs);
		return NULL;
	}

	return hs;
}

hash_set_t * hash_set_create_size(size_t n, bool auto_resize)
{
	hash_set_t * hs = malloc(sizeof(*hs));
	if (!hs)
		return NULL;

	hs->hm = hash_map_create_size(n, auto_resize);
	if (!hs->hm)
	{
		free(hs);
		return NULL;
	}

	return hs;
}

void hash_set_destroy(hash_set_t * hs)
{
	hash_set_clear(hs);
	hash_map_destroy(hs->hm);
	hs->hm = NULL;
	free(hs);
}


//
// General

size_t hash_set_size(const hash_set_t * hs)
{
	return hash_map_size(hs->hm);
}

bool hash_set_empty(const hash_set_t * hs)
{
	return hash_map_empty(hs->hm);
}

int hash_set_insert(hash_set_t * hs, void * elt)
{
	return hash_map_insert(hs->hm, elt, elt);
}

void * hash_set_erase(hash_set_t * hs, const void * elt)
{
	return hash_map_erase(hs->hm, elt);
}

void hash_set_clear(hash_set_t * hs)
{
	hash_map_clear(hs->hm);
}

bool hash_set_exists(const hash_set_t * hs, const void * elt)
{
	return (elt == hash_map_find_val(hs->hm, elt));
}


//
// Resizing

size_t hash_set_bucket_count(const hash_set_t * hs)
{
	return hash_map_bucket_count(hs->hm);
}

int hash_set_resize(hash_set_t * hs, size_t n)
{
	return hash_map_resize(hs->hm, n);
}



//
// Iteration

void hash_set_it_init(hash_set_it_t * it, hash_set_t * hs)
{
	hash_map_it_init(it, hs->hm);
}

void * hash_set_next(hash_set_it_t * it)
{
	return hash_map_val_next(it);
}
