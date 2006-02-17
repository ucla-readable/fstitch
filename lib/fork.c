// implement fork from user space

#include <inc/string.h>
#include <inc/cfs_ipc_client.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

pde_t
get_pde(void* addr)
{
	return vpd[PDX(addr)];
}

pte_t
get_pte(void* addr)
{
	pde_t pde = get_pde(addr);
	if(! (pde & PTE_P) )
		return 0;

	pte_t pte = vpt[VPN(addr)];
	pte = (pte & ~PTE_W) | (pte & pde & PTE_W); // mark W iff pde and pte are W

	return pte;
}

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(void* addr, uint32_t err, uint32_t esp, uint32_t eflags, uint32_t eip)
{
	// Check that the faulting access was a write to a copy-on-write
	// page.  If not, panic.
	// Hint:
	//   Use the read-only page table mapping at vpt (see inc/pmap.h).

	int r;
	pte_t * pte = (pte_t *) UVPT; /* i.e. vpt */
	pde_t * pde = &pte[UVPT >> PGSHIFT]; /* i.e. vpd */
	
	if(!(err & FEC_WR))
		panic("page fault at 0x%08x (read; 0x%08x)", eip, addr);
	if(!pde[PDX(addr)])
		panic("page fault at 0x%08x (directory; 0x%08x)", eip, addr);
	if(!(pte[(uint32_t) addr >> PGSHIFT] & PTE_COW))
		panic("page fault at 0x%08x (table; 0x%08x)", eip, addr);

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.
	//   No need to explicitly delete the old page's mapping.
	
	r = sys_page_alloc(0, (void *) PFTEMP, PTE_U | PTE_W | PTE_P);
	if(r)
		panic("pgfault: %i", r);
	memcpy((void *) PFTEMP, (void *) PTE_ADDR(addr), PGSIZE);
	/* these two can't fail */
	sys_page_map(0, (void *) PFTEMP, 0, (void *) PTE_ADDR(addr), PTE_U | PTE_W | PTE_P);
	sys_page_unmap(0, (void *) PFTEMP);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why mark ours copy-on-write again
// if it was already copy-on-write?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
// 
static int
duppage(envid_t envid, unsigned pn)
{
	int r;
	pte_t pte = vpt[pn];
	void * addr = (void *) (pn << PGSHIFT);

	if(pte & PTE_SHARE)
		goto clone;
	else if(pte & PTE_W)
	{
		pte &= PTE_USER & ~PTE_W;
		pte |= PTE_COW;
		r = sys_page_map(0, addr, envid, addr, pte);
		if(r)
			return r;
		sys_page_map(0, addr, 0, addr, pte);
	}
	else if(pte & PTE_COW)
	{
		pte &= PTE_USER;
		r = sys_page_map(0, addr, envid, addr, pte);
		if(r)
			return r;
		/* necessary? */
		sys_page_map(0, addr, 0, addr, pte);
	}
	else if(pte)
	{
clone:
		r = sys_page_map(0, addr, envid, addr, pte & PTE_USER);
		if(r)
			return r;
	}
	
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use vpd, vpt, and duppage.
//   Remember to fix "env" and the user exception stack in the child process
//   -- and make sure never to mark the current environment's user exception
//   stack as copy-on-write!
//
int
fork(void)
{
	int r;
	uint32_t pdx, ptx;
	envid_t envid;
	pte_t * pte = (pte_t *) UVPT; /* i.e. vpt */
	pde_t * pde = &pte[UVPT >> PGSHIFT]; /* i.e. vpd */
	
	set_pgfault_handler(pgfault);
	
	envid = sys_exofork();
	if(envid < 0)
		panic("sys_exofork: %i", envid);
	if(!envid)
	{
		env = &envs[ENVX(sys_getenvid())];
		return 0;
	}
	
	for(pdx = 0; pdx != (UTOP >> PTSHIFT); pdx++)
	{
		if(!pde[pdx])
			continue;
		for(ptx = 0; ptx != NPTENTRIES; ptx++)
		{
			uint32_t addr = PGADDR(pdx, ptx, 0);
			if(addr != UXSTACKTOP - PGSIZE)
			{
				r = duppage(envid, addr >> PGSHIFT);
				if(r)
					panic("fork: %i", r);
			}
		}
	}
	
	sys_set_pgfault_upcall(envid, (void *) env->env_pgfault_upcall);
	r = sys_page_alloc(envid, (void *) (UXSTACKTOP - PGSIZE), PTE_U | PTE_W | PTE_P);
	if(r)
		panic("fork: %i", r);
	// Copy our IRQ handler if we have one installed
	if(env->env_irq_upcall)
		sys_set_irq_upcall(envid, (void *) env->env_irq_upcall);
	
	// Set kernel's symbol and symbol string tables.
	extern uint8_t _binary_symtbl_start[],    _binary_symtbl_size[];
	extern uint8_t _binary_symstrtbl_start[], _binary_symstrtbl_size[];
	if ((r = sys_set_symtbls(envid, 
									 (void*)  _binary_symtbl_start,
									 (size_t) _binary_symtbl_size,
									 (void*)  _binary_symstrtbl_start,
									 (size_t) _binary_symstrtbl_size)) < 0)
		panic("sys_set_symtbls: %i", r);

	/* Copy our opgroup scope.
	 * No need to error if kfsd is gone (-E_TIMEOUT), let fork continue. */
	r = cfs_opgroup_scope_copy(envid);
	if(r < 0 && r != -E_TIMEOUT)
		panic("cfs_opgroup_scope_copy: %i", r);

	if((r = sys_env_set_status(envid, ENV_RUNNABLE)))
		panic("fork: %i", r);
	
	return envid;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
