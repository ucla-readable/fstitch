#ifndef KUDOS_LIB_PANIC_H
#define KUDOS_LIB_PANIC_H

#include <linux/kernel.h>

// static_assert(x) will generate a compile-time error if 'x' is false.
#define static_assert(x)	switch (x) case 0: case (x):

#endif /* !KUDOS_LIB_PANIC_H */
