#include <inc/x86.h>
#include <inc/elf.h>
#include <inc/string.h>
#include <inc/error.h>
#include <inc/config.h>

#include <kern/env.h>
#include <kern/elf.h>
#include <kern/stabs.h>
#include <kern/trap.h>

const char locn_syntax[] = "[k:|<envnum>:]<laddr|symname>";


struct Sym *kern_symtbl;
size_t      kern_symtbl_size;
char       *kern_symstrtbl;
size_t      kern_symstrtbl_size;


int
get_symtbl(envid_t envid,
			  struct Sym **_symtbl_begin, struct Sym **_symtbl_end,
			  char **_symstrtbl_begin,    char **_symstrtbl_end)
{
	int ret = 0;
	int r;
	struct Sym *symtbl_begin,    *symtbl_end;
	char       *symstrtbl_begin, *symstrtbl_end;

	if(ENVID_KERNEL == envid)
	{
		symtbl_begin    = kern_symtbl;
		symtbl_end      = (struct Sym*) ((uintptr_t) kern_symtbl + kern_symtbl_size);
		symstrtbl_begin = kern_symstrtbl;
		symstrtbl_end   = (char*) ((uintptr_t) kern_symstrtbl + kern_symstrtbl_size);
	}
	else
	{
#if ENABLE_ENV_SYMS
		struct Env *env;
		if((r=envid2env(envid, &env, 0)))
			return r;
		
		symtbl_begin    = env->symtbl;
		symtbl_end      = (struct Sym*) ((uintptr_t) symtbl_begin + env->symtbl_size);
		symstrtbl_begin = env->symstrtbl;
		symstrtbl_end   = (char*) ((uintptr_t) symstrtbl_begin + env->symstrtbl_size);
#else
		symtbl_begin    = NULL;
		symtbl_end      = NULL;
		symstrtbl_begin = NULL;
		symstrtbl_end   = NULL;
		ret = -E_SYMTBL;
#endif
	}


	if(_symtbl_begin)
	{
		if(!symtbl_begin)
			return -E_SYMTBL;
		*_symtbl_begin    = symtbl_begin;
	}
	if(_symtbl_end)
	{
		if(!symtbl_end)
			return -E_SYMTBL;
      *_symtbl_end      = symtbl_end;
	}
	if(_symstrtbl_begin)
	{
		if(!symstrtbl_begin)
			return -E_SYMTBL;
		*_symstrtbl_begin = symstrtbl_begin;
	}
	if(_symstrtbl_end)
	{
		if(!symstrtbl_end)
			return -E_SYMTBL;
		*_symstrtbl_end   = symstrtbl_end;
	}

	return ret;
}


void
set_kernel_symtbls(void)
{
	extern uint8_t _binary_symtbl_start[], _binary_symtbl_size[];
	extern uint8_t _binary_symstrtbl_start[], _binary_symstrtbl_size[];
	set_symtbls(ENVID_KERNEL,
					(struct Sym*)_binary_symtbl_start, (size_t)_binary_symtbl_size,
					(char*)_binary_symstrtbl_start, (size_t)_binary_symstrtbl_size);
}

int set_symtbls(envid_t envid,
					 struct Sym *symtbl, size_t symtbl_size,
					 char *symstrtbl,    size_t symstrtbl_size)
{
	int r;

	if(ENVID_KERNEL == envid)
	{
		kern_symtbl         = symtbl;
		kern_symtbl_size    = symtbl_size;
		kern_symstrtbl      = symstrtbl;
		kern_symstrtbl_size = symstrtbl_size;
	}
	else
	{
#if ENABLE_ENV_SYMS
			struct Env *env;
			if((r=envid2env(envid, &env, 0)))
				return r;
			
			env->symtbl         = symtbl;
			env->symtbl_size    = symtbl_size;
			env->symstrtbl      = symstrtbl;
			env->symstrtbl_size = symstrtbl_size;
#endif
	}
	
	return 0;
}


