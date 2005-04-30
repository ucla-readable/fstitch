#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

#include <inc/types.h>

struct bdesc;
typedef struct bdesc bdesc_t;

struct datadesc;
typedef struct datadesc datadesc_t;

#include <kfs/bd.h>
#include <kfs/chdesc.h>

struct datadesc {
	uint8_t * data;
	uint32_t refs;
	chdesc_t * changes;
	uint16_t length;
};

struct bdesc {
	uint32_t number, refs;
	datadesc_t * ddesc;
};

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(BD_t * bd, uint32_t number, uint16_t length);

/* prepare a bdesc's data to be modified, copying its data if it is currently shared with another bdesc */
int bdesc_touch(bdesc_t * bdesc);

/* prepare the bdesc to be permanently translated ("altered") by copying it if it has nonzero reference count */
int bdesc_alter(bdesc_t ** bdesc);

/* increase the reference count of a bdesc, copying it if it is currently translated (but sharing the data) */
int bdesc_retain(bdesc_t ** bdesc);

/* free a bdesc if it has zero reference count */
void bdesc_drop(bdesc_t ** bdesc);

/* decrease the bdesc reference count but do not free it even if it reaches 0 */
void bdesc_forget(bdesc_t ** bdesc);

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc);

/* a function for caches and cache-like modules to use for bdesc overwriting */
int bdesc_overwrite(bdesc_t * cached, bdesc_t * written);

/* compares two bdescs' blocknos for qsort */
int bdesc_blockno_compare(const void * b1, const void * b2);

#endif /* __KUDOS_KFS_BDESC_H */
