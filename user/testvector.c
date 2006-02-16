#include <inc/lib.h>
#include <inc/vector.h>

void umain(void)
{
	int r;
	vector_t * v = vector_create();
	if (!v)
		printf("vector_create() = NULL\n");

	int a = 5;

	if ((r = vector_push_back(v, &a)) < 0)
		printf("vector_push_back() FAILED: %i\n", r);

	int * pa = vector_elt(v, 0);
	if (pa != &a)
		printf("vector_elt() FAILED (%x)\n", pa);
}
