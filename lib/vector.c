#include <inc/malloc.h>
#include <inc/error.h>
#include <inc/vector.h>


static void ** vector_create_elts(size_t n);
static void    vector_destroy_elts(vector_t * v);
static int     vector_grow(vector_t * v);

# define INIT_CAPACITY 10


//
// Construction/destruction

vector_t * vector_create(void)
{
	// Create a vector with no elements, but with a capacity.

	vector_t * v = vector_create_size(INIT_CAPACITY);
	if (!v)
		return NULL;

	v->size = 0;
	return v;
}

vector_t * vector_create_size(size_t n)
{
	vector_t * v = malloc(sizeof(*v));
	if (!v)
		return NULL;

	v->size = n;
	v->elts = vector_create_elts(n);
	if (!v->elts)
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

static void ** vector_create_elts(size_t n)
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

int vector_push_back(vector_t * v, void * elt)
{
	int r;
	if (v->size == v->capacity)
	{
		if ((r = vector_grow(v)) < 0)
			return r;
	}

	v->elts[v->size++] = elt;
	return 0;
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

int vector_reserve(vector_t * v, size_t n)
{
	size_t i;
	const size_t n_elts = v->size;

	if (n <= v->capacity)
		return 1;

	void ** elts = vector_create_elts(n);
	if (!elts)
		return -E_NO_MEM;

	for (i=0; i < n_elts; i++)
		elts[i] = v->elts[i];

	vector_destroy_elts(v);
	v->elts = elts;
	v->size = n_elts;
	v->capacity = n;

	return 0;
}

static int vector_grow(vector_t * v)
{
	return vector_reserve(v, 2*v->capacity);
}
