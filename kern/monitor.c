// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/pmap.h>
#include <inc/assert.h>
#include <inc/x86.h>
#include <inc/error.h>
#include <inc/config.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/trap.h>
#include <kern/pmap.h>
#include <kern/env.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/vga.h>
#include <kern/breakpoints.h>
#include <kern/elf.h>
#include <kern/version.h>

struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{"help", "Display this list of commands", mon_help},
	{"kerninfo", "Display information about the kernel", mon_kerninfo},
	{"break", "Breakpoint inspection and manipulation", mon_breakpoint},
 	{"bt", "Display a backtrace", mon_backtrace},
	{"syms", "Display symbols", mon_symbols},
	{"page_alloc", "Allocate a physical page", mon_page_alloc},
	{"page_free", "Free a physical page", mon_page_free},
	{"page_status", "Display physical page status", mon_page_status},
	{"page_map", "Map a physical page to a virtual address", mon_page_map},
	{"page_unmap", "Unmap a virtual address", mon_page_unmap},
	{"show_page_maps", "Show page mappings for a virtual address range", mon_show_page_maps},
	{"set_dir_perm", "Set directory permissions for a virtual address", mon_set_dir_perm},
	{"set_page_perm", "Set page permissions for a virtual address", mon_set_page_perm},
	{"dump_phys_mem", "Dump memory at a physical address range", mon_dump_mem},
	{"dump_virt_mem", "Dump memory at a virtual address range", mon_dump_mem},
	{"env_list", "List active environments", mon_env_list},
	{"env_current", "List or set the current envid", mon_env_current},
	{"env_priority", "Set the priority of an environment", mon_env_priority},
	{"env_run", "Run an environment, or the current environment", mon_env_run},
	{"env_kill", "Kill an environment, or the current environment", mon_env_kill},
	{"env_debug", "Manage environment debugging output", mon_env_debug},
	{"shell", "Start a shell", mon_shell},
	{"exit", "Exits the monitor", mon_exit},
};
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))


/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char** argv, struct Trapframe* tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		printf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char** argv, struct Trapframe* tf)
{
	extern char _start[], etext[], edata[], end[];

	version();

	printf("Special kernel symbols:\n");
	printf("  _start %08x (virt)  %08x (phys)\n", _start, _start-KERNBASE);
	printf("  etext  %08x (virt)  %08x (phys)\n", etext, etext-KERNBASE);
	printf("  edata  %08x (virt)  %08x (phys)\n", edata, edata-KERNBASE);
	printf("  end    %08x (virt)  %08x (phys)\n", end, end-KERNBASE);
	printf("Kernel executable memory footprint: %dKB\n",
		(end-_start+1023)/1024);
	return 0;
}


// List all symbols
int
mon_symbols(int argc, char **argv, struct Trapframe *tf)
{
	uint32_t i;
	struct Sym *symtbl, *symtbl_end;

	if(argc > 2)
	{
		printf("Usage: %s [envx]\n");
		return 0;
	}

	envid_t envid;
	if(argc == 1)
	{
		envid = ENVID_KERNEL;
	} 
	else
	{
		const int envx = strtol(argv[1], 0, 10);
		if(envs[envx].env_status == ENV_FREE)
		{
			printf("Envx %d is free\n", envx);
			return 0;
		}
		envid = envs[envx].env_id;

		if(envid != curenv->env_id)
		{
			printf("Only able to display symbols for current env\n");
			return 0;
		}
	}

	int r;
	if((r = get_symtbl(envid, &symtbl, &symtbl_end, 0, 0)))
	{
		printf("get_symtbl() on envid 0x%x: %e\n", envid, r);
		return 0;
	}

	struct Sym *sym = &symtbl[1]; // 1 and not 0 because the 0th entry is empty
	for(i=1; sym < symtbl_end; ++sym, ++i)
	{
		char *name = get_symbol_name(envid, sym);
		printf("(%03d) @ 0x%08x, info: 0x%02x, \"%s\"\n",
				 i, sym->st_value, sym->st_info, name);
	}
	return 0;
}

