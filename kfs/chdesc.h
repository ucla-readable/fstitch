#ifndef __KUDOS_KFS_CHDESC_H
#define __KUDOS_KFS_CHDESC_H

#include <inc/types.h>

#include <kfs/bdesc.h>

struct chdesc;
typedef struct chdesc chdesc_t;

struct chmetadesc;
typedef struct chmetadesc chmetadesc_t;

struct chdesc {
	bdesc_t * block;
	uint32_t refs;
	enum {BIT, BYTE, NOOP} type;
	union {
		struct {
			uint32_t offset;
			uint32_t xor;
		} bit;
		struct {
			uint32_t offset;
			uint32_t length;
			uint8_t * olddata;
			uint8_t * newdata;
		} byte;
	} data;
	chmetadesc_t * dependencies;
	chmetadesc_t * dependents;
	/* marker for graph traversal */
	uint8_t marked;
};

struct chmetadesc {
	chdesc_t * desc;
	chmetadesc_t * next;
};

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency);

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
int chdesc_satisfy(chdesc_t * chdesc);

#endif /* __KUDOS_KFS_CHDESC_H */
