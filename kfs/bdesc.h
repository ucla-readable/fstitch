#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

#include <inc/types.h>

/* struct BD needs bdesc, so we avoid the cycle */
struct BD;

struct bdesc;
typedef struct bdesc bdesc_t;

struct datadesc;
typedef struct datadesc datadesc_t;

struct datadesc {
	uint8_t * data;
	uint32_t refs;
};

struct bdesc {
	struct BD * bd;
	uint32_t number, refs;
	uint16_t offset, length;
	struct datadesc * ddesc;
	/* this field is very likely binary */
	uint16_t translated;
};

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(struct BD * bd, uint32_t number, uint16_t offset, uint16_t length);

/* prepare a bdesc's data to be modified, copying its data if it is currently shared with another bdesc */
int bdesc_touch(bdesc_t * bdesc);

/* prepare the bdesc to be permanently translated ("altered") by copying it if it has nonzero reference count */
int bdesc_alter(bdesc_t ** bdesc);

/* increase the reference count of a bdesc, copying it if it is currently translated (but sharing the data) */
int bdesc_retain(bdesc_t ** bdesc);

/* free a bdesc if it has zero reference count */
void bdesc_drop(bdesc_t ** bdesc);

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc);

#endif /* __KUDOS_KFS_BDESC_H */