int
mon_backtrace(int argc, char** argv, struct Trapframe *tf)
{
	if(argc == 3)
	{
		register_t ebp = (uint32_t) strtol(argv[1], 0, 16);
		register_t eip = (uint32_t) strtol(argv[2], 0, 16);
		return print_backtrace(tf, &ebp, &eip);
	}
	else
	{
		return print_backtrace(tf, NULL, NULL);
	}
}


int
mon_breakpoint(int argc, char **argv, struct Trapframe *tf)
{
	size_t switch_argn;

	if(argc <= 1)
	{
		printf("Usage: set <debug_reg> %s [exec | mem <w|rw> <size>]\n", locn_syntax);
		printf("       <on|off> [<debug_reg>]  // if no <debug_reg>, all registers\n");
		printf("       ss <on|off>\n");
		printf("       show\n");
		return 0;
	}

	switch_argn = 1;
	if(!strcmp("set", argv[switch_argn]))
	{
		switch_argn++;

		if(argc <= switch_argn)
		{
			printf("Bad number of arguments\n");
			return 0;
		}
		uint32_t reg = (uint32_t) strtol(argv[switch_argn++], NULL, 10);
		if(reg >= 4)
		{
			printf("Illegal debug register\n");
			return 0;
		}
		if(argc <= switch_argn)
		{
			printf("Bad number of arguments\n");
			return 0;
		}

		int r;
		envid_t envid;
		uintptr_t addr;
		if((r=locn_to_vaddr(argv[switch_argn], &envid, &addr)))
		{
			if(-E_BAD_SYM == r)
				printf("No symbols with the name \"%s\"\n", argv[switch_argn]);
			else if(-E_BAD_ENV == r)
				printf("Bad env number\n");
			else if(-E_INVAL == r)
				printf("Multiple colons\n");
			else
				printf("Unexpected error \"%e\"\n", r);
			return 0;
		}
	
		switch_argn++;

		if(argc <= switch_argn)
		{
				printf("Bad number of arguments\n");
				return 0;
		}

		bool mem_exec;
		if(!strcmp("mem", argv[switch_argn]))
		{
			mem_exec = 0;

			if(argc != switch_argn+3)
			{
				printf("Bad number of arguments\n");
				return 0;
			}
			switch_argn++;

			bool w_rw;
			if(!strcmp("w", argv[switch_argn]))
				w_rw = 0;
			else if(!strcmp("rw", argv[switch_argn]))
				w_rw = 1;
			else
			{
				printf("Bad w/rw argument\n");
				return 0;
			}

			int len = strtol(argv[switch_argn+1], NULL, 10);

			return breakpoints_set(envid, reg, addr, mem_exec, w_rw, len);
		}
		else if(!strcmp("exec", argv[switch_argn]))
		{
			mem_exec = 1;

			return breakpoints_set(envid, reg, addr, mem_exec, 0, 0);
		}
		else
		{
			printf("Bad argument to set\n");
			return 0;
		}
	}
	else if(!strcmp("on", argv[switch_argn])
			  || !strcmp("off", argv[switch_argn]))
	{
		bool active;
		if(!strcmp("on", argv[switch_argn++]))
			active = 1;
		else
			active = 0;

		int32_t reg;
		if(argc == 1+switch_argn)
			reg = (int32_t) strtol(argv[switch_argn], NULL, 10);
		else if(argc == switch_argn)
			reg = -1;
		else
		{
			printf("Bad on/off argument, argc = %d\n", argc);
			return 0;
		}

		return breakpoints_active(reg, active, 0);
	}
	else if(!strcmp("ss", argv[switch_argn]))
	{
		switch_argn++;
		if(switch_argn+1 != argc)
		{
			printf("Bad number of arguments\n");
			return 0;
		}
	
		bool active;
		if(!strcmp("on", argv[switch_argn]))
			active = 1;
		else if(!strcmp("off", argv[switch_argn]))
			active = 0;
		else
		{
			printf("Unknown argument %s\n", argv[switch_argn]);
			return 0;
		}
		return breakpoints_ss_active(tf, active);
	}
	else if(!strcmp("show", argv[switch_argn]))
	{
		return breakpoints_print(tf);
	}
	else
	{
		printf("Unknown command %s\n", argv[switch_argn]);
		return 0;
	}
	
	return 0;
}

