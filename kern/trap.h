/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_KERN_TRAP_H
#define KUDOS_KERN_TRAP_H
#ifndef KUDOS_KERNEL
# error "This is a KudOS kernel header; user programs should not #include it"
#endif

#include <inc/trap.h>
#include <inc/mmu.h>


/* The user trap frame is always at the top of the kernel stack */
#define UTF	((struct Trapframe*)(KSTACKTOP - sizeof(struct Trapframe)))

/* The kernel's interrupt descriptor table */
extern struct Gatedesc idt[];

/*
 * Page fault modes inside kernel.
 */
#define PFM_NONE 0x0    // No page faults expected: must be a kernel bug.
			// On fault, panic.
#define PFM_KILL 0x1    // On fault, kill user process.

extern uint32_t page_fault_mode;

typedef void (*irq_handler_t)(int irq);

void handle_int_0(void);
void handle_int_1(void);
void handle_int_2(void);
void handle_int_3(void);
void handle_int_4(void);
void handle_int_5(void);
void handle_int_6(void);
void handle_int_7(void);
void handle_int_8(void);
void handle_int_9(void);
void handle_int_10(void);
void handle_int_11(void);
void handle_int_12(void);
void handle_int_13(void);
void handle_int_14(void);
void handle_int_15(void);
void handle_int_16(void);
void handle_int_17(void);
void handle_int_18(void);
void handle_int_19(void);

void handle_int_32(void);
void handle_int_33(void);
void handle_int_34(void);
void handle_int_35(void);
void handle_int_36(void);
void handle_int_37(void);
void handle_int_38(void);
void handle_int_39(void);
void handle_int_40(void);
void handle_int_41(void);
void handle_int_42(void);
void handle_int_43(void);
void handle_int_44(void);
void handle_int_45(void);
void handle_int_46(void);
void handle_int_47(void);

void handle_int_48(void);

void idt_init(void);
void print_trapframe(struct Trapframe *tf);
int request_irq(int irq, irq_handler_t handler);
void probe_irq_on(void);
int probe_irq_off(void);
void page_fault_handler(struct Trapframe *);
void reboot(void) __attribute__((noreturn));

#endif /* KUDOS_KERN_TRAP_H */
