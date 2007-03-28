#ifndef __KUDOS_KFS_JOURNAL_BD_H
#define __KUDOS_KFS_JOURNAL_BD_H

#include <kfs/bd.h>

/* journal_bd modules are initially created as passthrough, read-only devices */
BD_t * journal_bd(BD_t * disk);

/* ...and they are fully activated upon the addition of a journal device */
int journal_bd_set_journal(BD_t * bd, BD_t * journal);

/* Add and remove "holds". While there is a hold no journal_bd will stop
 * a transaction. */
void journal_bd_add_hold(void);
void journal_bd_remove_hold(void);

#endif /* __KUDOS_KFS_JOURNAL_BD_H */
