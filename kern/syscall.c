/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/josnic.h>
#include <inc/sb16.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/kclock.h>
#include <kern/sched.h>
#include <kern/kernbin.h>
#include <kern/sb16.h>
#include <kern/vga.h>
#include <kern/3c509.h>
#include <kern/elf.h>
#include <kern/kclock.h>


/**********************************
 * SYSTEM CALLS FOR LAB 3, PART 1 *
 *                                *
 **********************************/

// Print a string to the system console.
// The system call returns 0.
static void
sys_cputs(const char* s)
{
	uint32_t old_fault_mode = page_fault_mode;
	page_fault_mode = PFM_KILL;
	printf("%s", TRUP(s));
	page_fault_mode = old_fault_mode;
}

// Read a character from the system console.
// Returns the character.
static int
sys_cgetc(void)
{
	int c;

	printf("KudOS kernel warning: [%08x] (%s) called sys_cgetc()\n", curenv->env_id, curenv->env_name);
	// The cons_getc() primitive doesn't wait for a character,
	// but the sys_cgetc() system call does.
	while ((c = cons_getc()) == -1)
		; /* spin */

	return c;
}

static int
sys_cgetc_nb(void)
{
	return cons_getc();
}

// Return the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	if (env_debug) {
		if (e == curenv)
			printf("[%08x] exiting gracefully\n", curenv->env_id);
		else
			printf("[%08x] destroying %08x\n", curenv->env_id, e->env_id);
	}
	env_destroy(e);
	return 0;
}



/********************************************************
 * SYSTEM CALLS FOR LAB 3, PART 3                       *
 *                                                      *
 * No need to implement these until you get to Part 3.  *
 *                                                      *
 ********************************************************/

// Exercise 8:
// Deschedule current environment and pick a different one to run.
// The system call returns 0.
static void
sys_yield(void)
{
	sched_yield();
}

// Exercise 9:
// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.
	//
	// Hint: Your code in env_run() shows how to copy a register set.
	
	struct Env * e;
	if(env_alloc(&e, curenv->env_id, curenv->env_rpriority))
		return -E_NO_FREE_ENV;
	e->env_tf = *UTF;
	e->env_tf.tf_eax = 0;
	memcpy(e->env_name, curenv->env_name, ENV_NAME_LENGTH);
	return e->env_id;
}

static int
sys_env_set_name(envid_t envid, char * name)
{
	uint32_t old_fault_mode = page_fault_mode;
	char buffer[ENV_NAME_LENGTH];
	struct Env * e;
	
	if(envid2env(envid, &e, 1) || e->env_status == ENV_FREE)
		return -E_BAD_ENV;
	
	strncpy(buffer, TRUP(name), ENV_NAME_LENGTH - 1);
	buffer[ENV_NAME_LENGTH - 1] = 0;
	strcpy(e->env_name, buffer);
	
	page_fault_mode = old_fault_mode;
	return 0;
}

// Exercise 9:
// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
  	// Hint: Use the 'envid2env' function from kern/env.c to translate an
  	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.
	
	struct Env * e;
	
	if(envid2env(envid, &e, 1) || e->env_status == ENV_FREE)
		return -E_BAD_ENV;
	if(status != ENV_RUNNABLE && status != ENV_NOT_RUNNABLE)
		return -E_INVAL;
	
	e->env_status = status;
	return 0;
}

// Exercise 9:
// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, uintptr_t va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!
	
	struct Env * e;
	struct Page * page;
	
	if(envid2env(envid, &e, 1) || e->env_status == ENV_FREE)
		return -E_BAD_ENV;
	if(va >= UTOP || PTE_ADDR(va) != va)
		return -E_INVAL;
	if(!(perm & PTE_U) || !(perm & PTE_P) || (perm & ~PTE_USER))
		return -E_INVAL;
	
	if(page_alloc(&page))
		return -E_NO_MEM;
	memset((void *) KADDR(page2pa(page)), 0, PGSIZE);
	if(page_insert(e->env_pgdir, page, va, perm))
	{
		page_free(page);
		return -E_NO_MEM;
	}
	
	return 0;
}

