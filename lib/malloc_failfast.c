#include <inc/lib.h>
#include <stdlib.h>

#ifdef USE_FAILFAST_MALLOC /* [ */

#define FAILFAST_ALIGN_END 0

/* Use a different start address than the default malloc... */
static size_t next_addr = 0x20000000;
static size_t used_memory = 0;

void * malloc(size_t size)
{
	void * memory = (void *) next_addr;
#if FAILFAST_ALIGN_END
	size_t offset = PGSIZE - (size % PGSIZE);
	if(offset == PGSIZE)
		offset = 0;
#endif
	
	if(next_addr >= 0x80000000)
		panic("out of address space");
	
	size = ROUNDUP32(size, PGSIZE);
	while(size > 0)
	{
		int r = sys_page_alloc(0, (void *) next_addr, PTE_P | PTE_U | PTE_W);
		assert(r >= 0);
		next_addr += PGSIZE;
		size -= PGSIZE;
		used_memory += PGSIZE;
	}
	
	next_addr += PGSIZE;
	
#if FAILFAST_ALIGN_END
	return memory + offset;
#else
	return memory;
#endif
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
#if FAILFAST_ALIGN_END
	ptr = (void *) ROUNDDOWN32(ptr, PGSIZE);
#endif
	if(!va_is_mapped(ptr))
		printf("[%08x] (%s) BUG: double free(0x%08x)\n", env->env_id, env->env_name, ptr);
	while(va_is_mapped(ptr))
	{
		int r = sys_page_unmap(0, ptr);
		assert(r >= 0);
		ptr += PGSIZE;
		used_memory -= PGSIZE;
	}
}

void malloc_stats(void)
{
	printf("used failfast malloc memory = %d\n", used_memory);
}

#endif /* ] USE_FAILFAST_MALLOC */
