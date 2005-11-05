
#include <inc/lib.h>

char *argv0;

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
#ifdef KUDOS
		printf_c(
#else
		printf(
#endif
			"%s: ", argv0);
#ifdef KUDOS
	printf_c(
#else
	printf(
#endif
		"user panic in %s at %s:%d: ", binaryname, file, line);
	vprintf(fmt, ap);
#ifdef KUDOS
	printf_c(
#else
	printf(
#endif
		"\n");

	// Cause a breakpoint exception
	for (;;)
		asm volatile("int3");
}

