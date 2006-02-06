#ifndef __KUDOS_LIB_OPGROUP_H
#define __KUDOS_LIB_OPGROUP_H

#define OPGROUP_FLAG_HIDDEN 0x2
#define OPGROUP_FLAG_ATOMIC 0x6

typedef int opgroup_id_t;

opgroup_id_t opgroup_create(int flags);
int opgroup_add_depend(opgroup_id_t dependent, opgroup_id_t dependency);

int opgroup_engage(opgroup_id_t opgroup);
int opgroup_disengage(opgroup_id_t opgroup);

int opgroup_release(opgroup_id_t opgroup);
int opgroup_abandon(opgroup_id_t opgroup);

#endif // __KUDOS_LIB_OPGROUP_H
