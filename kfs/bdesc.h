#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

#include <inc/types.h>

/* struct BD needs bdesc, so we avoid the cycle */
struct BD;

struct bdesc;
typedef struct bdesc bdesc_t;

struct bdesc {
	struct BD * bd;
	uint32_t number, refs;
	uint16_t offset, length;
	uint8_t * data;
	uint16_t translated;
};

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(struct BD * bd, uint32_t number, uint16_t offset, uint16_t length);

/* copy a bdesc, leaving the original unchanged and giving the new bdesc reference count 0 */
bdesc_t * bdesc_copy(bdesc_t * orig);

/* prepare a bdesc to be modified, copying it if it has a reference count higher than 1 */
int bdesc_touch(bdesc_t ** bdesc);

/* prepare the bdesc to be permanently translated ("altered") by copying it if it has nonzero reference count  */
int bdesc_alter(bdesc_t ** bdesc);

/* increase the reference count of a bdesc, copying it if it is currently translated */
int bdesc_reference(bdesc_t ** bdesc);

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc);

#endif /* __KUDOS_KFS_BDESC_H */
