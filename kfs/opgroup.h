#ifndef __KUDOS_KFS_OPGROUP_H
#define __KUDOS_KFS_OPGROUP_H

#include <kfs/chdesc.h>

/* The following operations change the state of opgroups:
 * W) Add dependents    Y) Write data
 * X) Release           Z) Add dependencies
 * 
 * Of these, adding dependents and releasing may always be performed. However,
 * adding a dependency may only be done before the opgroup is releasing, and
 * writing data can only be done if there are no dependents. It should also be
 * noted that abandoning an opgroup before releasing it causes it to be aborted.
 * (Think about abandoning your pet mouse: if you release it beforehand, it can
 * live in the wild. If not, it will die in its cage.)
 * 
 * The following table shows the possible states and what operations are valid.
 * Notice that each of W, X, Y, and Z above sets a bit in the state of an
 * opgroup. Adding a dependency means that the opgroup now has dependencies,
 * releasing an opgroup means it is now released, etc.
 * 
 * W X Y Z  Can do: [ (Y) means we do not allow this now, but plan to when we
 * -------            work out how to hide changes from other clients. ]
 * 0 0 0 0   W   X  (Y)  Z
 * 0 0 0 1   W   X  (Y)  Z
 * 0 0 1 0   W   X   Y   Z  <--- initially, these states cannot exist due to (Y)
 * 0 0 1 1   W   X   Y   Z  <-/
 * 0 1 0 0   W   X   Y
 * 0 1 0 1   W   X   Y
 * 0 1 1 0   W   X   Y
 * 0 1 1 1   W   X   Y
 * 1 0 0 0   W   X       Z  <--- these are "noop" opgroups
 * 1 0 0 1   W   X       Z  <-/
 * 1 0 1 0   W   X       Z  <--- initially, these states cannot exist due to (Y)
 * 1 0 1 1   W   X       Z  <-/
 * 1 1 0 0   W   X          <--- these are "noop" opgroups (the first is "dead")
 * 1 1 0 1   W   X          <-/
 * 1 1 1 0   W   X
 * 1 1 1 1   W   X
 * */

typedef int opgroup_id_t;

typedef struct opgroup {
	opgroup_id_t id;
	chdesc_t * head;
	chdesc_t * tail;
	chdesc_t * keep;
} opgroup_t;

#define OPGROUP_FLAG_HIDDEN 0x2
#define OPGROUP_FLAG_ATOMIC 0x6

int opgroup_init(void);

opgroup_t * opgroup_create(int flags);
int opgroup_add_depend(opgroup_t * dependent, opgroup_t * dependency);

int opgroup_engage(opgroup_t * opgroup);
int opgroup_disengage(opgroup_t * opgroup);

int opgroup_release(opgroup_t * opgroup);
int opgroup_abandon(opgroup_t ** opgroup);

opgroup_t * opgroup_lookup(opgroup_id_t id);

#endif /* __KUDOS_KFS_OPGROUP_H */
