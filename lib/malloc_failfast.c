#include <inc/lib.h>
#include <inc/malloc.h>

#ifdef USE_FAILFAST_MALLOC /* [ */

/* Use a different start address than the default malloc... */
static size_t next_addr = 0x20000000;

void * malloc(size_t size)
{
	void * memory = (void *) next_addr;
	
	if(next_addr >= 0x80000000)
		panic("out of address space");
	
	size = ROUNDUP32(size, PGSIZE);
	while(size > 0)
	{
		int r = sys_page_alloc(0, (void *) next_addr, PTE_P | PTE_U | PTE_W);
		assert(r >= 0);
		next_addr += PGSIZE;
		size -= PGSIZE;
	}
	
	next_addr += PGSIZE;
	
	return memory;
}

void * calloc(size_t count, size_t size)
{
	return malloc(count * size);
}

static bool va_is_mapped(const void * va)
{
	return (vpd[PDX(va)] & PTE_P) && (vpt[VPN(va)] & PTE_P);
}

void free(void * ptr)
{
	while(va_is_mapped(ptr))
	{
		int r = sys_page_unmap(0, ptr);
		assert(r >= 0);
		ptr += PGSIZE;
	}
}

void malloc_stats(void)
{
	printf("(no malloc stats available)\n");
}

#endif /* ] USE_FAILFAST_MALLOC */
