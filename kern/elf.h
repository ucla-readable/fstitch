#ifndef KUDOS_KERN_ELF_H
#define KUDOS_KERN_ELF_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/elf.h>

//
// Use and resolve symbols

// Syntax of "locn", useful for printing help
extern const char locn_syntax[];


// Given the address of an instruction, return the current fn's symbol
struct Sym* eip_to_fnsym(envid_t envid, uint32_t eip);

char * get_symbol_name(envid_t envid, struct Sym *s);

// Set *sym by looking up name for envid
int name_to_symbol(envid_t envid, char *name, struct Sym **sym);

// Set envid and va for "locn". See definition comments for more info.
int locn_to_vaddr(char *locn, envid_t *_envid, uintptr_t *_va);

// Given envid, set the symtbl and symstrtbl beginnings and endings that != 0.
// Use envid ENVID_KERNEL to use the kernel's elf.
int get_symtbl(envid_t envid,
					struct Sym **_symtbl_begin, struct Sym **_symtbl_end,
					char **_symstrtbl_begin,    char **_symstrtbl_end);

int  print_backtrace(struct Trapframe *tf, register_t *ebp, register_t *eip);

void print_location(uintptr_t eip, bool first_frame);


//
// Set symbol tables

void set_kernel_symtbls(void);

int set_symtbls(envid_t,
					 struct Sym *symtbl, size_t symtbl_size,
					 char *symstrtbl,    size_t symstrtbl_size);


#endif   // !KUDOS_KERN_ELF_H
