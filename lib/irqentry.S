#include <inc/mmu.h>
#include <inc/pmap.h>

// Asynchronous IRQ handler entry point. This is where we ask the kernel to
// redirect us to whenever an IRQ happens that we want to be notified about (see
// the call to sys_set_irq_handler in lib/irq.c).
//
// This code is similar to that found in pfentry.S. Refer to that file for more
// information. However, note that this code does not need to be able to switch
// stacks, and it has an IRQ number instead of a fault address and error code.
//
// The kernel pushes the following stack frame onto the user stack for us:
//
//	IRQ-time eip
//	IRQ-time eflags
//	[ 3 spare words ]
//	IRQ number		<-- %esp

.text
.globl _irq_upcall
_irq_upcall:
	// Save the caller-saved registers.
	movl %eax, 12(%esp)
	movl %ecx, 8(%esp)
	movl %edx, 4(%esp)

	// Call the C IRQ handler.
	call _irq_handler

	// Restore the caller-saved registers.
	movl 12(%esp), %eax
	movl 8(%esp), %ecx
	movl 4(%esp), %edx

	// Adjust the stack pointer.
	addl $16, %esp

	// Restore eflags from the stack.
	popfl

	// Return to where we were before the IRQ occurred.
	ret
