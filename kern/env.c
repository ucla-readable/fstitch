/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/breakpoints.h>

int env_debug = 0;
struct Env* envs = NULL;		// All environments
struct Env* curenv = NULL;	        // Current environment
volatile uint64_t env_tsc = 0;		// Current env start TSC
static struct Env_list env_free_list;	// Free list

//
// Converts an envid to an env pointer.
//
// RETURNS
//   0 on success, -E_BAD_ENV on error.
//   On success, sets *penv to the environment.
//   On error, sets *penv to NULL.
//
int
envid2env(envid_t envid, struct Env** penv, int checkperm)
{
	struct Env* e;

	// If envid is zero, return the current environment.
	if (envid == 0) {
		*penv = curenv;
		return 0;
	}

	// Look up the Env structure via the index part of the envid,
	// then check the env_id field in that struct Env
	// to ensure that the envid is not stale
	// (i.e., does not refer to a _previous_ environment
	// that used the same slot in the envs[] array).
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid) {
		*penv = 0;
		return -E_BAD_ENV;
	}

	// Check that the calling environment has legitimate permission
	// to manipulate the specified environment.
	// If checkperm is set, the specified environment
	// must be either the current environment
	// or an immediate child of the current environment.
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id) {
		*penv = 0;
		return -E_BAD_ENV;
	}
	
	*penv = e;
	return 0;
}

//
// Marks all environments in 'envs' as free and inserts them into 
// the env_free_list.  Insert in reverse order, so that
// the first call to env_alloc() returns envs[0].
//
// Note: There's some similarity with the page_init() function from Lab 2.
// This is simpler, though.
//
void
env_init(void)
{
	int i;
	LIST_INIT(&env_free_list);
	
	for(i = NENV - 1; 0 <= i; i--)
	{
		envs[i].env_status = ENV_FREE;
		LIST_INSERT_HEAD(&env_free_list, &envs[i], env_link);
	}
}

//
// Initializes the kernel virtual memory layout for environment e.
// Allocates a page directory and initializes
// the kernel portion of the new environment's address space.
// Also sets e->env_cr3 and e->env_pgdir accordingly.
// We do NOT (yet) map anything into the user portion
// of the environment's virtual address space.
//
// RETURNS
//   0 -- on sucess
//   <0 -- otherwise 
//
static int
env_setup_vm(struct Env* e)
{
	int i, r;
	struct Page* p = NULL;

	// Allocate a page for the page directory
	if ((r = page_alloc(&p)) < 0)
		return r;

	// Hint:
	//    - The VA space of all envs is identical above UTOP
	//      (except at VPT and UVPT, which we've set below).
	//	See inc/pmap.h for permissions and layout.
	//	Can you use boot_pgdir as a template?  Hint: Yes.
	//	(Make sure you got the permissions right in Lab 2.)
	//    - The initial VA below UTOP is empty.
	//    - You do not need to make any more calls to page_alloc.
	//    - Note: pp_ref is not maintained for physical pages
	//	mapped above UTOP.

	e->env_cr3 = page2pa(p);
	e->env_pgdir = (pde_t *) KADDR(e->env_cr3);
	/* manually increase reference count, because it is the page directory page */
	p->pp_ref++;
	
	for(i = 0; i != PDX(UTOP); i++)
		e->env_pgdir[i] = 0;
	for(; i != NPDENTRIES; i++)
		e->env_pgdir[i] = boot_pgdir[i];

	// VPT and UVPT map the env's own page table, with
	// different permissions.
	e->env_pgdir[PDX(VPT)]   = e->env_cr3 | PTE_P | PTE_W;
	e->env_pgdir[PDX(UVPT)]  = e->env_cr3 | PTE_P | PTE_U;

	return 0;
}

