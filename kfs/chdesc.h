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
};

struct chmetadesc {
	chdesc_t * desc;
	chmetadesc_t * next;
};

#endif /* __KUDOS_KFS_CHDESC_H */
