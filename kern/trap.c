#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>
#include <inc/config.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/irq.h>
#include <kern/sb16.h>
#include <kern/3c509.h>
#include <kern/elf.h>


uint32_t page_fault_mode = PFM_NONE;
static struct Taskstate ts;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { {0}, };
struct Pseudodesc idt_pd =
{
	0, sizeof(idt) - 1, (unsigned long) idt,
};


#if ENABLE_ENV_FP
// Set __sizeof_Trapframe_fp
void
static_make_sizeof_tf_fp(void)
{
	struct Trapframe tf;
	/* This assembly line does not actually generate any assembly
    * instructions. Rather, it generates a symbol called
    * __sizeof_Trapframe_fp that has an absolute (i.e. not
    * memory-related) value which is the size of a struct
    * Trapframe.tf_fp. We do this because sizeof() only works inside
    * C, but we need to know this size in trapentry.S. GCC will only
    * accept inline assembly inside a function however, so we put it here.
	 */
	__asm__ __volatile__(".globl __sizeof_Trapframe_fp\n.set __sizeof_Trapframe_fp, %c0"
								: : "i" (sizeof(tf.tf_fp)));
}
#endif

#include <kern/stabs.h>
#define USE_STABS 1

static const char *trapname(int trapno)
{
	static const char *excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == IRQ_OFFSET)
		return "Timer interrupt";
	if (trapno == T_SYSCALL)
		return "System call";

	return "(unknown trap)";
}


#if ENABLE_INKERNEL_INTS
#define ISTRAP 1
#else
#define ISTRAP 0
#endif

void
idt_init(void)
{
	extern struct Segdesc gdt[];	

	SETGATE(idt[0], ISTRAP, GD_KT, &handle_int_0, 3);
	SETGATE(idt[1], ISTRAP, GD_KT, &handle_int_1, 3);
	SETGATE(idt[2], ISTRAP, GD_KT, &handle_int_2, 3);
	SETGATE(idt[3], ISTRAP, GD_KT, &handle_int_3, 3);
	SETGATE(idt[4], ISTRAP, GD_KT, &handle_int_4, 3);
	SETGATE(idt[5], ISTRAP, GD_KT, &handle_int_5, 3);
	SETGATE(idt[6], ISTRAP, GD_KT, &handle_int_6, 3);
	SETGATE(idt[7], ISTRAP, GD_KT, &handle_int_7, 3);
	SETGATE(idt[8], ISTRAP, GD_KT, &handle_int_8, 3);
	SETGATE(idt[9], ISTRAP, GD_KT, &handle_int_9, 3);
	SETGATE(idt[10], ISTRAP, GD_KT, &handle_int_10, 3);
	SETGATE(idt[11], ISTRAP, GD_KT, &handle_int_11, 3);
	SETGATE(idt[12], ISTRAP, GD_KT, &handle_int_12, 3);
	SETGATE(idt[13], ISTRAP, GD_KT, &handle_int_13, 3);
	SETGATE(idt[14], ISTRAP, GD_KT, &handle_int_14, 0); /* T_PGFLT */
	SETGATE(idt[15], ISTRAP, GD_KT, &handle_int_15, 3);
	SETGATE(idt[16], ISTRAP, GD_KT, &handle_int_16, 3);
	SETGATE(idt[17], ISTRAP, GD_KT, &handle_int_17, 3);
	SETGATE(idt[18], ISTRAP, GD_KT, &handle_int_18, 3);
	SETGATE(idt[19], ISTRAP, GD_KT, &handle_int_19, 3);
	
	SETGATE(idt[32], 0, GD_KT, &handle_int_32, 0);
	SETGATE(idt[33], 0, GD_KT, &handle_int_33, 0);
	SETGATE(idt[34], 0, GD_KT, &handle_int_34, 0);
	SETGATE(idt[35], 0, GD_KT, &handle_int_35, 0);
	SETGATE(idt[36], 0, GD_KT, &handle_int_36, 0);
	SETGATE(idt[37], 0, GD_KT, &handle_int_37, 0);
	SETGATE(idt[38], 0, GD_KT, &handle_int_38, 0);
	SETGATE(idt[39], 0, GD_KT, &handle_int_39, 0);
	SETGATE(idt[40], 0, GD_KT, &handle_int_40, 0);
	SETGATE(idt[41], 0, GD_KT, &handle_int_41, 0);
	SETGATE(idt[42], 0, GD_KT, &handle_int_42, 0);
	SETGATE(idt[43], 0, GD_KT, &handle_int_43, 0);
	SETGATE(idt[44], 0, GD_KT, &handle_int_44, 0);
	SETGATE(idt[45], 0, GD_KT, &handle_int_45, 0);
	SETGATE(idt[46], 0, GD_KT, &handle_int_46, 0);
	SETGATE(idt[47], 0, GD_KT, &handle_int_47, 0);
	
	SETGATE(idt[48], ISTRAP, GD_KT, &handle_int_48, 3); /* T_SYSCALL */

	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	ts.ts_esp0 = KSTACKTOP;
	ts.ts_ss0 = GD_KD;

	// Initialize the TSS field of the gdt.
	gdt[GD_TSS >> 3] = SEG16(STS_T32A, (uint32_t) (&ts),
					sizeof(struct Taskstate), 0);
	gdt[GD_TSS >> 3].sd_s = 0;

	// Load the TSS
	ltr(GD_TSS);

	// Load the IDT
	asm volatile("lidt idt_pd+2");
}


