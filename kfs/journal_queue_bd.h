#ifndef __KUDOS_KFS_JOURNAL_QUEUE_BD_H
#define __KUDOS_KFS_JOURNAL_QUEUE_BD_H

#include <inc/types.h>
#include <inc/hash_map.h>
#include <kfs/bd.h>

BD_t * journal_queue_bd(BD_t * disk);

bool journal_queue_detect(BD_t * bd);
int journal_queue_release(BD_t * bd);
int journal_queue_hold(BD_t * bd);
int journal_queue_passthrough(BD_t * bd);
const hash_map_t * journal_queue_blocklist(BD_t * bd);

#endif /* __KUDOS_KFS_JOURNAL_QUEUE_BD_H */
