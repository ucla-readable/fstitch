#ifndef KUDOS_LIB_STDARG_H
#define KUDOS_LIB_STDARG_H

#if defined(KUDOS)

#include <inc/stdarg.h>

#elif defined(UNIXUSER)

#include <stdarg.h>
// unix va_arg does not allow types less than sizeof(int), so cast these to
// int and then back to their original type
#undef va_arg
#define va_arg(ap, type) (assert(sizeof(int) >= sizeof(type)), (type) __builtin_va_arg(ap, int))

#elif defined(__KERNEL__)

#include <linux/kernel.h>
// kernel va_arg does not allow types less than sizeof(int), so cast these to
// int and then back to their original type
#undef va_arg
// TODO: kernel assert() is not fully featured and causes the asserts in va_arg to not compile:
//#define va_arg(ap, type) (assert(sizeof(int) >= sizeof(type)), (type) __builtin_va_arg(ap, int))
#define va_arg(ap, type) (type) __builtin_va_arg(ap, int)

#else
#error Unknown target system
#endif

#endif // !KUDOS_LIB_STDARG_H
