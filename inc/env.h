/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_INC_ENV_H
#define KUDOS_INC_ENV_H

#include <inc/types.h>
#include <inc/queue.h>
#include <inc/trap.h>
#include <inc/pmap.h>
#include <inc/config.h>

typedef int32_t envid_t;

// An environment ID 'envid_t' has three parts:
//
// +1+---------------21-----------------+--------10--------+
// |0|          Uniqueifier             |   Environment    |
// | |                                  |      Index       |
// +------------------------------------+------------------+
//                                       \--- ENVX(eid) --/
//
// The environment index ENVX(eid) equals the environment's offset in the
// 'envs[]' array.  The uniqueifier distinguishes environments that were
// created at different times, but share the same environment index.
//
// All real environments are greater than 0 (so the sign bit is zero).
// envid_ts less than 0 signify errors.  The envid_t == 0 is special, and
// stands for the current environment.

#define LOG2NENV		10
#define NENV			(1<<LOG2NENV)
#define ENVX(envid)		((envid) & (NENV - 1))
#define ENV_NAME_LENGTH		32

// Values of env_status in struct Env
#define ENV_FREE		0
#define ENV_RUNNABLE		1
#define ENV_NOT_RUNNABLE	2

#define ENV_MAX_PRIORITY	63
#define ENV_DEFAULT_PRIORITY	(ENV_MAX_PRIORITY / 2)

struct Env {
	struct Trapframe env_tf;	// Saved registers
	LIST_ENTRY(Env) env_link;	// Free list link pointers
	envid_t env_id;			// Unique environment identifier
	envid_t env_parent_id;		// env_id of this env's parent
	unsigned env_status;		// Status of the environment
	uint32_t env_runs;		// Number of times environment has run
	uint64_t env_tsc;		// Pentium TSC total count
	int env_epriority;		// Effective priority of the environment
	int env_rpriority;		// Real priority of the environment
	int env_jiffies;		// Current scheduled start jiffies
	char env_name[ENV_NAME_LENGTH];	// Environment name

	// Address space
	pde_t* env_pgdir;		// Kernel virtual address of page dir
	physaddr_t env_cr3;		// Physical address of page dir

	// Exception handling
	uintptr_t env_pgfault_upcall;	// page fault upcall entry point

	// Lab 4 IPC
	bool env_ipc_recving;		// env is blocked receiving
	uintptr_t env_ipc_dstva;	// va at which to map received page
	uint32_t env_ipc_value;		// data value sent to us 
	envid_t env_ipc_from;		// envid of the sender	
	unsigned env_ipc_perm;		// perm of page mapping received

#if ENABLE_ENV_SYMS
	struct Sym *symtbl;
	size_t      symtbl_size;
	char       *symstrtbl;
	size_t      symstrtbl_size;
#endif
};

#endif /* !KUDOS_INC_ENV_H */
