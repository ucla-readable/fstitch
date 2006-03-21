#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

#include <lib/types.h>

struct bdesc;
typedef struct bdesc bdesc_t;

struct datadesc;
typedef struct datadesc datadesc_t;

#include <kfs/bd.h>
#include <kfs/chdesc.h>
#include <kfs/blockman.h>

struct datadesc {
	uint8_t * data;
	uint32_t ref_count;
	chdesc_t * changes;
	blockman_t * manager;
	uint32_t managed_number;
	uint16_t length;
};

struct bdesc {
	uint32_t number;
	uint32_t ref_count;
	uint32_t ar_count;
	bdesc_t * ar_next;
	datadesc_t * ddesc;
	uint16_t count;
};

/* allocate a new bdesc */
/* the actual size will be length * count bytes */
bdesc_t * bdesc_alloc(uint32_t number, uint16_t length, uint16_t count);

/* wrap a ddesc in a new bdesc */
bdesc_t * bdesc_alloc_wrap(datadesc_t * ddesc, uint32_t number, uint16_t count);

/* make a new bdesc that shares a ddesc with another bdesc */
bdesc_t * bdesc_alloc_clone(bdesc_t * original, uint32_t number);

/* increase the reference count of a bdesc */
bdesc_t * bdesc_retain(bdesc_t * bdesc);

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc);

/* schedule the bdesc to be released at the end of the current run loop */
bdesc_t * bdesc_autorelease(bdesc_t * bdesc);

/* push an autorelease pool onto the stack */
int bdesc_autorelease_pool_push(void);

/* pop an autorelease pool off the stack */
void bdesc_autorelease_pool_pop(void);

/* get the number of autorelease pools on the stack */
unsigned int bdesc_autorelease_pool_depth(void);

/* scan the autorelease pool stack and return the total ar_count of a ddesc */
int bdesc_autorelease_poolstack_scan(datadesc_t * ddesc);

/* compares two bdescs' blocknos for qsort */
int bdesc_blockno_compare(const void * a, const void * b);

#endif /* __KUDOS_KFS_BDESC_H */
