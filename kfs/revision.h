#ifndef __KUDOS_KFS_REVISION_H
#define __KUDOS_KFS_REVISION_H

#include <lib/types.h>
#include <kfs/bdesc.h>
#include <kfs/bd.h>

// we should have comments that describe what these functions do, and
// when they (typically) should be called. I have put the first
// part. -adlr

// revision_tail_prepare makes a DAG of all chdescs on the specified
// block and their dependencies, recursively. It then rolls back all
// those chdescs that are not on the specified BD.
// You should be careful, because this function can rollback chdescs without
// rolling back their afters.
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
	int ready_size;
	bool all_ready;
	chdesc_t ** ready;
} revision_slice_t;

/* create a new revision slice in 'new_slice' and push the slice down */
int revision_slice_create(bdesc_t * block, BD_t * owner, BD_t * target, revision_slice_t * new_slice);
void revision_slice_pull_up(revision_slice_t * slice);
/* destroy the contents of 'slice' */
void revision_slice_destroy(revision_slice_t * slice);

#endif /* __KUDOS_KFS_REVISION_H */
