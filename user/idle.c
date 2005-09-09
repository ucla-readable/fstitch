// idle loop

#include <inc/x86.h>
#include <inc/lib.h>

void
umain(void)
{
	binaryname = "idle";
	sys_env_set_name(0, "idle");

	// Loop forever, simply trying to yield to a different environment.
	// Instead of busy-waiting like this, a better way would be to use the
	// processor's HLT instruction to cause the processor to stop executing
	// until the next interrupt - doing so allows the processor to conserve
	// power more effectively.
	while (1) {
		sys_yield();

		// Break into the KudOS kernel monitor after each sys_yield().
		// A real, "production" OS of course would NOT do this -
		// it would just endlessly loop waiting for hardware interrupts
		// to cause other environments to become runnable.
		breakpoint();
	}
}