//
// Allocates and initializes a new env.
//
// RETURNS
//   0 -- on success, sets *new to point at the new env 
//   <0 -- on failure
//
int
env_alloc(struct Env** new, envid_t parent_id, int priority)
{
	int r;
	struct Env* e;

	if (!(e = LIST_FIRST(&env_free_list)))
		return -E_NO_FREE_ENV;
	if(priority < 0 || priority > ENV_MAX_PRIORITY)
		return -E_INVAL;
	
	/* only allow priority 0 for envs[0] */
	if(!priority && e != envs)
		priority = ENV_DEFAULT_PRIORITY;

	// Allocate and set up the page directory for this environment.
	if ((r = env_setup_vm(e)) < 0)
		return r;

	// Generate an env_id for this environment.
	{
		// Find e's position in the envs array.
		envid_t idx = e - envs;
		// Increment the uniqueifier, and make sure it's not zero.
		envid_t uniqueifier = (e->env_id + (1 << LOG2NENV));
		if (uniqueifier == idx)
			uniqueifier += 1 << LOG2NENV;
		// Make sure it's not negative!
		e->env_id = (uniqueifier | idx) & 0x7FFFFFFF;
	}
	
	// Set the basic status variables.
	e->env_parent_id = parent_id;
	e->env_status = ENV_NOT_RUNNABLE;
	e->env_runs = 0;
	e->env_tsc = 0;
	e->env_rpriority = priority;
	e->env_name[0] = 0;

	// Clear out all the saved register state,
	// to prevent the register values
	// of a prior environment inhabiting this Env structure
	// from "leaking" into our new environment.
	memset(&e->env_tf, 0, sizeof(e->env_tf));

	// Set up appropriate initial values for the segment registers.
	// GD_UD is the user data segment selector in the GDT, and 
	// GD_UT is the user text segment selector (see inc/pmap.h).
	// The low 2 bits of each segment register contains the
	// Requestor Privilege Level (RPL); 3 means user mode.
	e->env_tf.tf_ds = GD_UD | 3;
	e->env_tf.tf_es = GD_UD | 3;
	e->env_tf.tf_ss = GD_UD | 3;
	e->env_tf.tf_esp = USTACKTOP;
	e->env_tf.tf_cs = GD_UT | 3;
	// e->env_tf.tf_eip is set later.

	// Enable interrupts while in user mode.
	e->env_tf.tf_eflags |= FL_IF;

	// Clear the page fault handler until user installs one.
	e->env_pgfault_upcall = 0;

	// Also clear the IPC receiving flag.
	e->env_ipc_recving = 0;

	// commit the allocation
	LIST_REMOVE(e, env_link);
	memset(&e->env_link, 0, sizeof(e->env_link));
	sched_update(e, priority);
	*new = e;

	if(env_debug)
		printf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
	return 0;
}


//
// Set up the initial program binary, stack, and processor flags
// for a user process.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
//
// This function loads all loadable segments from the ELF binary image
// into the environment's user memory, starting at the appropriate
// virtual addresses indicated in the ELF program header.
// At the same time it clears to zero any portions of these segments
// that are marked in the program header as being mapped
// but not actually present in the ELF file - i.e., the program's bss section.
//
// All this is very similar to what our boot loader does, except the boot
// loader also needs to read the code from disk.  Take a look at
// boot/main.c to get ideas.
//
// Finally, this function maps one page for the program's initial stack.
//
static void
load_icode(struct Env* e, uint8_t* binary, size_t size)
{
	// Hint:
	//  Load each program section into virtual memory
	//  at the address specified in the ELF section header.
	//  You should only load sections with ph->p_type == ELF_PROG_LOAD.
	//  Each section's virtual address can be found in ph->p_va
	//  and its size in memory can be found in ph->p_memsz.
	//  The ph->p_filesz bytes from the ELF binary, starting at
	//  'binary + ph->p_offset', should be copied to virtual address
	//  ph->p_va.  Any remaining memory bytes should be cleared to zero.
	//  (The ELF header should have ph->p_filesz <= ph->p_memsz.)
	//  Use functions from the previous lab to allocate and map pages.
	//
	//  All page protection bits should be user read/write for now.
	//  ELF sections are not necessarily page-aligned, but you can
	//  assume for this function that no two sections will touch
	//  the same virtual page.
	//
	// Hint:
	//  Loading the sections is much simpler if you can move data
	//  directly into the virtual addresses stored in the ELF binary!
	//  So which page directory should be in force during
	//  this function?
	//
	// Hint:
	//  You must also do something with the program's entry point.
	//  What?

	int i;
	struct Page * page;
	struct Elf * elf = (struct Elf *) binary;
	struct Proghdr * ph = (struct Proghdr *) &binary[elf->e_phoff];
	
	physaddr_t old_cr3 = rcr3();
	
	/* FIXME check magic number */
	
	e->env_tf.tf_eip = elf->e_entry;
	lcr3(e->env_cr3);
	
	for(i = 0; i < elf->e_phnum; i++)
	{
		uintptr_t start, end;
		int pages, j;
		
		if(ph[i].p_type != ELF_PROG_LOAD)
			continue;
		
		start = ROUNDDOWN32(ph[i].p_va, PGSIZE);
		end = ROUND32(ph[i].p_va + ph[i].p_memsz, PGSIZE);
		pages = (end - start) >> PGSHIFT;
		
		/* FIXME make these panics go away */
		if(end < start || end > UTOP)
		{
			panic("range problem with segment");
		}
		if(ph[i].p_filesz > ph[i].p_memsz)
		{
			panic("size problem with segment");
		}
		
		for(j = 0; j < pages; j++)
		{
			if(page_alloc(&page))
			{
				panic("failed to allocate");
			}
			if(page_insert(e->env_pgdir, page, start + j * PGSIZE, PTE_W | PTE_U))
			{
				panic("failed to insert");
			}
		}
		
		memcpy((void *) ph[i].p_va, &binary[ph[i].p_offset], ph[i].p_filesz);
		memset((void *) ph[i].p_va + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);
	}
	
	// Now map one page for the program's initial stack
	// at virtual address USTACKTOP - PGSIZE.
	
	if(page_alloc(&page))
	{
		panic("no stack space");
	}
	if(page_insert(e->env_pgdir, page, USTACKTOP - PGSIZE, PTE_W | PTE_U))
	{
		panic("failed to insert");
	}
	
	lcr3(old_cr3);
}