// Exercise 9:
// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, uintptr_t srcva,
	     envid_t dstenvid, uintptr_t dstva,
	     int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.
	
	struct Env * se;
	struct Env * de;
	struct Page * page;
	pte_t * pte;
	
	if(envid2env(srcenvid, &se, 1) || se->env_status == ENV_FREE)
		return -E_BAD_ENV;
	if(envid2env(dstenvid, &de, 1) || de->env_status == ENV_FREE)
		return -E_BAD_ENV;
	if(srcva >= UTOP || PTE_ADDR(srcva) != srcva)
		return -E_INVAL;
	if(dstva >= UTOP || PTE_ADDR(dstva) != dstva)
		return -E_INVAL;
	
	page = page_lookup(se->env_pgdir, srcva, &pte);
	if(!pte)
		return -E_INVAL;
	
	if(!(perm & PTE_U) || !(perm & PTE_P) || (perm & ~PTE_USER))
		return -E_INVAL;
	/* we don't have to check the page directory permissions because
	 * for all pages < UTOP, the directory entries are already UW */
	if((perm & PTE_W) && !(*pte & PTE_W))
		return -E_INVAL;
	
	if(page_insert(de->env_pgdir, page, dstva, perm))
		return -E_NO_MEM;
	
	return 0;
}

// Exercise 9:
// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, uintptr_t va)
{
	// Hint: This function is a wrapper around page_remove().
	
	struct Env * e;
	
	if(envid2env(envid, &e, 1) || e->env_status == ENV_FREE)
		return -E_BAD_ENV;
	if(va >= UTOP || PTE_ADDR(va) != va)
		return -E_INVAL;
	
	page_remove(e->env_pgdir, va);
	
	return 0;
}

static int
sys_env_set_priority(envid_t envid, int priority)
{
	struct Env * e;
	
	if(envid2env(envid, &e, 1) || e->env_status == ENV_FREE)
		return -E_BAD_ENV;
	
	e->env_rpriority = priority;
	if(sched_update(e, priority))
		return -E_INVAL;
	
	return 0;
}

// Lab 4 Exercise 1:
// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_set_pgfault_upcall(envid_t envid, uintptr_t upcall)
{
	struct Env * e;
	
	if(envid2env(envid, &e, 1) || e->env_status == ENV_FREE)
		return -E_BAD_ENV;
	
	e->env_pgfault_upcall = upcall;
	return 0;
}

// Lab 4 Exercise 8:
// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(envid_t fromenv, uintptr_t dstva, int timeout)
{
	if(dstva >= UTOP)
		curenv->env_ipc_dstva = UTOP;
	else if(dstva != PTE_ADDR(dstva))
		return -E_INVAL;
	else
		curenv->env_ipc_dstva = dstva;
	
	/* 0 or negative mean "forever" (really 248 days at 100Hz) */
	if(timeout < 1)
		timeout = 0x7FFFFFFF;
	
	curenv->env_ipc_recving = 1;
	curenv->env_status = ENV_NOT_RUNNABLE;
	curenv->env_ipc_timeout = jiffies + timeout;
	curenv->env_ipc_allow_from = fromenv;
	sched_yield();
}

