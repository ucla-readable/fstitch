#ifndef __KUDOS_KFS_REVISION_H
#define __KUDOS_KFS_REVISION_H

/* In-place revision tail code is the "classic" way of doing it: roll the data
 * back on the actual copy of the block in memory. Turning this off uses a copy
 * of the block data when rolling a block back, thus avoiding the need to roll
 * it forward again. Since we already make a copy in many terminal BDs, this can
 * be more efficient when there are many rollbacks. */
#define REVISION_TAIL_INPLACE 0

#include <kfs/bdesc.h>
#include <kfs/bd.h>

int revision_init(void);

#if REVISION_TAIL_INPLACE
/* roll back change descriptors on the passed block which are not yet ready to
 * go to disk, constructing the previous version of the block in place */
int revision_tail_prepare(bdesc_t * block, BD_t * bd);
#else
/* roll back change descriptors on the passed block which are not yet ready to
 * go to disk, constructing the previous version of the block in the buffer */
int revision_tail_prepare(bdesc_t * block, BD_t * bd, uint8_t * buffer);
#endif

/* undo everything done by revision_tail_prepare() above */
int revision_tail_revert(bdesc_t * block, BD_t * bd);

/* this function calls chdesc_satisfy() on all the non-rolled back change
 * descriptors on the block, and rolls the others forward again */
int revision_tail_acknowledge(bdesc_t * block, BD_t * bd);

#if __KERNEL__
/* this function marks the non-rolled back change descriptors as "in flight" and
 * rolls the others forward again */
int revision_tail_inflight_ack(bdesc_t * block, BD_t * bd);

/* this function schedules a flight slot to receive the interrupt-time
 * notification that a block has been written to the controller or (with
 * sufficient hardware support) to the disk */
int revision_tail_schedule_flight(void);

/* this function cancels the scheduled flight set up by revision_tail_schedule_flight() */
void revision_tail_cancel_flight(void);

/* this function returns true iff there are any scheduled or holding flights */
int revision_tail_flights_exist(void);

/* this function should be called at interrupt time to notify the system that a
 * block has been written to the controller or (with NCQ) to the disk */
void revision_tail_request_landing(bdesc_t * block);

/* this function processes pending landing requests */
void revision_tail_process_landing_requests(void);
/* this function waits for landing requests to be set up */
void revision_tail_wait_for_landing_requests(void);
#endif /* __KERNEL__ */

/* ---- Revision slices ---- */

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
