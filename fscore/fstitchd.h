/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_FSTITCHD
#define __FSTITCH_FSCORE_FSTITCHD

// When a shutdown_module callback will be made
#define SHUTDOWN_PREMODULES  1 // before modules are deconstructed
#define SHUTDOWN_POSTMODULES 2 // after modules are deconstructed

typedef void (*fstitchd_shutdown_module)(void * arg);
int _fstitchd_register_shutdown_module(const char * name, fstitchd_shutdown_module fn, void * arg, int when);
#define fstitchd_register_shutdown_module(fn, arg, when) _fstitchd_register_shutdown_module(#fn, fn, arg, when)

void fstitchd_request_shutdown(void);
int fstitchd_is_running(void);

#endif // not __FSTITCH_FSCORE_FSTITCHD