int mon_page_alloc(int argc, char ** argv, struct Trapframe * tf)
{
	struct Page * page;
	if(page_alloc(&page))
	{
		printf("Out of memory\n");
		return -E_NO_MEM;
	}
	printf("  0x%08x\n", page2pa(page));
	return 0;
}

int mon_page_free(int argc, char ** argv, struct Trapframe * tf)
{
	pde_t * pgdir = (pde_t *) KADDR(rcr3());
	physaddr_t pa;
	struct Page * page;
	
	if(argc != 2)
	{
		printf("Usage: %s <physaddr>\n", argv[0]);
		return 0;
	}
	
	pa = strtol(argv[1], NULL, 16);
	/* clip it to PGSIZE */
	pa = PTE_ADDR(pa);
	page = pa2page(pa);
	
	if(!page->pp_link.le_next && !page->pp_link.le_prev)
	{
		int pdi, maps = 0;
		printf("  0x%08x: freed, unmapped from:\n", pa);
		for(pdi = 0; pdi != 1024; pdi++)
		{
			int pti;
			pte_t * pt;
			if(!pgdir[pdi])
				continue;
			pt = (pte_t *) KADDR(PTE_ADDR(pgdir[pdi]));
			for(pti = 0; pti != 1024; pti++)
			{
				if(pt[pti] & PTE_P && PTE_ADDR(pt[pti]) == pa)
				{
					uintptr_t va = (pdi << PTSHIFT) + (pti << PGSHIFT);
					if(KERNBASE <= va)
						break;
					page_remove(pgdir, va);
					printf("    0x%08x\n", va);
					maps++;
				}
			}
		}
		if(!maps)
		{
			printf("    (not unmapped)\n");
			if(!page->pp_ref)
				page_free(page);
		}
	}
	else
		printf("  0x%08x: free\n", pa);
	
	return 0;
}

int mon_page_status(int argc, char ** argv, struct Trapframe * tf)
{
	pde_t * pgdir = (pde_t *) KADDR(rcr3());
	physaddr_t pa;
	struct Page * page;
	
	if(argc != 2)
	{
		printf("Usage: %s <physaddr>\n", argv[0]);
		return 0;
	}
	
	pa = strtol(argv[1], NULL, 16);
	/* clip it to PGSIZE */
	pa = PTE_ADDR(pa);
	page = pa2page(pa);
	
	if(!page->pp_link.le_next && !page->pp_link.le_prev)
	{
		int pdi, maps = 0;
		printf("  0x%08x: allocated, reference count %d, mapped at:\n", pa, page->pp_ref);
		for(pdi = 0; pdi != 1024; pdi++)
		{
			int pti;
			pte_t * pt;
			if(!pgdir[pdi])
				continue;
			pt = (pte_t *) KADDR(PTE_ADDR(pgdir[pdi]));
			for(pti = 0; pti != 1024; pti++)
			{
				if(pt[pti] & PTE_P && PTE_ADDR(pt[pti]) == pa)
				{
					printf("    0x%08x\n", (pdi << PTSHIFT) + (pti << PGSHIFT));
					maps++;
				}
			}
		}
		if(!maps)
			printf("    (not mapped)\n");
	}
	else
		printf("  0x%08x: free\n", pa);
	
	return 0;
}

