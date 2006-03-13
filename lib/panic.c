#if defined(KUDOS)
#include <inc/lib.h>
#elif defined(UNIXUSER)
#include <stdarg.h>
#elif defined(__KERNEL__)
#erorr Kernel not yet supported
#else
#error Unknown target system
#endif

#include <lib/assert.h>
#include <lib/stdio.h>

char *argv0;

#if defined(UNIXUSER)
#define printf_c printf
const char binaryname[] = "?";
#elif defined(__KERNEL__)
#define printf_c printf
const char binaryname[] = "kfsd";
#endif

/*
 * Panic is called on unresolvable fatal errors.
 * It prints "panic: <message>", then causes a breakpoint exception,
 * which causes KudOS to enter the KudOS kernel monitor.
 */
void
_panic(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	va_start(ap, fmt);

	// Print the panic message
	if (argv0)
		printf_c("%s: ", argv0);
	printf_c("user panic in %s at %s:%d: ", binaryname, file, line);
	vprintf(fmt, ap);
	printf_c("\n");

	// Cause a breakpoint exception
#if defined(KUDOS)	
	for (;;)
		asm volatile("int3");
#elif defined(UNIXUSER) || defined(__KERNEL__)
	assert(0);
#else
#error Unknown target system
#endif
}

