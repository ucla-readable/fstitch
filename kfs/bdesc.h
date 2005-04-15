#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

/* RULES FOR USING BDESCS:
 *
 * 1. When a bdesc is returned or passed to you, you do not need to
 *    retain it to use it. However, you must do one of three things:
 *
 *     a) Retain it (bdesc_retain) and store it to release it (bdesc_release) later.
 *     b) Return or pass it to another module via CALL.
 *     c) Drop it (bdesc_drop).
 *
 *    If you choose option a above, you may optionally also do
 *    b (but only after a). However, b and c are mutually exclusive.
 *
 * 2. If you want to write to the data in a bdesc, you must call
 *    bdesc_touch() first. Note that this may create a copy of the data,
 *    so you must do this before saving any pointers to write to.
 *
 * 3. Since the data descriptor in a bdesc may be copied by someone
 *    else calling bdesc_touch(), you should never save any pointers into
 *    the data of a bdesc. Always access the data through the data
 *    descriptor.
 *
 * 4. To alter the structure of a bdesc (i.e. change any fields other
 *    than data), you must call bdesc_alter(). You should not call
 *    bdesc_retain() before using bdesc_alter(). Exception to this rule:
 *    if you will pass the bdesc to a function and then regain control
 *    when it returns, you may instead increase the "translated" field in
 *    the bdesc before doing the translation. Then, when control returns,
 *    you must restore the bdesc to its previous state. In this special
 *    case, you must also check the reference count of the bdesc prior to
 *    passing it, and if it is zero, you must not de-translate the bdesc
 *    - in this case, you must assume that the bdesc has been freed and
 *    not use it, just as though you had called bdesc_drop().
 * 
 * These rules guarantee the following conditions:
 * 1. The memory associated with bdescs and their data will be freed when
 *    it is no longer used.
 * 2. The data in a bdesc can only be changed by somebody with the same
 *    bdesc pointer, not a different one sharing the data.
 * 3. Except for the data, nothing else in a bdesc will change while you
 *    have a reference to it.
 * */

/* NOTE on rules 1b+c above - we need to work out what the behavior is if you
 * pass a bdesc to a function which then fails for some reason. Is it still
 * responsible for dropping the bdesc? (Answer: it should be, but currently we
 * do not generally honor this.) */

#include <inc/types.h>

#include <kfs/bd.h>

/* struct BD needs bdesc, so we avoid the cycle */
struct BD;

struct bdesc;
typedef struct bdesc bdesc_t;

struct datadesc;
typedef struct datadesc datadesc_t;

struct datadesc {
	uint8_t * data;
	uint32_t refs;
	uint16_t length;
};

struct bdesc {
	struct BD * bd;
	uint32_t number, refs;
	datadesc_t * ddesc;
	uint16_t translated;
};

/* allocate a new bdesc */
bdesc_t * bdesc_alloc(struct BD * bd, uint32_t number, uint16_t length);

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
