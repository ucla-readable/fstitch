/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_KERN_ENV_H
#define KUDOS_KERN_ENV_H

#include <inc/env.h>

#ifndef KUDOS_MULTIENV
// Change this value to 1 once you've started on Lab 3 Part 3.
#define KUDOS_MULTIENV 1
#endif

extern int env_debug;
extern struct Env* envs;		// All environments
extern struct Env* curenv;	        // Current environment
extern volatile uint64_t env_tsc;	// Current env start TSC

#define ENVID_KERNEL -1

LIST_HEAD(Env_list, Env);		// Declares 'struct Env_list'

void env_init(void);
int env_alloc(struct Env** e, envid_t parent_id, int priority);
void env_free(struct Env*);
void env_create(uint8_t* binary, size_t size);
void env_destroy(struct Env* e);	// Does not return if e == curenv

int envid2env(envid_t envid, struct Env** penv, int checkperm);
void env_run(struct Env* e) __attribute__((noreturn));
void env_pop_tf(struct Trapframe* tf) __attribute__((noreturn));

// for the grading script
#define ENV_CREATE2(start, size)	{		\
	extern uint8_t start[], size[];			\
	env_create(start, (int)size);			\
}

#define ENV_CREATE(x)			{		\
	extern uint8_t _binary_obj_##x##_start[],	\
		_binary_obj_##x##_size[];		\
	env_create(_binary_obj_##x##_start,		\
		(int)_binary_obj_##x##_size);		\
}

#endif // !KUDOS_KERN_ENV_H