// Given envid and eip, return the struct Sym* for the function
// most likely containing the eip.
struct Sym *
eip_to_fnsym(envid_t envid, uint32_t eip)
{
	/* This function works by searching through all fn symbols, settling
	 * on fn with the highest address that is not greater than eip, which
	 * seems like it could be eip's function.
	 */
	int r;
	struct Sym *symtbl_begin, *symtbl_end;
	if((r=get_symtbl(envid, &symtbl_begin, &symtbl_end, 0, 0)))
	{
		if(-E_SYMTBL == r)
			return 0;
		else
			panic("%e", r);
	}

	register_t cur_cr3=0;
	uint32_t prev_pfm=0;

	if(ENVID_KERNEL != envid)
	{
		struct Env *env;
		if((r=envid2env(envid, &env, 0)))
			return 0;
		cur_cr3 = rcr3();
		lcr3(env->env_cr3);

		prev_pfm = page_fault_mode;		
		page_fault_mode = PFM_KILL;

	}

	struct Sym *closest_fn = symtbl_begin;
	struct Sym *sym;
	for(sym = symtbl_begin; sym < symtbl_end; ++sym)
	{
		if(STT_FUNC == ELF32_ST_TYPE(sym->st_info))
		{
			uint32_t addr = sym->st_value;
			if(addr <= eip && addr > closest_fn->st_value)
				closest_fn = sym;
		}
	}

	if(ENVID_KERNEL != envid)
	{
		page_fault_mode = prev_pfm;
		lcr3(cur_cr3);
	}
	return closest_fn;
}


// Given a "locn", sets envid and va accordingly.
// locn may be:
// [k:|<envnum>:]<laddr|symname>
//
// k is used to allow the distinguishment between a symbol in user vs kernel
// space, if there is no conflict using int instead of k will produce the
// same result.
//
// returns:
//   0 on success and sets envid and va,
//   -E_INVAL on multiple colons, -E_BAD_ENV, -E_BAD_SYM
int
locn_to_vaddr(char *locn, envid_t *_envid, uintptr_t *_va)
{
	int i;
	int locn_len = strlen(locn);
	int colon_idx = -1;
	char *locn_addr;
	int envnum;
	envid_t envid;
	uintptr_t va;

	assert(locn && _envid && _va);

	// detect if there is a  [k:|<envnum>:]
	for(i=0; i<locn_len; i++)
	{
		if(':' == locn[i])
		{
			if(-1 != colon_idx)
				return -E_INVAL;
			colon_idx = i;
		}
	}

	// decode the [k:|<envnum>:]
	if(-1 != colon_idx)
	{
		if('k' == locn[0])
		{
			envnum = ENVID_KERNEL;
		}
		else
		{
			locn[colon_idx] = '\0'; // TODO: is this ok to do to locn[]?
			envnum = strtol(locn, 0, 10);
			if(envnum < 0 || NENV <= envnum || !curenv
				|| envs[envnum].env_status == ENV_FREE)
				return -E_BAD_ENV;
		}
	}
	else
	{
		envnum = ENVID_KERNEL;
	}

	if(envnum != ENVID_KERNEL)
		envid = envs[envnum].env_id;
	else
		envid = ENVID_KERNEL;
	
	
	// decode the <laddr|symname>
	locn_addr = &locn[colon_idx+1];
	if(isnum(locn_addr[0]))
	{
		va = (uintptr_t) strtol(locn_addr, NULL, 16);
	}
	else
	{
		struct Sym *sym;
		int r = name_to_symbol(envid, locn_addr, &sym);
		if(r)
			return r;
		va = sym->st_value;
	}

	
	*_envid = envid;
	*_va    = va;

	return 0;
}

int
name_to_symbol(envid_t envid, char *name, struct Sym **sym)
{
	int r;
	struct Sym *symtbl_begin, *symtbl_end;
	assert(name && sym);
	if((r=get_symtbl(envid, &symtbl_begin, &symtbl_end, 0, 0)))
		return r;

	register_t cur_cr3=0;
	uint32_t prev_pfm=0;

	if(ENVID_KERNEL != envid)
	{
		struct Env *env;
		if((r=envid2env(envid, &env, 0)))
			return r;
		cur_cr3 = rcr3();
		lcr3(env->env_cr3);

		prev_pfm = page_fault_mode;		
		page_fault_mode = PFM_KILL;
	}

	for(*sym = symtbl_begin; *sym < symtbl_end; ++*sym)
	{
		if(!strcmp(name, get_symbol_name(envid, *sym)))
		{
			if(STT_FUNC != ELF32_ST_TYPE((*sym)->st_info))
				printf("WARNING: the addresses of non-function symbols (%s) are not correct\n", name);
			return 0;
		}
	}

	if(ENVID_KERNEL != envid)
	{
		page_fault_mode = prev_pfm;
		lcr3(cur_cr3);
	}

	return -E_BAD_SYM;
}

