/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_BSD_PTABLE_BD_H
#define __FSTITCH_FSCORE_BSD_PTABLE_BD_H

#include <fscore/bd.h>

#define  DKTYPENAMES
#define  FSTYPENAMES

void * bsd_ptable_init(BD_t * bd);
int bsd_ptable_count(void * info);
uint8_t bsd_ptable_type(void * info, int index);
BD_t * bsd_ptable_bd(void * info, int index);
void bsd_ptable_free(void * info);

#endif /* __FSTITCH_FSCORE_BSD_PTABLE_BD_H */
