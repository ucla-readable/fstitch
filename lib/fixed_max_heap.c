#include <inc/fixed_max_heap.h>
#include <inc/malloc.h>

static int parent(int child) {
	return ((child+1)/2)-1;
}

static int child(int parent) {
	return ((parent+1)*2)-1;;
}

static void reheapify_up_elt(fixed_max_heap_t *heap, int p)
{
	if (p == 0) return;
	if (heap->len < 2) return;
	if (heap->weights[parent(p)] < heap->weights[p]) {
		// swap
		void *temp;
		int itemp;
		temp = heap->arr[parent(p)];
		heap->arr[parent(p)] = heap->arr[p];
		heap->arr[p] = temp;

		itemp = heap->weights[parent(p)];
		heap->weights[parent(p)] = heap->weights[p];
		heap->weights[p] = itemp;
		
		if (p >= 2)
			reheapify_up_elt(heap, parent(p));
	}
}

// this fxn assumes only the final elt may be in the wrong place
static void reheapify_up(fixed_max_heap_t *heap)
{
	if (heap->len < 2) return;
	reheapify_up_elt(heap, heap->len);
}

static void reheapify_down_elt(fixed_max_heap_t *heap, int p)
{
	int max;
	int swap = p;
	void *temp;
	int itemp;
	// do we have children?
	if (child(p) >= heap->len) return;
	if (heap->weights[p] < heap->weights[child(p)]) {
		max = heap->weights[child(p)];
		swap = child(p);
	}
	if (!(child(p)+1 >= heap->len)) // if we have a right child...
		if (heap->weights[swap] < heap->weights[child(p)+1]) {
			max = heap->weights[child(p)+1];
			swap = child(p)+1;
		}
	if (swap == p) return;
	itemp = heap->weights[swap];
	heap->weights[swap] = heap->weights[p];
	heap->weights[p] = itemp;
	temp = heap->arr[swap];
	heap->arr[swap] = heap->arr[p];
	heap->arr[p] = temp;
	reheapify_down_elt(heap, swap);
}

static void reheapify_down(fixed_max_heap_t *heap)
{
	if (heap->len < 2) return;
	return reheapify_down_elt(heap, 0);
}

fixed_max_heap_t *
fixed_max_heap_create(int len)
{
	fixed_max_heap_t *ret;
	ret = (fixed_max_heap_t*)malloc(sizeof(fixed_max_heap_t));
	if (ret == NULL)
		return NULL;
	ret->arr = (void*)malloc(sizeof(void*) * len);
	if (ret->arr == NULL) {
		free(ret);
		return NULL;
	}
	ret->weights = (int*)malloc(sizeof(int) * len);
	if (ret->weights == NULL) {
		free(ret->arr);
		free(ret);
		return NULL;
	}
	ret->len = 0;
	ret->max = len;
	return ret;
}

void
fixed_max_heap_free(fixed_max_heap_t *heap)
{
	free(heap->arr);
	free(heap->weights);
	free(heap);
}

void
fixed_max_heap_insert(fixed_max_heap_t *heap, void *elt, int weight)
{
	assert(heap->len < heap->max);
	heap->arr[heap->len] = elt;
	heap->weights[heap->len] = weight;
	heap->len++;
	reheapify_up(heap);
}

void *
fixed_max_heap_pop(fixed_max_heap_t *heap)
{
	void *ret;
	assert(heap->len >= 1);
	ret = heap->arr[0];
	heap->arr[0] = heap->arr[heap->len - 1];
	heap->weights[0] = heap->weights[heap->len - 1];
	heap->len--;
	reheapify_down(heap);
	return ret;
}

void
fixed_max_heap_delete(fixed_max_heap_t *heap, void *elt)
{
	// find element XXX take advantage of the fact that is a heap for
	// the search
	int i;
	bool found = 0;
	assert(heap->len > 0);
	for (i = 0; i < heap->len; i++) {
		if (heap->arr[i] == elt) {
			found = 1;
			break;
		}
	}
	assert(found);
	heap->weights[i] = 0x0fffffff;
	reheapify_up_elt(heap, i);
	// move last elt to top
	heap->arr[0] = heap->arr[heap->len - 1];
	heap->weights[0] = heap->weights[heap->len - 1];
	heap->len--;
	reheapify_down(heap);
}

// returns 1 if found. 0 if not found.
int
fixed_max_heap_contains(const fixed_max_heap_t *heap, const void *elt)
{
	// find element XXX take advantage of the fact that is a heap for
	// the search
	int i;
	for (i = 0; i < heap->len; i++)
		if (heap->arr[i] == elt)
			return 1;
	return 0;
}

int
fixed_max_heap_length(const fixed_max_heap_t *heap)
{
	return heap->len;
}