char *
get_symbol_name(envid_t envid, struct Sym *s)
{
	int r;
	char *no_name = "<no name>";
	char *symstrtbl;
	if((r=get_symtbl(envid, 0, 0, &symstrtbl, 0)))
	{
		if(-E_SYMTBL == r)
			return no_name;
		else
			panic("%e", r);
	}

	if(ENVID_KERNEL != envid)
	{
		struct Env *env;
		if((r=envid2env(envid, &env, 0)))
			return 0;
		if(env != curenv)
		{
			panic("Only able to get symbol names for symbols in current env");
			// This is because we must return the string.
			// If we changed to allocate a string and have the caller dealloc
			// we could allow this.
		}
	}

	uint32_t tbl_idx = s->st_name;
	if(0==tbl_idx)
		return no_name;
	char *n = &symstrtbl[tbl_idx];

	// If "bye\0bye\0" would be in the table, I believe it can be
	// encoded as "bye\0\0".
   // TODOl1: Watch for and handle this.

	return n;
}


//
// Support functions for print_backtrace()

uint32_t
read_uint(uintptr_t addr, uint32_t bytes_offset)
{
	return *( (uint32_t*)(addr + bytes_offset) );
}

// Return the current value of eip
uint32_t read_eip(void) __attribute__((noinline));
uint32_t
read_eip(void)
{
	/* Unfortunately, it seems one can't do a "movl %eip...". I would
	 * guess one could push %eip, read it from the stack, and add a
	 * little. The implemented method is easier to implement, however:
	 * call a fn that pulls eip from the stack.
	 */
	return read_uint(read_ebp(), sizeof(uintptr_t));
}

static void
print_backtrace_location(uint32_t eip)
{
	if(curenv && eip < KERNBASE)
		printf("%08x (%s)", curenv->env_id, curenv->env_name);
	else
		printf("kernel");
}

int
print_backtrace(struct Trapframe *tf, register_t *ebp, register_t *eip)
{
	const uint32_t max_bt_args = 5;
	uint32_t stack_depth = 0;
	uint32_t prev_ebp, ret_eip;
	int first_frame = 1;
	bool bt_user;

	// really, these are the current ebp/eip,name prev to make while loop easier
	if(ebp != NULL && eip != NULL)
	{
		prev_ebp = *ebp;
		ret_eip = *eip;
	}
	else if(tf)
	{
		// in kernel monitor because of a trap, backtrace from the trapper
		prev_ebp = tf->tf_ebp;
		ret_eip = tf->tf_eip;
	}
	else
	{
		// just in the kernel monitor, backtrace from here
		prev_ebp = read_ebp();
		ret_eip = read_eip();
	}

	bt_user = curenv && ret_eip < KERNBASE;
	printf("Backtrace in ");
	print_backtrace_location(ret_eip);
	printf(":\n");

	while(prev_ebp)
	{
		uint32_t ebp = prev_ebp;
		uint32_t eip = ret_eip;
		prev_ebp = read_uint(ebp, 0);
		ret_eip = read_uint(ebp, sizeof(uintptr_t));

		if (bt_user != (curenv && eip < KERNBASE))
		{
			printf("= Stack changes to ");
			print_backtrace_location(eip);
			printf("\n");
			bt_user = !bt_user;
		}

		printf("[%u] ", stack_depth);

		/* all frames other than the first were (very likely)
		 * created by a call instruction, which is length 5 */
		print_location(eip, first_frame);
		if(first_frame)
			first_frame = 0;
		printf("\n");

		printf(" args");
		int i;
		for(i=0; i<max_bt_args; i++)
		{
			uint32_t *arg_addr = ((uint32_t*) ebp) + 2+i;
			if(USTACKTOP <= (uintptr_t)arg_addr && (uintptr_t)arg_addr < USTACKTOP+PGSIZE)
				printf(" --------"); // this region is not mapped
			else
				printf(" %08x", *arg_addr);
		}

		printf("  eip %08x", eip);
		printf("  ebp %08x", ebp);
		printf("\n");
		stack_depth++;
	}
	return 0;
}

void
print_location(uintptr_t eip, bool first_frame)
{
#if USE_STABS
	eipinfo_t info;
	if (stab_eip(eip, &info) >= 0)
		printf("%.*s+%u  %s:%d", info.eip_fnlen, info.eip_fn, eip - info.eip_fnaddr, info.eip_file, info.eip_line);
#else
	struct Sym * sym = eip_to_fnsym(curenv->env_id, first_frame ? eip : eip - 5);
	printf("%s", get_symbol_name(curenv->env_id, sym));
#endif
}