int mon_page_map(int argc, char ** argv, struct Trapframe * tf)
{
	pde_t * pgdir = (pde_t *) KADDR(rcr3());
	physaddr_t pa;
	uintptr_t va;
	struct Page * page;
	pte_t * pte;
	
	if(argc != 3)
	{
		printf("Usage: %s <physaddr> <virtaddr>\n", argv[0]);
		return 0;
	}
	
	pa = strtol(argv[1], NULL, 16);
	va = strtol(argv[2], NULL, 16);
	
	/* clip them to PGSIZE */
	pa = PTE_ADDR(pa);
	va = PTE_ADDR(va);
	
	page = pa2page(pa);
	
	pgdir_walk(pgdir, va, 0, &pte);
	if(pte && *pte)
		printf("  0x%08x: unmapped (was mapped to 0x%08x)\n", va, PTE_ADDR(*pte));
	page_insert(pgdir, page, va, PTE_U | PTE_W);
	printf("  0x%08x: mapped at 0x%08x\n", pa, va);
	
	return 0;
}

int mon_page_unmap(int argc, char ** argv, struct Trapframe * tf)
{
	pde_t * pgdir = (pde_t *) KADDR(rcr3());
	uintptr_t va;
	pte_t * pte;
	
	if(argc != 2)
	{
		printf("Usage: %s <virtaddr>\n", argv[0]);
		return 0;
	}
	
	va = strtol(argv[1], NULL, 16);
	/* clip it to PGSIZE */
	va = PTE_ADDR(va);
	
	pgdir_walk(pgdir, va, 0, &pte);
	if(pte && *pte)
	{
		printf("  0x%08x: unmapped (was mapped to 0x%08x)\n", va, PTE_ADDR(*pte));
		page_remove(pgdir, va);
	}
	else
		printf("  0x%08x: not mapped\n", va);
	
	return 0;
}

int mon_show_page_maps(int argc, char ** argv, struct Trapframe * tf)
{
	pde_t * pgdir = (pde_t *) KADDR(rcr3());
	physaddr_t start, end;
	int maps = 0;
	
	if(argc != 3)
	{
		printf("Usage: %s <vstart> <vend>\n", argv[0]);
		return 0;
	}
	
	start = strtol(argv[1], NULL, 16);
	end = strtol(argv[2], NULL, 16);
	
	if(start > end)
	{
		printf("Start address must be <= end address\n");
		return 0;
	}
	
	for(start = PTE_ADDR(start); start <= end; start += PGSIZE)
	{
		pde_t * pde = &pgdir[PDX(start)];
		pte_t * pte;
		pgdir_walk(pgdir, start, 0, &pte);
		if(!pte || !*pte)
			continue;
		printf("  0x%08x: mapped to 0x%08x, directory (", start, PTE_ADDR(*pte));
		if(*pde & PTE_U)
			printf("U");
		if(*pde & PTE_W)
			printf("W");
		printf("), table (");
		if(*pte & PTE_U)
			printf("U");
		if(*pte & PTE_W)
			printf("W");
		printf("), effective (");
		if((*pde & PTE_U) && (*pte & PTE_U))
			printf("U");
		if((*pde & PTE_W) && (*pte & PTE_W))
			printf("W");
		printf(")\n");
		maps++;
	}
	if(!maps)
		printf("  (no mappings)\n");
	
	return 0;
}

int mon_set_dir_perm(int argc, char ** argv, struct Trapframe *tf)
{
	pde_t * pgdir = (pde_t *) KADDR(rcr3());
	uintptr_t address;
	pde_t * pde;
	pde_t mask = ~(PTE_U | PTE_W);
	
	if(argc != 2 && argc != 3)
	{
		printf("Usage: %s <virtaddr> [U][W]\n", argv[0]);
		return 0;
	}
	
	address = strtol(argv[1], NULL, 16);
	pde = &pgdir[PDX(address)];
	
	if(!*pde)
	{
		printf("No page directory entry for address 0x%08x\n", address);
		return 0;
	}
	
	if(argc == 3)
	{
		if(strchr(argv[2], 'U'))
			mask &= PTE_U;
		if(strchr(argv[2], 'W'))
			mask &= PTE_W;
	}
	
	*pde &= mask;
	tlb_invalidate(pgdir, PTE_ADDR(address));
	
	return 0;
}

