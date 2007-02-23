#ifndef __KUDOS_KFS_OPGROUP_H
#define __KUDOS_KFS_OPGROUP_H

#ifdef KFSD
#include <kfs/chdesc.h>
#endif

/* The following operations change the state of opgroups:
 * A) Add afters    W) Write data (i.e. engage)
 * R) Release       B) Add befores
 * 
 * NOTICE: If you read any of this paragraph, read all of it.
 * Of these, adding afters and releasing may always be performed. However,
 * adding a before may only be done before the opgroup is released, and
 * writing data can only be done if there are no afters. It should also be
 * noted that abandoning an opgroup before releasing it causes it to be aborted.
 * (Think about abandoning your pet mouse: if you release it beforehand, it can
 * live in the wild. If not, it will die in its cage.) Finally, "writing data"
 * to an opgroup can occur any time an opgroup is engaged. Thus any operation
 * which would make writing data invalid (like adding an after) must require
 * that the opgroup is not currently engaged. (So it's not strictly true that
 * adding afters may always be performed.)
 * 
 * The following table shows the possible states and what operations are valid.
 * Notice that each of A, R, W, and B above sets a bit in the state of an
 * opgroup. Adding a before means that the opgroup now has before,
 * releasing an opgroup means it is now released, etc.
 * 
 * A R W B  Can do: [ (W) means we do not allow this now, but plan to when we
 * -------            work out how to hide changes from other clients. ]
 * 0 0 0 0   A   R  (W)  B
 * 0 0 0 1   A   R  (W)  B
 * 0 0 1 0   A   R   W   B  <--- initially, these states cannot exist due to (W)
 * 0 0 1 1   A   R   W   B  <-/
 * 0 1 0 0   A   R   W
 * 0 1 0 1   A   R   W
 * 0 1 1 0   A   R   W
 * 0 1 1 1   A   R   W
 * 1 0 0 0   A   R          <--- these are "noop" opgroups. B is disallowed to easily prevent cycles.
 * 1 0 0 1   A   R          <-/
 * 1 0 1 0   A   R       B  <--- initially, these states cannot exist due to (W)
 * 1 0 1 1   A   R       B  <-/
 * 1 1 0 0   A   R          <--- these are "noop" opgroups (the first is "dead")
 * 1 1 0 1   A   R          <-/
 * 1 1 1 0   A   R
 * 1 1 1 1   A   R
 *
 *
 * Valid operations for atomic opgroups:
 * - Add after always
 * - Add before iff !released
 * - Engage iff !released
 * - Release iff !engaged
 * - Abandon iff released (abandon without release requires a hidden opgroup)
 * */

typedef int opgroup_id_t;

#define OPGROUP_FLAG_HIDDEN 0x2
#define OPGROUP_FLAG_ATOMIC 0x6

#ifdef KFSD

struct opgroup;
typedef struct opgroup opgroup_t;

struct opgroup_scope;
typedef struct opgroup_scope opgroup_scope_t;

opgroup_scope_t * opgroup_scope_create(void);
opgroup_scope_t * opgroup_scope_copy(opgroup_scope_t * scope);
void opgroup_scope_destroy(opgroup_scope_t * scope);

void opgroup_scope_set_current(opgroup_scope_t * scope);

/* normal opgroup operations are relative to the current scope */

opgroup_t * opgroup_create(int flags);
int opgroup_sync(opgroup_t * opgroup);
int opgroup_add_depend(opgroup_t * after, opgroup_t * before);

int opgroup_engage(opgroup_t * opgroup);
int opgroup_disengage(opgroup_t * opgroup);

int opgroup_release(opgroup_t * opgroup);
int opgroup_abandon(opgroup_t ** opgroup);

opgroup_t * opgroup_lookup(opgroup_id_t id);
opgroup_id_t opgroup_id(const opgroup_t * opgroup);

/* add change descriptors to the engaged opgroups in the current scope */
int opgroup_prepare_head(chdesc_t ** head);
int opgroup_finish_head(chdesc_t * head);

#else /* KFSD */

opgroup_id_t opgroup_create(int flags);
int opgroup_sync(opgroup_id_t opgroup);
int opgroup_add_depend(opgroup_id_t after, opgroup_id_t before);

int opgroup_engage(opgroup_id_t opgroup);
int opgroup_disengage(opgroup_id_t opgroup);

int opgroup_release(opgroup_id_t opgroup);
int opgroup_abandon(opgroup_id_t opgroup);

/* Create a linear dependency chain:
 * Create, release, and engage a new opgroup.
 * If 'previous >= 0' then the new opgroup will depend on previous
 * and previous will be disengaged and abandoned. */
int opgroup_linear(opgroup_id_t previous);

#endif /* KFSD */

#endif /* __KUDOS_KFS_OPGROUP_H */
