/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef FSTITCH_LIB_STDARG_H
#define FSTITCH_LIB_STDARG_H

#include <linux/kernel.h>
// kernel va_arg does not allow types less than sizeof(int), so cast these to
// int and then back to their original type
#undef va_arg
// TODO: kernel assert() is not fully featured and causes the asserts in va_arg to not compile:
//#define va_arg(ap, type) (assert(sizeof(int) >= sizeof(type)), (type) __builtin_va_arg(ap, int))
#define va_arg(ap, type) (type) __builtin_va_arg(ap, int)

#endif // !FSTITCH_LIB_STDARG_H