void
print_trapframe(struct Trapframe *tf)
{
#if CLASS_TF_FORMAT
	printf("TRAP frame at %p\n", tf);
	printf("  edi  0x%08x\n", tf->tf_edi);
	printf("  esi  0x%08x\n", tf->tf_esi);
	printf("  ebp  0x%08x\n", tf->tf_ebp);
	printf("  oesp 0x%08x\n", tf->tf_oesp);
	printf("  ebx  0x%08x\n", tf->tf_ebx);
	printf("  edx  0x%08x\n", tf->tf_edx);
	printf("  ecx  0x%08x\n", tf->tf_ecx);
	printf("  eax  0x%08x\n", tf->tf_eax);
	printf("  es   0x----%04x\n", tf->tf_es);
	printf("  ds   0x----%04x\n", tf->tf_ds);
	printf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	printf("  err  0x%08x\n", tf->tf_err);
	printf("  eip  0x%08x\n", tf->tf_eip);
	printf("  cs   0x----%04x\n", tf->tf_cs);
	printf("  flag 0x%08x\n", tf->tf_eflags);
	printf("  esp  0x%08x\n", tf->tf_esp);
	printf("  ss   0x----%04x\n", tf->tf_ss);
#else
  	printf("TRAP frame at %p\n", tf);

	printf("  esp  0x%08x", tf->tf_esp);
	printf("  ebp  0x%08x", tf->tf_ebp);
	printf("  oesp 0x%08x\n", tf->tf_oesp);

	printf("  eax  0x%08x", tf->tf_eax);
	printf("  ebx  0x%08x", tf->tf_ebx);
	printf("  ecx  0x%08x", tf->tf_ecx);
	printf("  edx  0x%08x\n", tf->tf_edx);

	printf("  cs   0x----%04x", tf->tf_cs);
	printf("  ds   0x----%04x", tf->tf_ds);
	printf("  es   0x----%04x", tf->tf_es);
	printf("  ss   0x----%04x\n", tf->tf_ss);

	printf("  edi  0x%08x", tf->tf_edi);
	printf("  esi  0x%08x", tf->tf_esi);
	printf("  flag 0x%08x\n", tf->tf_eflags);

	printf("  err  0x%08x", tf->tf_err);
	printf("  trap 0x%08x  %s\n", tf->tf_trapno, trapname(tf->tf_trapno));

	printf("  eip  0x%08x  ", tf->tf_eip);
	envid_t envid;
	if(curenv && tf->tf_eip < KERNBASE) {
		envid = curenv->env_id;
		printf("%d:", ENVX(curenv->env_id));
	} else {
		envid = ENVID_KERNEL;
		printf("%c:", 'k');
	}
	print_location(tf->tf_eip, 1);
	printf("\n");
#endif
}

