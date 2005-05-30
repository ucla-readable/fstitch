#ifndef KUDOS_KERN_STABS_H
#define KUDOS_KERN_STABS_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

typedef struct eipinfo {
	const char *eip_fn;
	int eip_fnlen;
	uintptr_t eip_fnaddr;
	const char *eip_file;
	int eip_line;
} eipinfo_t;

int stab_eip(uintptr_t vaddr, eipinfo_t *info);

#endif // !KUDOS_KERN_STABS_H
