#if defined(__linux__)
#define _GNU_SOURCE /* for vasprintf() */
#endif

#include <lib/kdprintf.h>

#include <linux/kernel.h>

int
kdprintf(int fd, const char *fmt, ...)
{
	va_list ap;
	int r;

	if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
		printk(KERN_ERR "Unknown fd %d passed to %s(fd)\n", fd, __FUNCTION__);
	va_start(ap, fmt);
	r = vprintk(fmt, ap);
	va_end(ap);

	return r;
}