void
trap(struct Trapframe *tf)
{
	if(tf->tf_cs != GD_KT)
		curenv->env_tsc += read_tsc() - env_tsc;
	
	/* IRQ - we want to allow this to happen in the kernel; these must be asynchronous! */
	/* note that user environments are not allowed to invoke these interrupt numbers */
	if(IRQ_OFFSET <= tf->tf_trapno && tf->tf_trapno < IRQ_OFFSET + MAX_IRQS)
	{
		int irq = tf->tf_trapno - IRQ_OFFSET;
		/* slave auto EOI may not work */
		if(7 < irq)
			outb(IO_PIC2, 0x60 + (irq & 7));
		
		if(irq == 0)
		{
			jiffies++;
			/* clock interrupt in user mode */
			if(tf->tf_cs != GD_KT)
				sched_yield();
			/* ignore clock interrupt in kernel */
		}
		else
			dispatch_irq(irq);
		
	}
	else if(tf->tf_trapno == T_BRKPT)
	{
		/* save the state in case we will be reading it */
		if(tf->tf_cs != GD_KT)
			curenv->env_tf = *tf;
		monitor(tf);
	}
	else if(tf->tf_trapno == T_DEBUG)
	{
		monitor(tf);
		ldr6(0); // clear debug flags for future breakpoints
		tf->tf_eflags |= FL_RF; // in case breakpoint on eip's inst, set RF flag
	}
	else if(tf->tf_trapno == T_PGFLT)
		page_fault_handler(tf);
	else if(tf->tf_trapno == T_SYSCALL)
	{
		tf->tf_eax = syscall(tf->tf_eax, tf->tf_edx, tf->tf_ecx, tf->tf_ebx, tf->tf_edi, tf->tf_esi);
		if(jiffies - curenv->env_jiffies > 0)
			/* our timeslice expired during the system call */
			sched_yield();
	}
	else if(tf->tf_trapno == T_TSS)
	{
		print_trapframe(tf);
		env_destroy(curenv);
		/* does not return */
	}
	else
	{
		/* the user process or the kernel has a bug */
		print_trapframe(tf);
		print_backtrace(tf, NULL, NULL);
		if(tf->tf_cs == GD_KT)
			panic("unhandled trap in kernel");
		else
			env_destroy(curenv);
		/* does not return */
	}
	
	/* if there are pending IRQs to be delivered to user environments,
	 * env_dispatch_irqs() sets curenv to one of them and pushes a signal
	 * handler onto the target environment's stack */
	env_dispatch_irqs();
	
	env_tsc = read_tsc();
	return;
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults based on the current value of
	// 'page_fault_mode' (which is either PFM_NONE or PFM_KILL).
	
	if(tf->tf_cs == GD_KT)
	{
		if(page_fault_mode != PFM_NONE)
		{
			printf("[%08x] PFM_KILL va %08x ip %08x\n", curenv->env_id, fault_va, tf->tf_eip);
			print_trapframe(tf);
			print_backtrace(tf, NULL, NULL);
			page_fault_mode = PFM_NONE;
			env_destroy(curenv);
			return;
		}
		print_trapframe(tf);
		print_backtrace(tf, NULL, NULL);
		panic("unhandled kernel page fault va 0x%08x ip 0x%08x", fault_va, tf->tf_eip);
	}

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.  If
	// there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack, or the exception stack overflows,
	// then destroy the environment that caused the fault.
	//
	// Hint:
	//   page_fault_mode and env_run() are useful here.
	//   How should you modify 'tf'?
	
	if(curenv->env_pgfault_upcall)
	{
		uint32_t old_fault_mode = page_fault_mode;
		uint32_t * uxstack = (uint32_t *) tf->tf_esp;
		
		/* note that we don't need TRUP() because of this check */
		if(tf->tf_esp < UXSTACKTOP - PGSIZE || tf->tf_esp > UXSTACKTOP)
			uxstack = (uint32_t *) UXSTACKTOP;
		
		page_fault_mode = PFM_KILL;
		
		uxstack[-6] = tf->tf_eip;
		uxstack[-7] = tf->tf_eflags;
		uxstack[-8] = tf->tf_esp;
		uxstack[-9] = tf->tf_err;
		uxstack[-10] = fault_va;
		
		tf->tf_esp = (register_t) &uxstack[-10];
		tf->tf_eip = curenv->env_pgfault_upcall;
		
		page_fault_mode = old_fault_mode;
		return;
	}

	// Destroy the environment that caused the fault.
	printf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	print_backtrace(tf, NULL, NULL);
	env_destroy(curenv);
}


void
reboot(void)
{
	/* first try the "right" way */
	outb(0x92, 0x3);
	
	/* then cause a triple fault */
	__asm__ __volatile__("cli");
	idt[3].gd_p = 0;
	idt[8].gd_p = 0;
	idt[11].gd_p = 0;
	__asm__ __volatile__("int3");
	panic("failed to reboot");
}
