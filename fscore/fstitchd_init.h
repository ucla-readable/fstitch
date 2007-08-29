/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_FSTITCHD_INIT
#define __FSTITCH_FSCORE_FSTITCHD_INIT

#define ALLOW_JOURNAL 1
#define ALLOW_UNLINK 1
#define ALLOW_UNSAFE_DISK_CACHE 1
#define ALLOW_CRASHSIM 1

// Bring fstitchd's modules up.
int fstitchd_init(int nwbblocks);

#endif // not __FSTITCH_FSCORE_FSTITCHD_INIT
