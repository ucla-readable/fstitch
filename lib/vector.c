#include <inc/malloc.h>
#include <inc/vector.h>


static void ** vector_create_elts(size_t n);
static void    vector_destroy_elts(vector_t * v);
static bool    vector_grow(vector_t * v);

#define INIT_NUM_ELTS 10


//
// Construction/destruction

vector_t * vector_create()
{
	return vector_create_size(INIT_NUM_ELTS);
}

vector_t * vector_create_size(size_t n)
{
	vector_t * v = malloc(sizeof(*v));
	if (!v)
		return NULL;

	v->size = n;
	if (! (v->elts = vector_create_elts(n)) )
	{
		free(v);
		return NULL;
	}
	v->capacity = n;

	return v;
}

void vector_destroy(vector_t * v)
{
	vector_destroy_elts(v);
	free(v);
}

static void **  vector_create_elts(size_t n)
{
	void ** elts = malloc(n*sizeof(*elts));
	return elts;
}

static void vector_destroy_elts(vector_t * v)
{
	free(v->elts);
	v->elts = NULL;
	v->size = 0;
	v->capacity = 0;
}


//
// General

// vector_size() inlined

// vector_empty() inlined

bool vector_push_back(vector_t * v, void * elt)
{
	if (v->size != v->capacity)
	{
		v->elts[v->size++] = elt;
		return 1;
	}
	else
	{
		if (!vector_grow(v))
			return 0;
		v->elts[v->size++] = elt;
		return 1;
	}
}

void vector_pop_back(vector_t * v)
{
	if (v->size == 0)
		return;
	v->size--;
}

void vector_erase(vector_t * v, size_t i)
{
	for (; i+1 < v->size; i++)
		v->elts[i] = v->elts[i+1];
	v->size--;
}

void vector_clear(vector_t * v)
{
	v->size = 0;
}


//
// Element access

// vector_elt() inlined

// vector_elt_front() inlined

// vector_elt_end() inlined


//
// Growing/shrinking

size_t vector_capacity(const vector_t * v)
{
	return v->capacity;
}

bool vector_reserve(vector_t * v, size_t n)
{
	size_t i;
	const size_t n_elts = v->size;

	if (n <= v->capacity)
		return 1;

	void ** elts = vector_create_elts(n);
	if (!elts)
		return 0;

	for (i=0; i < n_elts; i++)
		elts[i] = v->elts[i];

	vector_destroy_elts(v);
	v->elts = elts;
	v->size = n_elts;
	v->capacity = n;

	return 1;
}

static bool vector_grow(vector_t * v)
{
	return vector_reserve(v, 2*v->capacity);
}
