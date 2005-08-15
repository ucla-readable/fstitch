#ifndef __KUDOS_KFS_CHDESC_H
#define __KUDOS_KFS_CHDESC_H

#include <inc/types.h>

struct chdesc;
typedef struct chdesc chdesc_t;

struct chmetadesc;
typedef struct chmetadesc chmetadesc_t;

struct chrefdesc;
typedef struct chrefdesc chrefdesc_t;

#include <kfs/bd.h>
#include <kfs/bdesc.h>

#define CHDESC_MARKED    0x01 /* marker for graph traversal */
#define CHDESC_INSET     0x02 /* indicator for set membership */
#define CHDESC_MOVED     0x04 /* flag for moving chdescs */
#define CHDESC_ROLLBACK  0x08 /* chdesc is rolled back */
#define CHDESC_READY     0x10 /* chdesc is ready to be written */
#define CHDESC_FREEING   0x20 /* this chdesc is being freed */

struct chdesc {
	BD_t * owner;
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
	uint32_t stamps;
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
chdesc_t * chdesc_create_noop(bdesc_t * block, BD_t * owner);
chdesc_t * chdesc_create_bit(bdesc_t * block, BD_t * owner, uint16_t offset, uint32_t xor);
int chdesc_create_byte(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * data, chdesc_t ** head, chdesc_t ** tail);
int chdesc_create_init(bdesc_t * block, BD_t * owner, chdesc_t ** head, chdesc_t ** tail);
int chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, chdesc_t ** tail);

/* add a dependency to a change descriptor */
int chdesc_add_depend(chdesc_t * dependent, chdesc_t * dependency);

/* remove a dependency from a change descriptor */
void chdesc_remove_depend(chdesc_t * dependent, chdesc_t * dependency);

/* apply and roll back change descriptors */
int chdesc_apply(chdesc_t * chdesc);
int chdesc_rollback(chdesc_t * chdesc);

/* satisfy a change descriptor, i.e. remove it from all others that depend on it */
int chdesc_satisfy(chdesc_t * chdesc);

/* create and remove weak references to a chdesc */
int chdesc_weak_retain(chdesc_t * chdesc, chdesc_t ** location);
void chdesc_weak_forget(chdesc_t ** location);
void chdesc_weak_release(chdesc_t ** location);

/* destroy a chdesc */
void chdesc_destroy(chdesc_t ** chdesc);

/* hidden functions for use in chdesc_util.c */
int __ensure_bdesc_has_changes(bdesc_t * block);
int __chdesc_create_full(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, chdesc_t ** tail, bool slip_under);
int __chdesc_add_depend_fast(chdesc_t * dependent, chdesc_t * dependency);
int __chdesc_overlap_multiattach(chdesc_t * chdesc, bdesc_t * block);

uint32_t chdesc_register_stamp(BD_t * bd);
void chdesc_release_stamp(uint32_t stamp);

static __inline void chdesc_stamp(chdesc_t * chdesc, uint32_t stamp) __attribute__((always_inline));
static __inline void chdesc_stamp(chdesc_t * chdesc, uint32_t stamp)
{
	chdesc->stamps |= stamp;
}

static __inline int chdesc_has_stamp(chdesc_t * chdesc, uint32_t stamp) __attribute__((always_inline));
static __inline int chdesc_has_stamp(chdesc_t * chdesc, uint32_t stamp)
{
	return chdesc->stamps & stamp;
}

/* also include utility functions */
#include <kfs/chdesc_util.h>

#endif /* __KUDOS_KFS_CHDESC_H */