// Lab 4 Exercise 8:
// Try to send 'value' to the target env 'envid'.
// If va != 0, then also send page currently mapped at 'va',
// so that receiver gets a duplicate mapping of the same page.
// If env_ipc_recvingfrom is not 0 and not the sender's id, fails.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target has not requested IPC with sys_ipc_recv.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again.
//
// If the sender sends a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
//
// Returns 0 on success where no page mapping occurs,
// 1 on success where a page mapping occurs, and < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, uintptr_t srcva, unsigned perm, uintptr_t capva)
{
	struct Env * e;
	int map = 0;
	uint32_t cappa;
	
	if(envid2env(envid, &e, 0) || e->env_status == ENV_FREE)
		return -E_BAD_ENV;
	
	if(!e->env_ipc_recving || (e->env_ipc_allow_from && e->env_ipc_allow_from != curenv->env_id))
	{
		/* apply priority inversion */
		if(e->env_epriority < curenv->env_epriority)
			sched_update(e, curenv->env_epriority);
		return -E_IPC_NOT_RECV;
	}
	
	if(capva >= UTOP)
		cappa = -1;
	else if(capva != PTE_ADDR(capva))
		return -E_INVAL;
	else
	{
		pte_t * pte;
		page_lookup(curenv->env_pgdir, capva, &pte);
		if(!pte)
			return -E_INVAL;
		cappa = PTE_ADDR(*pte);
	}
	
	if(srcva >= UTOP)
		e->env_ipc_perm = 0;
	else if(srcva != PTE_ADDR(srcva))
		return -E_INVAL;
	else
	{
		pte_t * pte;
		struct Page * page = page_lookup(curenv->env_pgdir, srcva, &pte);
		
		if(!pte)
			return -E_INVAL;
		
		if(!(perm & PTE_U) || !(perm & PTE_P) || (perm & ~PTE_USER))
			return -E_INVAL;
		/* we don't have to check the page directory permissions because
		 * for all pages < UTOP, the directory entries are already UW */
		if((perm & PTE_W) && !(*pte & PTE_W))
			return -E_INVAL;
		
		if(e->env_ipc_dstva != UTOP)
		{
			if(page_insert(e->env_pgdir, page, e->env_ipc_dstva, perm))
				return -E_NO_MEM;
			e->env_ipc_perm = perm;
			map = 1;
		}
		else
			e->env_ipc_perm = 0;
	}
	
	e->env_ipc_from = curenv->env_id;
	e->env_ipc_value = value;
	e->env_ipc_cap = cappa;
	e->env_ipc_recving = 0;
	e->env_status = ENV_RUNNABLE;
	e->env_tf.tf_eax = 0;
	
	/* reverse the priority inversion */
	if(e->env_epriority != e->env_rpriority)
		sched_update(e, e->env_rpriority);
	
#if 0
	/* return to caller */
	return map;
#else
	/* switch to target */
	UTF->tf_eax = map;
	env_run(e);
#endif
}

