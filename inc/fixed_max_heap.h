#ifndef FIXED_HEAP_H
#define FIXED_HEAP_H

struct fixed_heap {
	void **arr;
	int *weights;
	int len;
	int max;
};

typedef struct fixed_heap fixed_heap_t;

fixed_heap_t *fixed_heap_create(int len);
void  fixed_heap_free(fixed_heap_t *heap);

void  fixed_heap_insert(fixed_heap_t *heap, void *elt, int weight);
void *fixed_heap_pop(fixed_heap_t *heap);
void  fixed_heap_delete(fixed_heap_t *heap, void *elt);
int   fixed_heap_length(const fixed_heap_t *heap);

#endif // FIXE_HEAP_H
