#ifndef _REVISION_H
#define _REVISION_H

#include <kfs/bdesc.h>
#include <kfs/bd.h>

// we should have comments that describe what these functions do, and
// when they (typically) should be called. I have put the first
// part. -adlr

// revision_tail_prepare makes a DAG of all chdescs on the specified
// block and their dependencies, recursively. It then rolls back all
// those chdescs that are not on the specified BD.
// You should be careful, because this function can rollback chdescs without
// rolling back their dependents.
int revision_tail_prepare(bdesc_t *block, BD_t *bd);
int revision_tail_prepare_stamp(bdesc_t * block, uint32_t stamp);

// revision_tail_revert undoes all of the rollbacks from
// revision_tail_prepare. Specifically, it makes a DAG of all chdescs
// on the block (and their dependencies, recursively) specified and
// rolls all chdescs forward that are not on the specified BD.
int revision_tail_revert(bdesc_t *block, BD_t *bd);
int revision_tail_revert_stamp(bdesc_t * block, uint32_t stamp);

// revision_tail_acknowledge commits all the chdescs that were rolled
// back with revision_tail_prepare. Specifically, it makes a DAG of
// all chdescs on the block (and their dependencies, recursively)
// specified and calls chdesc_destroy() on them. For chdescs not on
// BD, it calls chdesc_apply() on them.
int revision_tail_acknowledge(bdesc_t *block, BD_t *bd);

typedef struct revision_slice {
	BD_t * owner;
	BD_t * target;
	int full_size, ready_size;
	chdesc_t ** full;
	chdesc_t ** ready;
} revision_slice_t;

revision_slice_t * revision_slice_create(bdesc_t * block, BD_t * owner, BD_t * target, bool external);
void revision_slice_push_down(revision_slice_t * slice);
void revision_slice_pull_up(revision_slice_t * slice);
void revision_slice_destroy(revision_slice_t * slice);

#endif // not _REVISION_H