int mon_set_page_perm(int argc, char ** argv, struct Trapframe * tf)
{
	pde_t * pgdir = (pde_t *) KADDR(rcr3());
	uintptr_t address;
	pte_t * pte;
	pte_t mask = ~(PTE_U | PTE_W);
	
	if(argc != 2 && argc != 3)
	{
		printf("Usage: %s <virtaddr> [U][W]\n", argv[0]);
		return 0;
	}
	
	address = strtol(argv[1], NULL, 16);
	pgdir_walk(pgdir, address, 0, &pte);
	
	if(!pte || !*pte)
	{
		printf("No page table entry for address 0x%08x\n", address);
		return 0;
	}
	
	if(argc == 3)
	{
		if(strchr(argv[2], 'U'))
			mask &= PTE_U;
		if(strchr(argv[2], 'W'))
			mask &= PTE_W;
	}
	
	*pte &= mask;
	tlb_invalidate(pgdir, PTE_ADDR(address));
	
	return 0;
}

int mon_dump_mem(int argc, char ** argv, struct Trapframe * tf)
{
	const int phys = strcmp(argv[0], "dump_virt_mem");
	physaddr_t start, end, now;
	
	/* special case for single byte dumps */
	if(argc == 2)
	{
		start = strtol(argv[1], NULL, 16);
		if(phys)
			start = KADDR(start);
		printf("0x%02x\n", *(uint8_t *) start);
		return 0;
	}
	
	if(argc != 3)
	{
		printf("Usage: %s <start> [end]\n", argv[0]);
		return 0;
	}
	
	start = strtol(argv[1], NULL, 16);
	end = strtol(argv[2], NULL, 16);
	
	if(start > end)
	{
		printf("Start address must be <= end address\n");
		return 0;
	}
	
	if(phys)
	{
		start = KADDR(start);
		end = KADDR(end);
	}
	
	/* display it hexdump -C style */
	now = start & ~0xf;
	while(now < end)
	{
		int i;
		
		if(phys)
			printf("%08x ", PADDR(now));
		else
			printf("%08x ", now);
		
		for(i = 0; i != 0x10; i++, now++)
		{
			if(i == 0x8)
				printf(" ");
			if(start <= now && now < end)
				printf(" %02x", *(uint8_t *) now);
			else
				printf("   ");
		}
		
		printf("  |");
		now -= 0x10;
		for(i = 0; i != 0x10; i++, now++)
		{
			if(start <= now && now < end)
			{
				uint8_t byte = *(uint8_t *) now;
				if(byte < ' ' || '~' < byte)
					byte = '.';
				printf("%c", byte);
			}
			else
				printf(" ");
		}
		printf("|\n");
	}
	
	return 0;
}

int mon_env_list(int argc, char ** argv, struct Trapframe * tf)
{
	int i, count = 0;
	for(i = 0; i != NENV; i++)
		if(envs[i].env_status != ENV_FREE)
		{
			printf("[%08x]%c stat %c pri %02d/%02d ip 0x%08x", envs[i].env_id, (&envs[i] == curenv) ? '*' : ' ', (envs[i].env_status == ENV_RUNNABLE) ? 'r' : 'N', envs[i].env_epriority, envs[i].env_rpriority, envs[i].env_tf.tf_eip);
			if(envs[i].env_tf.tf_trapno == T_SYSCALL)
				printf(" syscall %02d", envs[i].env_tf.tf_eax);
			else
				printf("           ");
			printf(" (%s)\n", envs[i].env_name);
			count++;
		}
	if(!count)
		printf("  (no environments)\n");
	return 0;
}

int mon_env_current(int argc, char ** argv, struct Trapframe * tf)
{
	envid_t envid;
	struct Env * e;
	
	if(argc == 1)
	{
		printf("[%08x]\n", curenv ? curenv->env_id : 0);
		return 0;
	}
	
	if(argc != 2)
	{
		printf("Usage: %s [envid]\n", argv[0]);
		return 0;
	}
	
	if(!tf)
	{
		printf("Cannot change current environment without a trap frame!\n");
		return 0;
	}
	
	envid = strtol(argv[1], NULL, 16);
	
	if(envid2env(envid, &e, 0) || e->env_status == ENV_FREE)
	{
		printf("No such environment\n");
		return 0;
	}
	
	/* we have to make sure the user trap frame matches the new current
	 * environment, so that the next call to env_run will not save it to the
	 * wrong environment... we don't need to save the current one back to
	 * curenv, because that was done in trap() */
	*tf = e->env_tf;
	curenv = e;
	lcr3(e->env_cr3);
	
	return 0;
}

