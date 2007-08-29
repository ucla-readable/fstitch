/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_JOURNAL_BD_H
#define __FSTITCH_FSCORE_JOURNAL_BD_H

#include <fscore/bd.h>

/* journal_bd modules are initially created as passthrough, read-only devices */
BD_t * journal_bd(BD_t * disk, uint8_t only_metadata);

/* ...and they are fully activated upon the addition of a journal device */
int journal_bd_set_journal(BD_t * bd, BD_t * journal);

/* Add and remove "holds". While there is a hold no journal_bd will stop
 * a transaction. */
void journal_bd_add_hold(void);
void journal_bd_remove_hold(void);

#endif /* __FSTITCH_FSCORE_JOURNAL_BD_H */
