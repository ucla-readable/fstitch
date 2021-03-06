/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_PATCHGROUP_H
#define __FSTITCH_FSCORE_PATCHGROUP_H

#ifdef FSTITCHD
#include <fscore/patch.h>
#endif

/* The following operations change the state of patchgroups:
 * A) Add afters    W) Write data (i.e. engage)
 * R) Release       B) Add befores
 * 
 * The following table shows the possible states and what operations are valid.
 * Notice that each of A, R, W, and B above sets a bit in the state of a
 * patchgroup. Adding a before means that the patchgroup now has before,
 * releasing a patchgroup means it is now released, etc.
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
 * 1 0 0 0   A   R          <--- these are "empty" patchgroups. B is disallowed to easily prevent cycles.
 * 1 0 0 1   A   R          <-/
 * 1 0 1 0   A   R       B  <--- initially, these states cannot exist due to (W)
 * 1 0 1 1   A   R       B  <-/
 * 1 1 0 0   A   R          <--- these are "empty" patchgroups (the first is "dead")
 * 1 1 0 1   A   R          <-/
 * 1 1 1 0   A   R
 * 1 1 1 1   A   R
 *
 *
 * Valid operations for atomic patchgroups:
 * - Add after always
 * - Add before iff !released
 * - Engage iff !released
 * - Release iff !engaged
 * - Abandon iff released (abandon without release requires a hidden patchgroup)
 * */

typedef int patchgroup_id_t;

#define PATCHGROUP_FLAG_HIDDEN 0x2
#define PATCHGROUP_FLAG_ATOMIC 0x6

#ifdef FSTITCHD

struct patchgroup;
typedef struct patchgroup patchgroup_t;

struct patchgroup_scope;
typedef struct patchgroup_scope patchgroup_scope_t;

patchgroup_scope_t * patchgroup_scope_create(void);
patchgroup_scope_t * patchgroup_scope_copy(patchgroup_scope_t * scope);
size_t patchgroup_scope_size(patchgroup_scope_t * scope);
void patchgroup_scope_destroy(patchgroup_scope_t * scope);

void patchgroup_scope_set_current(patchgroup_scope_t * scope);

/* normal patchgroup operations are relative to the current scope */

patchgroup_t * patchgroup_create(int flags);
int patchgroup_sync(patchgroup_t * patchgroup);
int patchgroup_add_depend(patchgroup_t * after, patchgroup_t * before);

int patchgroup_engage(patchgroup_t * patchgroup);
int patchgroup_disengage(patchgroup_t * patchgroup);

int patchgroup_release(patchgroup_t * patchgroup);
int patchgroup_abandon(patchgroup_t ** patchgroup);

patchgroup_t * patchgroup_lookup(patchgroup_id_t id);
patchgroup_id_t patchgroup_id(const patchgroup_t * patchgroup);

int patchgroup_engaged(void);
void patchgroup_masquerade(void);
void patchgroup_demasquerade(void);

/* add patches to the engaged patchgroups in the current scope */
int patchgroup_prepare_head(patch_t ** head);
int patchgroup_finish_head(patch_t * head);

int patchgroup_label(patchgroup_t * patchgroup, const char * label);

#else /* FSTITCHD */

patchgroup_id_t patchgroup_create(int flags);
int patchgroup_sync(patchgroup_id_t patchgroup);
int patchgroup_add_depend(patchgroup_id_t after, patchgroup_id_t before);

int patchgroup_engage(patchgroup_id_t patchgroup);
int patchgroup_disengage(patchgroup_id_t patchgroup);

int patchgroup_release(patchgroup_id_t patchgroup);
int patchgroup_abandon(patchgroup_id_t patchgroup);

int patchgroup_label(patchgroup_id_t patchgroup, const char * label);


/* Create, release, and engage a new patchgroup.
 * Make the new patchgroup depend on previous... until <0.
 * Each previous must be disengaged. Does not abandon previous. */
patchgroup_id_t patchgroup_create_engage(patchgroup_id_t previous, ...);

/* Create a linear dependency chain:
 * Create, release, and engage a new patchgroup.
 * If 'previous >= 0' then the new patchgroup will depend on previous
 * and previous will be disengaged and abandoned. */
patchgroup_id_t patchgroup_linear(patchgroup_id_t previous);

#endif /* FSTITCHD */

#endif /* __FSTITCH_FSCORE_PATCHGROUP_H */