int mon_env_priority(int argc, char ** argv, struct Trapframe * tf)
{
	struct Env * e;
	int priority;
	
	if(argc == 2)
	{
		e = curenv;
		priority = strtol(argv[1], NULL, 10);
	}
	else if(argc == 3)
	{
		envid_t envid = strtol(argv[1], NULL, 16);
		if(envid2env(envid, &e, 0) || e->env_status == ENV_FREE)
		{
			printf("No such environment\n");
			return 0;
		}
		priority = strtol(argv[2], NULL, 10);
	}
	else
	{
		printf("Usage: %s [envid] <priority>\n", argv[0]);
		return 0;
	}
	
	sched_update(e, priority);
	e->env_rpriority = priority;
	
	return 0;
}

int mon_env_run(int argc, char ** argv, struct Trapframe * tf)
{
	envid_t envid;
	struct Env * e;
	
	if(argc == 1)
	{
		if(!curenv || curenv->env_status == ENV_FREE)
			printf("No current environment\n");
		else if(curenv->env_status != ENV_RUNNABLE)
			printf("Current environment not runnable\n");
		else
			env_run(curenv);
		return 0;
	}
	
	if(argc != 2)
	{
		printf("Usage: %s [envid]\n", argv[0]);
		return 0;
	}
	
	envid = strtol(argv[1], NULL, 16);
	
	if(envid2env(envid, &e, 0) || e->env_status == ENV_FREE)
	{
		printf("No such environment\n");
		return 0;
	}
	
	if(e->env_status != ENV_RUNNABLE)
		printf("Environment [%08x] is not runnable\n", e);
	else
		env_run(e);
	
	return 0;
}

int mon_env_kill(int argc, char ** argv, struct Trapframe * tf)
{
	int i;
	
	if(argc == 1)
	{
		if(!curenv || curenv->env_status == ENV_FREE)
			printf("No current environment\n");
		else
			env_destroy(curenv);
		return 0;
	}
	
	for(i = 1; i != argc; i++)
	{
		struct Env * e;
		envid_t envid = strtol(argv[i], NULL, 16);
		
		if(envid2env(envid, &e, 0) || e->env_status == ENV_FREE)
		{
			printf("No such environment [%08x]\n", envid);
			return 0;
		}
		
		env_destroy(e);
	}
	
	return 0;
}

int mon_env_debug(int argc, char ** argv, struct Trapframe * tf)
{
	if(argc > 2)
		printf("Usage: %s [debug]\n", argv[0]);
	else if(argc == 2)
	{
		env_debug = strtol(argv[1], NULL, 0);
		printf("env_debug %sabled\n", env_debug ? "en" : "dis");
	}
	else
		printf("env_debug is %sabled\n", env_debug ? "en" : "dis");
	
	return 0;
}

int mon_shell(int argc, char ** argv, struct Trapframe * tf)
{
	ENV_CREATE(user_initsh);
	return 0;
}

int mon_exit(int argc, char ** argv, struct Trapframe * tf)
{
	return -1;
}


/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char* buf, struct Trapframe* tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			printf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	printf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe* tf)
{
	char* buf;

#if CLASS_WELCOME_FORMAT
	printf("Welcome to the JOS kernel monitor!\n");
	printf("Type 'help' for a list of commands.\n");
#else
	printf("KudOS kernel monitor.\n");
#endif

	if (tf != NULL) {
		if (tf->tf_trapno == T_DEBUG)
			breakpoints_print(tf);
		print_trapframe(tf);
	}

	while (1) {
		buf = readline("M> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
