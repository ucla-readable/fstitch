#include <inc/lib.h>
#include <inc/fixed_heap.h>

int  num[8] = {20, 9, 10, 17, 11, 8, 7, 30};
int snum[8] = {30, 20, 11, 10, 9, 8, 7};

void
umain(int argc, char *argv[])
{
	fixed_heap_t *h;
	int i;
	h = fixed_heap_create(8);
	assert(fixed_heap_length(h) == 0);
	for (i = 0; i < 8; i++) {
		fixed_heap_insert(h, num[i]+100, num[i]);
		assert(fixed_heap_length(h) == (i+1));
	}
	fixed_heap_delete(h, 100+17);
	assert(fixed_heap_length(h) == 7);
	for (i = 0; i < 7; i++) {
		int ret = fixed_heap_pop(h);
		printf("%d\n", ret);
		assert(ret == (snum[i]+100));
		assert(fixed_heap_length(h) == (7-i-1));
	}
	printf("success!\n");
}
