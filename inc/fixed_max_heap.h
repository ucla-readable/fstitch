#ifndef FIXED_MAX_HEAP_H
#define FIXED_MAX_HEAP_H

struct fixed_max_heap {
	void **arr;
	int *weights;
	int len;
	int max;
};

typedef struct fixed_max_heap fixed_max_heap_t;

fixed_max_heap_t *fixed_max_heap_create(int len);
void  fixed_max_heap_free(fixed_max_heap_t *heap);

void  fixed_max_heap_insert(fixed_max_heap_t *heap, void *elt, int weight);
void *fixed_max_heap_pop(fixed_max_heap_t *heap);
void  fixed_max_heap_delete(fixed_max_heap_t *heap, void *elt);
int   fixed_max_heap_length(const fixed_max_heap_t *heap);
int   fixed_max_heap_contains(fixed_max_heap_t *heap, void *elt);

#endif // FIXE_MAX_HEAP_H
