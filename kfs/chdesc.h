#ifndef __KUDOS_KFS_CHDESC_H
#define __KUDOS_KFS_CHDESC_H

#include <inc/types.h>

#include <kfs/bdesc.h>

#define CHDESC_MARKED    0x01 /* marker for graph traversal */
#define CHDESC_IN_DEPMAN 0x02 /* depman has a reference to it */
#define CHDESC_ROLLBACK  0x04 /* chdesc is rolled back */

struct chdesc;
typedef struct chdesc chdesc_t;

struct chmetadesc;
typedef struct chmetadesc chmetadesc_t;

struct chrefdesc;
typedef struct chrefdesc chrefdesc_t;

struct chdesc {
	bdesc_t * block;
	enum {BIT, BYTE, NOOP} type;
	union {
		struct {
			/* offset is in units of 32-bit words */
			uint16_t offset;
			uint32_t xor;
		} bit;
		struct {
			/* offset is in bytes */
			uint16_t offset;
			uint16_t length;
			uint8_t * olddata;
			uint8_t * newdata;
		} byte;
	};
	chmetadesc_t * dependencies;
	chmetadesc_t * dependents;
	chrefdesc_t * weak_refs;
	uint16_t flags, distance;
};

struct chmetadesc {
	chdesc_t * desc;
	chmetadesc_t * next;
};

struct chrefdesc {
	chdesc_t ** desc;
	chrefdesc_t * next;
};

/* create new chdescs */
chdesc_t * chdesc_create_noop(bdesc_t * block);
chdesc_t * chdesc_create_bit(bdesc_t * block, uint16_t offset, uint32_t xor);
int chdesc_create_byte(bdesc_t * block, uint16_t offset, uint16_t length, void * data, chdesc_t ** head, chdesc_t ** tail);
int chdesc_create_init(bdesc_t * block, chdesc_t ** head, chdesc_t ** tail);
int chdesc_create_full(bdesc_t * block, void * data, chdesc_t ** head, chdesc_t ** tail);

/* perform overlap attachment */
int chdesc_overlap_attach(chdesc_t * recent, chdesc_t * original);
int chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block);

/* unmark a chdesc graph (i.e. clear CHDESC_MARKED) */
void chdesc_unmark_graph(chdesc_t * root);

/* add a dependency to a change descriptor without checking for cycles */
int chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency);

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency);

/* remove a dependency from a change descriptor */
int chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency);

/* apply and roll back change descriptors */
int chdesc_apply(chdesc_t * chdesc);
int chdesc_rollback(chdesc_t * chdesc);

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
int chdesc_satisfy(chdesc_t * chdesc);

/* create and remove weak references to a chdesc */
int chdesc_weak_retain(chdesc_t * chdesc, chdesc_t ** location);
void chdesc_weak_release(chdesc_t ** location);

/* destroy a chdesc */
int chdesc_destroy(chdesc_t ** chdesc);

#endif /* __KUDOS_KFS_CHDESC_H */
