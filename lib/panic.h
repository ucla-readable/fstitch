#ifndef KUDOS_LIB_PANIC_H
#define KUDOS_LIB_PANIC_H

#if defined(__KERNEL__)

#warning warn and panic not yet implemented
#define warn(...) do { } while(0)
#define panic(...) do { } while(0)

#else

#include <stdio.h>
#include <assert.h>

void _warn(const char*, int, const char*, ...);
void _panic(const char*, int, const char*, ...) __attribute__((noreturn));

#define warn(...) _warn(__FILE__, __LINE__, __VA_ARGS__)
#define panic(...) _panic(__FILE__, __LINE__, __VA_ARGS__)

// static_assert(x) will generate a compile-time error if 'x' is false.
#define static_assert(x)	switch (x) case 0: case (x):

#endif

#endif /* !KUDOS_LIB_PANIC_H */
