#if defined(KUDOS)
#include <inc/lib.h>
#elif defined(UNIXUSER)
#include <stdio.h>
#include <stdarg.h>
#else
#error Unknown target system
#endif

char *argv0;

#ifdef UNIXUSER
#define printf_c printf
const char binaryname[] = "?";
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
	for (;;)
		asm volatile("int3");
}