//
// Allocates a new env and loads the elf binary into it.
// This function is ONLY called during kernel initialization,
// before running the first user-mode environment.
// The new env's parent env id is set to 0.
//
void
env_create(uint8_t* binary, size_t size)
{
	struct Env * e;
	int r;
	r = env_alloc(&e, 0, 0);
	if(r)
	{
		panic("env_alloc: %e", r);
		return;
	}
	load_icode(e, binary, size);
	e->env_status = ENV_RUNNABLE;
}

//
// Frees env e and all memory it uses.
// 
void
env_free(struct Env *e)
{
	pte_t *pt;
	uint32_t pdeno, pteno;
	physaddr_t pa;

	// Note the environment's demise.
	if(env_debug)
		printf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, e->env_id);

	if(e == curenv)
		lcr3(PADDR(boot_pgdir));

	// Flush all mapped pages in the user portion of the address space
	static_assert(UTOP % PTSIZE == 0);
	for (pdeno = 0; pdeno < PDX(UTOP); pdeno++) {

		// only look at mapped page tables
		if (!(e->env_pgdir[pdeno] & PTE_P))
			continue;

		// find the pa and va of the page table
		pa = PTE_ADDR(e->env_pgdir[pdeno]);
		pt = (pte_t*)KADDR(pa);

		// unmap all PTEs in this page table
		for (pteno = 0; pteno <= PTX(~0); pteno++) {
			if (pt[pteno] & PTE_P)
				page_remove(e->env_pgdir,
					(pdeno << PDXSHIFT) |
					(pteno << PTXSHIFT));
		}

		// free the page table itself
		e->env_pgdir[pdeno] = 0;
		page_decref(pa2page(pa));
	}

	// free the page directory
	pa = e->env_cr3;
	e->env_pgdir = 0;
	e->env_cr3 = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	sched_remove(e);
	e->env_status = ENV_FREE;
	LIST_INSERT_HEAD(&env_free_list, e, env_link);
}

//
// Frees env e.  And schedules a new env
// if e was the current env.
//
void
env_destroy(struct Env *e) 
{
	env_free(e);

	if (curenv == e) {
		curenv = NULL;
		sched_yield();
	}
}


//
// Restores the register values in the Trapframe
//  (does not return)
//
void
env_pop_tf(struct Trapframe *tf)
{
#if 0
	printf(" --> %d 0x%x\n", ENVX(curenv->env_id), tf->tf_eip);
#endif

	__asm __volatile("movl %0,%%esp" : : "g" (tf) : "memory");
#if ENABLE_ENV_FP
	if(curenv->env_runs > 1)
		__asm __volatile("frstor (%%esp)" : : "g" (tf) : "memory");
	else
		__asm __volatile("finit");
	__asm __volatile("addl %0,%%esp" : : "r" (sizeof(tf->tf_fp)), "g" (tf) : "memory");
#endif
	__asm __volatile(
		"popal\n"
		"\tpopl %%es\n"
		"\tpopl %%ds\n"
		"\taddl $0x8,%%esp\n" /* skip tf_trapno and tf_errcode */
		"\tiret"
		: : "g" (tf) : "memory");
	panic("iret failed");  /* mostly to placate the compiler */
}

//
// Context switch from curenv to env e.
// Note: if this is the first call to env_run, curenv is NULL.
//  (This function does not return.)
//
void
env_run(struct Env* e)
{
	if (curenv) {
		// In Part 3 of the lab, save the register state of the
		// previously executing environment.  There's no need to
		// change anything here until Part 3.
		//
		// Hint: This can be done in a single line of code!
		//   Check out the symbol UTF defined in kern/trap.h.
		
		curenv->env_tf = *UTF;
	}

	// Step 1: Set 'curenv' to the new environment to be run,
	//	and update the 'env_runs' counter.
	// Step 2: Use lcr3() to switch to the new environment's address space.
	// Step 3: Use env_pop_tf() to restore the new environment's registers
	//	and drop into user mode in the new environment.

	// Hint: This function loads the new environment's state from
	//	e->env_tf.  Go back through the code you wrote above
	//	and make sure you have set the relevant parts of
	//	e->env_tf to sensible values, based on e's memory
	//	layout.
	
	curenv = e;
	if(!++curenv->env_runs)
	{
		/* env_runs has wrapped, reset counters */
		curenv->env_runs = 2;
		curenv->env_tsc = 0;
	}
	breakpoints_sched(e->env_id);
	curenv->env_jiffies = jiffies;
	env_tsc = read_tsc();
	lcr3(e->env_cr3);
	if (ENABLE_INKERNEL_INTS)
	{
		/* env_pop_tf() resets %esp, so we need it to be a
		 * valid stack location in case of an interrupt */
		*UTF = e->env_tf;
		env_pop_tf(UTF);
	}
	else
	{
		env_pop_tf(&e->env_tf);
	}
}
