#ifndef KUDOS_KERN_BREAKPOINTS_H
#define KUDOS_KERN_BREAKPOINTS_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/types.h>

struct Trapframe;

int breakpoints_print(struct Trapframe *tf);
int breakpoints_set(envid_t envid, uint32_t reg, uintptr_t addr, bool mem_exec,
					bool w_rw, int len);
int breakpoints_active(int32_t reg, bool active, bool callerisbreakpoint);
int breakpoints_ss_active(struct Trapframe *tf, bool active);

void breakpoints_sched(envid_t envid);
void breakpoints_init(void);

#endif	// !KUDOS_KERN_BREAKPOINTS_H
