#ifndef __KUDOS_KFS_OPGROUP_H
#define __KUDOS_KFS_OPGROUP_H

#include <lib/hash_map.h>

#include <kfs/chdesc.h>

/* The following operations change the state of opgroups:
 * C) Add dependents    W) Write data
 * R) Release           T) Add dependencies
 * 
 * Of these, adding dependents and releasing may always be performed. However,
 * adding a dependency may only be done before the opgroup is released, and
 * writing data can only be done if there are no dependents. It should also be
 * noted that abandoning an opgroup before releasing it causes it to be aborted.
 * (Think about abandoning your pet mouse: if you release it beforehand, it can
 * live in the wild. If not, it will die in its cage.)
 * 
 * The following table shows the possible states and what operations are valid.
 * Notice that each of C, R, W, and T above sets a bit in the state of an
 * opgroup. Adding a dependency means that the opgroup now has dependencies,
 * releasing an opgroup means it is now released, etc.
 * 
 * C R W T  Can do: [ (W) means we do not allow this now, but plan to when we
 * -------            work out how to hide changes from other clients. ]
 * 0 0 0 0   C   R  (W)  T
 * 0 0 0 1   C   R  (W)  T
 * 0 0 1 0   C   R   W   T  <--- initially, these states cannot exist due to (W)
 * 0 0 1 1   C   R   W   T  <-/
 * 0 1 0 0   C   R   W
 * 0 1 0 1   C   R   W
 * 0 1 1 0   C   R   W
 * 0 1 1 1   C   R   W
 * 1 0 0 0   C   R       T  <--- these are "noop" opgroups
 * 1 0 0 1   C   R       T  <-/
 * 1 0 1 0   C   R       T  <--- initially, these states cannot exist due to (W)
 * 1 0 1 1   C   R       T  <-/
 * 1 1 0 0   C   R          <--- these are "noop" opgroups (the first is "dead")
 * 1 1 0 1   C   R          <-/
 * 1 1 1 0   C   R
 * 1 1 1 1   C   R
 * */

typedef int opgroup_id_t;

struct opgroup;
typedef struct opgroup opgroup_t;

struct opgroup_scope;
typedef struct opgroup_scope opgroup_scope_t;

#define OPGROUP_FLAG_HIDDEN 0x2
#define OPGROUP_FLAG_ATOMIC 0x6

opgroup_scope_t * opgroup_scope_create(void);
opgroup_scope_t * opgroup_scope_copy(opgroup_scope_t * scope);
void opgroup_scope_destroy(opgroup_scope_t * scope);

void opgroup_scope_set_current(opgroup_scope_t * scope);

/* normal opgroup operations are relative to the current scope */

opgroup_t * opgroup_create(int flags);
int opgroup_add_depend(opgroup_t * dependent, opgroup_t * dependency);

int opgroup_engage(opgroup_t * opgroup);
int opgroup_disengage(opgroup_t * opgroup);

int opgroup_release(opgroup_t * opgroup);
int opgroup_abandon(opgroup_t ** opgroup);

opgroup_t * opgroup_lookup(opgroup_id_t id);

/* add change descriptors to the engaged opgroups in the current scope */
int opgroup_insert_change(chdesc_t * head, chdesc_t * tail);

#endif /* __KUDOS_KFS_OPGROUP_H */
