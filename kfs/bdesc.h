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
	uint32_t ref_count;
	chdesc_t * changes;
	uint16_t length;
};

struct bdesc {
	uint32_t number;
	uint32_t ref_count;
	uint32_t ar_count;
	bdesc_t * ar_next;
	datadesc_t * ddesc;
};

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(uint32_t number, uint16_t length);

/* increase the reference count of a bdesc */
bdesc_t * bdesc_retain(bdesc_t * bdesc);

/* decrease the bdesc reference count and free it if it reaches 0 */
void bdesc_release(bdesc_t ** bdesc);

/* schedule the bdesc to be released at the end of the current run loop */
bdesc_t * bdesc_autorelease(bdesc_t * bdesc);

/* run the scheduled bdesc autoreleases */
void bdesc_run_autorelease(void);

/* a function for caches and cache-like modules to use for bdesc overwriting */
/* this may no longer be needed, but it is left commented just in case */
//int bdesc_overwrite(bdesc_t * cached, bdesc_t * written);

/* compares two bdescs' blocknos for qsort */
int bdesc_blockno_compare(const void * a, const void * b);

#endif /* __KUDOS_KFS_BDESC_H */