static int
sys_batch_syscall(register_t * batch, int count, uint32_t flags)
{
	uint32_t old_fault_mode = page_fault_mode;
	int i;
	page_fault_mode = PFM_KILL;
	for(i = 0; i < count; i++)
	{
		/* all syscalls must preserve page_fault_mode for recursion due to batching */
		batch[0] = syscall(batch[0], batch[1], batch[2], batch[3], batch[4], batch[5]);
		batch = &batch[6];
	}
	page_fault_mode = old_fault_mode;
	return i;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' (which must be read-only) in the address space of 'envid'.
// The page's contents are set to data from the kernel binary named 'name'
// starting at binary offset 'offset'.
// Thus, multiple calls to sys_kernbin_page_alloc can map an entire
// ELF binary.
// Any bytes past the length of the kernel binary are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
// Multiple calls to sys_kernbin_page_alloc() for the same binary/offset
// pair can return mappings to the same physical page (i.e. there is a cache).
//
// Returns the size of the kernel binary on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if perm contains PTE_W.
//	-E_INVAL if name does not name a valid kernel binary.
//	-E_INVAL if offset is not page-aligned.
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static ssize_t
sys_kernbin_page_alloc(envid_t envid, const char* name, uint32_t offset, uintptr_t va, unsigned perm)
{
	uint32_t old_fault_mode = page_fault_mode;
	struct Env* env;
	struct Kernbin* kernbin;
	struct Page* pg;
	int retval;
	int pgoff;
	size_t size;
	
	if ((retval = envid2env(envid, &env, 1)) < 0)
		return retval;

	// Find the named kernbin structure.  Protect the binary name!
	page_fault_mode = PFM_KILL;
	name = TRUP(name);
	for (kernbin = kernbins; kernbin->name; kernbin++)
		if (strcmp(kernbin->name, name) == 0)
			break;
	page_fault_mode = old_fault_mode;
	if (!kernbin->name)
		return -E_INVAL;

	// Check the offset and destination VA
	if ((offset & (PGSIZE - 1))
	    || (va & (PGSIZE - 1))
	    || va >= UTOP
	    || (~perm & (PTE_U | PTE_P))
	    /* excludes PTE_W from PTE_USER */
	    || (perm & ~(PTE_U | PTE_P | PTE_AVAIL)))
		return -E_INVAL;

	// Check the page cache
	pgoff = offset >> PGSHIFT;
	if (pgoff < KERNBIN_MAXPAGES && (pg = kernbin->pages[pgoff])) {
		if ((retval = page_insert(env->env_pgdir, pg, va, perm)) < 0)
			return retval;
		return kernbin->size;
	}
	
	// Allocate a page and map it at va
	if ((retval = page_alloc(&pg)) < 0)
		return retval;
	if ((retval = page_insert(env->env_pgdir, pg, va, perm)) < 0) {
		page_free(pg);
		return retval;
	}
	
	// Copy the relevant portion of the binary
	if (offset < kernbin->size) {
		size = PGSIZE;
		if (offset + size > kernbin->size)
			size = kernbin->size - offset;
		memcpy((void*) page2kva(pg), kernbin->data + offset, size);
	} else
		size = 0;

	// Erase remaining portion of page
	memset((uint8_t*) page2kva(pg) + size, 0, PGSIZE - size);

	// Cache the page (and don't lose the reference)
	if (pgoff < KERNBIN_MAXPAGES) {
		pg->pp_ref++;
		kernbin->pages[pgoff] = pg;
	}
	
	// Return the binary size
	return kernbin->size;
}


// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3), and with interrupts enabled.
//
// Hint: You must do something special if 'envid' is the current environment!
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_set_trapframe(envid_t envid, struct Trapframe* tf)
{
	int retval;
	struct Env* e;
	struct Trapframe ltf;
	uint32_t old_fault_mode = page_fault_mode;

	page_fault_mode = PFM_KILL;
	ltf = *TRUP(tf);
	page_fault_mode = old_fault_mode;

	ltf.tf_eflags |= FL_IF;
	/* should this just be assigned GD_UT | 3? depends on iret semantics... */
	ltf.tf_cs |= 3;

	if ((retval = envid2env(envid, &e, 1)) < 0)
		return retval;
	if (e == curenv)
		*UTF = ltf;
	else
		e->env_tf = ltf;
	return 0;
}

static int
sys_sb16_ioctl(int req, uint32_t a1, uint32_t a2, uint32_t a3)
{
	switch(req)
	{
		case SB16_IOCTL_CLOSE:
			return sb16_close();
		case SB16_IOCTL_OPEN:
			return sb16_open((uint16_t) a1, (uint8_t) a2, (uintptr_t) a3);
		case SB16_IOCTL_SETVOLUME:
			return sb16_setvolume((uint8_t) a1);
		case SB16_IOCTL_START:
			return sb16_start();
		case SB16_IOCTL_STOP:
			return sb16_stop();
		case SB16_IOCTL_WAIT:
			return sb16_wait();
	}
	
	return -E_INVAL;
}

static int
sys_vga_set_mode_320(uintptr_t address)
{
	int page, r;
	
	if(address > UTOP - (16 << PGSHIFT) || address != PTE_ADDR(address))
		return -E_INVAL;
	
	r = vga_set_mode_320();
	if(r)
		return r;
	
	/* FIXME: this mapping won't be undone! */
	for(page = 0; page != 16; page++)
	{
		if(page_insert(curenv->env_pgdir, &pages[(VGA_PMEM >> PGSHIFT) + page], address + (page << PGSHIFT), PTE_U | PTE_W | PTE_P))
		{
			while(page--)
				page_remove(curenv->env_pgdir, address + (page << PGSHIFT));
			vga_set_mode_text();
			return -E_NO_MEM;
		}
	}
	
	return 0;
}

static int
sys_vga_set_mode_text(void)
{
	return vga_set_mode_text();
}

static int
sys_vga_set_palette(uint8_t * palette, uint8_t dim)
{
	uint32_t old_fault_mode = page_fault_mode;
	
	page_fault_mode = PFM_KILL;
	vga_set_palette(TRUP(palette), dim);
	page_fault_mode = old_fault_mode;
	
	return 0;
}

static int
sys_net_ioctl(int req, int ival1, void * pval, int ival2)
{
	uint32_t old_fault_mode = page_fault_mode;
	int value = -E_INVAL;
	
	switch(req)
	{
		case NET_IOCTL_ALLOCATE:
			/* allocate/reserve card */
			value = el3_allocate(ival1);
			break;
		case NET_IOCTL_RELEASE:
			/* release card */
			value = el3_release(ival1);
			break;
		case NET_IOCTL_GETADDRESS:
			/* get card HW address */
			page_fault_mode = PFM_KILL;
			value = el3_get_address(ival1, TRUP(pval));
			break;
		case NET_IOCTL_SETFILTER:
			/* set packet filter */
			value = el3_set_filter(ival1, ival2);
			break;
		case NET_IOCTL_RESET:
			/* reset the card */
			value = el3_tx_reset(ival1);
			break;
		case NET_IOCTL_SEND:
			/* send packet */
			page_fault_mode = PFM_KILL;
			value = el3_send_packet(ival1, TRUP(pval), ival2);
			break;
		case NET_IOCTL_QUERY:
			/* query for available packets */
			value = el3_query(ival1);
			break;
		case NET_IOCTL_RECEIVE:
			/* receive packet */
			page_fault_mode = PFM_KILL;
			value = el3_get_packet(ival1, TRUP(pval), ival2);
			break;
	}
	
	page_fault_mode = old_fault_mode;
	return value;
}

static int
sys_reboot()
{
	reboot();
}

static int
sys_set_symtbls(envid_t envid, struct Sym *symtbl, size_t symtbl_size, char *symstrtbl, size_t symstrtbl_size)
{
	int r;
	struct Env *e;

	if (ENVID_KERNEL == envid)
		return -E_BAD_ENV;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;

	return set_symtbls(e->env_id, symtbl, symtbl_size, symstrtbl, symstrtbl_size);
}

int serial_getc(uint8_t port);

#define ON_PAGE_ADDR(addr) (addr == ROUND32(addr, PGSHIFT))

static int
sys_reg_serial(int port, uintptr_t buffer_pg)
{
	int r;
	if(port != -1)
	{
		if((r = Com_user(port)) < 0)
			return r;
		if(r != curenv->env_id)
			return -E_INVAL;
	}
	else
	{
		for(port=0; port < NCOMS; port++)
		{
			if(Com_user(port) == 0)
				break;
		}
		if(port >= NCOMS)
			return -E_INVAL;
	}

	if(buffer_pg >= UTOP || !ON_PAGE_ADDR(buffer_pg))
		return -E_INVAL;


	struct Page *pp = page_lookup(curenv->env_pgdir, buffer_pg, NULL);
	if(pp == 0)
		return -E_INVAL;
	uintptr_t kbuffer_pg = page2kva(pp);

	// Store parameters
	com[port].buf  = kbuffer_pg;
	com[port].user = curenv->env_id;
	UTF->tf_eflags |= FL_IOPL_3;

	// Drain the port's data...
	while(serial_getc(port) != -1);

	return com[port].addr;
}

static int
sys_unreg_serial(int port)
{
	if(port >= NCOMS)
		return -E_INVAL;
	if(Com_user(port) != curenv->env_id)
		return -E_INVAL;

	com[port].user = 0;

	// NOTE: we don't remove the tf flag IOPL_3 since we don't know
	// if the env would otherwise have the flag.

	return 0;
}

static int
sys_grant_io(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;

	if (e == curenv)
		UTF->tf_eflags |= FL_IOPL_3;
	else
		e->env_tf.tf_eflags |= FL_IOPL_3;

	return 0;
}

// RTC challenge
static int
sys_get_hw_time(int* sec, int* min, int* hour, int* day, int* mon)
{
	uint32_t old_fault_mode = page_fault_mode;
	
	*TRUP(sec) = 0xFF & mc146818_read(NULL, 0);
	*TRUP(min) = 0xFF & mc146818_read(NULL, 2);
	*TRUP(hour) = 0xFF & mc146818_read(NULL, 4);
	*TRUP(day) = 0xFF & mc146818_read(NULL, 7);
	*TRUP(mon) = 0xFF & mc146818_read(NULL, 8);
	
	page_fault_mode = old_fault_mode;
	
	return 0xFF & mc146818_read(NULL, 9);
}

// Dispatches to the correct kernel function, passing the arguments.
register_t
syscall(register_t sn, register_t a1, register_t a2, register_t a3, register_t a4, register_t a5)
{
	// printf("syscall %d %x %x %x from env %08x\n", sn, a1, a2, a3, curenv->env_id);

	switch(sn)
	{
		case SYS_cputs:
			sys_cputs((char *) a1);
			return 0;
		case SYS_cgetc:
			return sys_cgetc();
		case SYS_cgetc_nb:
			return sys_cgetc_nb();
		case SYS_getenvid:
			return sys_getenvid();
		case SYS_env_destroy:
			return sys_env_destroy(a1);
		case SYS_yield:
			sys_yield();
			/* never get here, but... */
			return 0;
		case SYS_exofork:
			return sys_exofork();
		case SYS_env_set_name:
			return sys_env_set_name(a1, (char *) a2);
		case SYS_env_set_status:
			return sys_env_set_status(a1, a2);
		case SYS_env_set_priority:
			return sys_env_set_priority(a1, a2);
		case SYS_page_alloc:
			return sys_page_alloc(a1, a2, a3);
		case SYS_page_map:
			return sys_page_map(a1, a2, a3, a4, a5);
		case SYS_page_unmap:
			return sys_page_unmap(a1, a2);
		case SYS_set_pgfault_upcall:
			return sys_set_pgfault_upcall(a1, a2);
		case SYS_ipc_recv:
			return sys_ipc_recv(a1, a2, a3);
		case SYS_ipc_try_send:
			return sys_ipc_try_send(a1, a2, a3, a4, a5);
		case SYS_batch_syscall:
			return sys_batch_syscall((register_t *) a1, a2, a3);
		case SYS_kernbin_page_alloc:
			return sys_kernbin_page_alloc(a1, (char *) a2, a3, a4, a5);
		case SYS_set_trapframe:
			return sys_set_trapframe(a1, (struct Trapframe *) a2);
		case SYS_sb16_ioctl:
			return sys_sb16_ioctl(a1, a2, a3, a4);
		case SYS_vga_set_mode_320:
			return sys_vga_set_mode_320(a1);
		case SYS_vga_set_mode_text:
			return sys_vga_set_mode_text();
		case SYS_vga_set_palette:
			return sys_vga_set_palette((uint8_t *) a1, a2);
		case SYS_net_ioctl:
			return sys_net_ioctl(a1, a2, (void *) a3, a4);
		case SYS_reboot:
			return sys_reboot();
		case(SYS_set_symtbls):
			return sys_set_symtbls(a1, (struct Sym*)a2, a3, (char*)a4, a5);
		case(SYS_reg_serial):
			return sys_reg_serial(a1, a2);
		case(SYS_unreg_serial):
			return sys_unreg_serial(a1);
		case(SYS_grant_io):
			return sys_grant_io(a1);
		case(SYS_get_hw_time):
			return sys_get_hw_time((int*)a1, (int*)a2, (int*)a3, (int*)a4, (int*)a5);
		default:
			return -E_INVAL;
	}
}
