/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_KERN_PMAP_H
#define KUDOS_KERN_PMAP_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/pmap.h>
#include <inc/env.h>
#include <inc/assert.h>
#include <inc/error.h>


/* This macro takes a user supplied address and turns it into
 * something that will cause a fault if it is a kernel address.  ULIM
 * itself is guaranteed never to contain a valid page.  
 */
#define TRUP(_p)   						\
({								\
	register typeof((_p)) __m_p = (_p);			\
	(uintptr_t) __m_p > ULIM ? (typeof(_p)) ULIM : __m_p;	\
})

/* This macro takes a kernel virtual address -- an address that points above
 * KERNBASE, where the machine's maximum 256MB of physical memory is mapped --
 * and returns the corresponding physical address.  It panics if you pass it a
 * non-kernel virtual address.
 */
#define PADDR(kva)						\
({								\
	physaddr_t __m_kva = (physaddr_t) (kva);		\
	if (__m_kva < KERNBASE)					\
		panic("PADDR called with invalid kva %08lx", __m_kva);\
	__m_kva - KERNBASE;					\
})

/* This macro takes a physical address and returns the corresponding kernel
 * virtual address.  It panics if you pass an invalid physical address. */
#define KADDR(pa)						\
({								\
	physaddr_t __m_pa = (pa);				\
	uint32_t __m_ppn = PPN(__m_pa);				\
	if (__m_ppn >= npage)					\
		panic("KADDR called with invalid pa %08lx", __m_pa);\
	__m_pa + KERNBASE;					\
})



extern char bootstacktop[], bootstack[];

extern struct Page* pages;
extern size_t npage;

extern physaddr_t boot_cr3;
extern pde_t* boot_pgdir;

extern struct Segdesc gdt[];
extern struct Pseudodesc gdt_pd;

void i386_vm_init();
void i386_detect_memory(register_t boot_eax, register_t boot_ebx);

int check_user_access(struct Env *env, const void *ptr, size_t len, pte_t pte_bits);

static inline int
check_user_page_access(struct Env *env, const void *ptr, pte_t pte_bits)
{
	pde_t pde = env->env_pgdir[PDX(ptr)];
	pte_t *pgtbl = (pte_t *) KADDR(PTE_ADDR(pde));
	pte_t pte;
	if ((uintptr_t) ptr < ULIM
	    && (pde & (PTE_P | PTE_U)) == (PTE_P | PTE_U)
	    && (!pte_bits || (pde & pte_bits))
	    && ((pte = pgtbl[PTX(ptr)]) & (PTE_P | PTE_U)) == (PTE_P | PTE_U)
	    && (!pte_bits || (pte & pte_bits)))
		return 0;
	else
		return -E_FAULT;
}

void page_init(void);
int  page_alloc(struct Page**);
void page_free(struct Page*);
int  page_insert(pde_t*, struct Page*, uintptr_t va, int perm);
void page_remove(pde_t*, uintptr_t va);
struct Page* page_lookup(pde_t*, uintptr_t va, pte_t**);
void page_decref(struct Page*);
void tlb_invalidate(pde_t*, uintptr_t va);

static inline ppn_t
page2ppn(struct Page* pp)
{
	return pp - pages;
}

static inline physaddr_t
page2pa(struct Page* pp)
{
	return page2ppn(pp) << PGSHIFT;
}

static inline struct Page*
pa2page(physaddr_t pa)
{
	if (PPN(pa) >= npage)
		panic("pa2page called with invalid pa");
	return &pages[PPN(pa)];
}

static inline uintptr_t
page2kva(struct Page* pp)
{
	return KADDR(page2pa(pp));
}

int pgdir_walk(pde_t* pgdir, uintptr_t va, int create, pte_t** ppte);

#endif /* !KUDOS_KERN_PMAP_H */
