#ifndef KUDOS_INC_VECTOR_H
#define KUDOS_INC_VECTOR_H

#include <lib/types.h>
#include <lib/hash_set.h> // for vector_create_hashset()

struct vector {
	size_t size;
	size_t capacity;
	void ** elts;
};
typedef struct vector vector_t;

// Create a vector.
vector_t * vector_create(void);
// Create a vector of size n.
vector_t * vector_create_size(size_t n);
// Create a vector with the same elements as hs.
vector_t * vector_create_hashset(const hash_set_t * hs);
// Destroy the vector, does not destroy elts.
void       vector_destroy(vector_t * v);

// Returns number of elts in the vector.
static __inline
size_t vector_size(const vector_t * v) __attribute__((always_inline));
// Returns whether the vector is empty.
static __inline
bool   vector_empty(const vector_t * v) __attribute__((always_inline));
// Push elt onto the back of the vector, growing if necessary.
// Returns 0 on success, or -E_NO_MEM.
int    vector_push_back(vector_t * v, void * elt);
// Push vector onto the back, growing if necessary.
// Returns 0 on success, or -E_NO_MEM.
int    vector_push_back_vector(vector_t * v, const vector_t * v2);
// Remove the last elt in the vector, does not destroy elt.
static __inline
void   vector_pop_back(vector_t * v) __attribute__((always_inline));
// Remove the given elt at position i, does not destroy elt.
void   vector_erase(vector_t * v, size_t i);
// Remove all elts in the vector, does not destroy elts.
void   vector_clear(vector_t * v);

// Sort the vector in ascending order. compar should return a value
// less than, equal to, or greater than zero if 'a' is less than,
// equal to, or greater than 'b', respectively.
void   vector_sort(vector_t *v, int (*compar)(const void *a, const void *b));

// Return the elt at position i.
static __inline
void * vector_elt(const vector_t * v, size_t i) __attribute__((always_inline));
// Set the elt at position i.
static __inline
void   vector_elt_set(vector_t * v, size_t i, void * elt) __attribute__((always_inline));
// Return the first elt.
static __inline
void * vector_elt_front(vector_t * v) __attribute__((always_inline));
// Return the last elt.
static __inline
void * vector_elt_end(vector_t * v) __attribute__((always_inline));

// Return the current capacity of the vector.
size_t vector_capacity(const vector_t * v);
// Ensure room for n elts is reserved in the vector.
// Returns 0 on success, or -E_NO_MEM.
int    vector_reserve(vector_t * v, size_t n);


//
// Implementations of inline functions

static __inline
size_t vector_size(const vector_t * v)
{
	return v->size;
}

static __inline
bool vector_empty(const vector_t * v)
{
	return (v->size == 0);
}

static __inline
void vector_pop_back(vector_t * v)
{
	if (v->size == 0)
		return;
	v->size--;
}

static __inline
void * vector_elt(const vector_t * v, size_t i)
{
	return v->elts[i];
}

static __inline
void vector_elt_set(vector_t * v, size_t i, void * elt)
{
	v->elts[i] = elt;
}

static __inline
void * vector_elt_front(vector_t * v)
{
	return v->elts[0];
}

static __inline
void * vector_elt_end(vector_t * v)
{
	return v->elts[v->size - 1];
}

#endif /* !KUDOS_INC_VECTOR_H */
