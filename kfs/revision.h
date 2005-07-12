#ifndef _REVISION_H
#define _REVISION_H

#include <kfs/bdesc.h>
#include <kfs/bd.h>

// we should have comments that describe what these functions do, and
// when they (typically) should be called. I have put the first
// part. -adlr

// revision_tail_prepare makes a DAG of all chdescs on the specified
// block and their dependencies, recursively. it then rolls back all
// of those chdescs, however it will not roll back any chdesc that is
// not on the BD specified. You should be careful, because this
// function can rollback blocks without rolling back their
// dependencies.
int revision_tail_prepare(bdesc_t *block, BD_t *bd);

// revision_tail_revert undoes all of the rollbacks from
// revision_tail_prepare. Specifically, it makes a DAG of all chdescs
// on the block (and their dependencies, recursively) specified and
// rolls them all forward, unless they are not on the BD specified.
int revision_tail_revert(bdesc_t *block, BD_t *bd);

// revision_tail_acknowledge commits all the chdescs that were rolled
// back with revision_tail_prepare. Specifically, it makes a DAG of
// all chdescs on the block (and their dependencies, recursively)
// specified and calls chdesc_destroy() on them. For chdescs not on
// BD, it calls chdesc_apply() on them.
int revision_tail_acknowledge(bdesc_t *block, BD_t *bd);

// revision_satisfy_external_deps loops. Each iteration it finds an
// external dependency and, if there is one, it tells the owner to
// sync the block containing that chdesc. When there are no external
// deps, it returns.
int revision_satisfy_external_deps(bdesc_t *block, BD_t *bd);

#endif // not _REVISION_H
