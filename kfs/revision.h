#ifndef _REVISION_H
#define _REVISION_H

#include <kfs/bdesc.h>
#include <kfs/bd.h>

int revision_tail_prepare(bdesc_t *block, BD_t *bd);
int revision_tail_revert(bdesc_t *block, BD_t *bd);
int revision_tail_acknowledge(bdesc_t *block, BD_t *bd);

#endif // not _REVISION_H
