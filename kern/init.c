/* See COPYRIGHT for copyright information. */

#include <inc/asm.h>
#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/monitor.h>
#include <kern/console.h>
#include <kern/pmap.h>
#include <kern/kclock.h>
#include <kern/trap.h>
#include <kern/env.h>
#include <kern/sched.h>
#include <kern/picirq.h>
#include <kern/pci.h>
#include <kern/sb16.h>
#include <kern/3c509.h>
#include <kern/ne.h>
#include <kern/elf.h>
#include <kern/breakpoints.h>
#include <kern/version.h>

void
i386_init(register_t boot_eax, register_t boot_ebx)
{
	/* This assembly line does not actually generate any assembly
	 * instructions. Rather, it generates a symbol called __sizeof_Trapframe
	 * that has an absolute (i.e. not memory-related) value which is the
	 * size of a struct Trapframe. We do this because sizeof() only works
	 * inside C, but we need to know this size in entry.S to initially set
	 * up the kernel's stack. GCC will only accept inline assembly inside a
	 * function however, so we put it here. */
	__asm__ __volatile__(".globl __sizeof_Trapframe\n.set __sizeof_Trapframe, %c0" : : "i" (sizeof(struct Trapframe)));
	
	extern char edata[], end[];

	// Before doing anything else,
	// clear the uninitialized global data (BSS) section of our program.
	// This ensures that all static/global variables start out zero.
	memset(edata, 0, end-edata);

	// Initialize the console.
	// Can't call printf until after we do this!
	cons_init();
	version();

	// Reset the floppy controller to make it stop spinning
	outb(0x3F2, 0);

	set_kernel_symtbls();
	breakpoints_init();

	// Lab 2 memory management initialization functions
	i386_detect_memory(boot_eax, boot_ebx);
	printf("Initializing memory... ");
	i386_vm_init();
	page_init();
	page_check();
	printf("done.\n");

	// Lab 2 interrupt and gate descriptor initialization functions
	idt_init();

	// Lab 3 user environment initialization functions
	env_init();
	sched_init();

	// Lab 4 multitasking initialization functions
	pic_init();
	kclock_init();
	//pci_init(); // pci not in use for now

#if ENABLE_INKERNEL_INTS
	__asm__ __volatile__("sti");

	// Empty buffers to reset their interrupts
	while (cons_getc() != -1);
#endif

	sb16_init();
	el3_init();
	ne_init();

	// Should always have an idle process as first one.
	ENV_CREATE(user_idle);

	// Start kfsd and netd
	ENV_CREATE(user_netd);
	ENV_CREATE(kfs_kfsd);

	// Start init
	ENV_CREATE(user_init);

	// Schedule and run the first user environment!
	sched_yield();

	// Drop into the kernel monitor.
	while (1)
	{
		monitor(NULL);
		printf("Nothing more to do, re-invoking kernel monitor.\n");
	}
}


/*
 * Variable panicstr contains argument to first call to panic; used as flag
 * to indicate that the kernel has already called panic.
 */
static const char *panicstr;

/*
 * Panic is called on unresolvable fatal errors.
 * It prints "panic: mesg", and then enters the kernel monitor.
 */
void
_panic(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	if (panicstr)
		goto dead;
	panicstr = fmt;

	va_start(ap, fmt);
	printf("kernel panic at %s:%d: ", file, line);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);

dead:
	/* break into the kernel monitor */
	while (1)
	{
		monitor(NULL);
		printf("Restarting kernel panic monitor (%s:%d).\n", file, line);
	}
}

/* like panic, but don't */
void
_warn(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("kernel warning at %s:%d: ", file, line);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

