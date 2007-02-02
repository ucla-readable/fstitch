#ifndef __KUDOS_KFS_REVISION_H
#define __KUDOS_KFS_REVISION_H

#include <lib/types.h>
#include <kfs/bdesc.h>
#include <kfs/bd.h>

/* these functions roll back change descriptors on the passed blocks which are
 * not yet ready to go to disk (the non-stamp case) or do not yet have the stamp */
int revision_tail_prepare(bdesc_t *block, BD_t *bd);
int revision_tail_prepare_stamp(bdesc_t * block, uint32_t stamp);

/* these functions undo everything done by the _prepare functions above */
int revision_tail_revert(bdesc_t *block, BD_t *bd);
int revision_tail_revert_stamp(bdesc_t * block, uint32_t stamp);

/* this function calls chdesc_satisfy() on all the non-rolled back change
 * descriptors on the block, and rolls the others forward again */
int revision_tail_acknowledge(bdesc_t *block, BD_t *bd);

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

/* ---- Revision slices: library functions for use inside barrier zones ---- */

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
