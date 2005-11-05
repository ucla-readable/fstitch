// XXX change this to use printfmt properly,
// with no string length limitation.

#include <inc/lib.h>

int
kdprintf(int fd, const char *fmt, ...)
{
	char buf[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return write(fd, buf, strlen(buf));
}
 
// The same as kdprintf(), but if fd 1 is not mapped prints to console
int
printf(const char *fmt, ...)
{
 	va_list ap;
	struct Fd* stdout;
 
 	va_start(ap, fmt);
	if(0 <= fd_lookup(1, &stdout)) {
		// fd 1 is mapped, do as kdprintf() does
		char buf[256];

		vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		return write(1, buf, strlen(buf));
	} else {
		// fd 1 is not mapped, print to the console just as printf_c() does
		int cnt;

		cnt = vprintf(fmt, ap);
		va_end(ap);
		return cnt;
	}
 }
