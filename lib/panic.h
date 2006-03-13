#ifndef KUDOS_LIB_PANIC_H
#define KUDOS_LIB_PANIC_H

#if defined(__KERNEL__)

#include <linux/kernel.h>
// Though kernel.h does not define warn or _warn, we use use neither

#else

#include <lib/stdio.h>
#include <lib/assert.h>

void _warn(const char*, int, const char*, ...);
void _panic(const char*, int, const char*, ...) __attribute__((noreturn));

#define warn(...) _warn(__FILE__, __LINE__, __VA_ARGS__)
#define panic(...) _panic(__FILE__, __LINE__, __VA_ARGS__)

#endif

// static_assert(x) will generate a compile-time error if 'x' is false.
#define static_assert(x)	switch (x) case 0: case (x):

#endif /* !KUDOS_LIB_PANIC_H */
