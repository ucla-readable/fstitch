#ifndef __KUDOS_KFS_JOURNAL_BD_H
#define __KUDOS_KFS_JOURNAL_BD_H

#include <inc/types.h>
#include <kfs/bd.h>

/* journal_bd modules are initially created as passthrough, read-only devices */
BD_t * journal_bd(BD_t * disk);

/* ...and they are fully activated upon the addition of a journal device */
int journal_bd_set_journal(BD_t * bd, BD_t * journal);

#endif /* __KUDOS_KFS_JOURNAL_BD_H */
