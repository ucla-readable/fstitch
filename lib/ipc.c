// User-level IPC library routines

#include <inc/lib.h>

// Receive a value via IPC and return it.
// If 'pg' is nonnull, then any page sent by the sender will be mapped at
//	that address.
// If 'fromenv' is nonnull, then store the IPC sender's envid in *fromenv.
// If 'perm' is nonnull, then store the IPC sender's page permission in *perm
//	(this is nonzero iff a page was successfully transferred to 'pg').
// If the system call fails, then store 0 in *fromenv and *perm (if
//	they're nonnull) and return the error.
//
// Hint:
//   Use 'env' to discover the value and who sent it.
//   If 'pg' is null, pass sys_ipc_recv a value that it will understand
//   as meaning "no page".  (Zero is not the right value.)
uint32_t
ipc_recv(envid_t* fromenv, void* pg, unsigned* perm, int timeout)
{
	int r;
	if(!pg)
		pg = (void *) UTOP;
	
	do {
		r = sys_ipc_recv(pg, timeout);
	} while(r == -E_TIMEOUT && timeout < 1);
	
	if(r)
	{
		if(fromenv)
			*fromenv = 0;
		if(perm)
			*perm = 0;
		return r;
	}
	
	if(fromenv)
		*fromenv = env->env_ipc_from;
	if(perm)
		*perm = env->env_ipc_perm;
	
	return env->env_ipc_value;
}

// Send 'val' (and 'pg' with 'perm', assuming 'pg' is nonnull) to 'toenv'.
// This function keeps trying until it succeeds.
// It should panic() on any error other than -E_IPC_NOT_RECV.
//
// Hint:
//   Use sys_yield() to be CPU-friendly.
//   If 'pg' is null, pass sys_ipc_recv a value that it will understand
//   as meaning "no page".  (Zero is not the right value.)
void
ipc_send(envid_t toenv, uint32_t val, void* pg, unsigned perm)
{
	int r;
	if(!pg)
		pg = (void *) UTOP;
	
	for(;;)
	{
		r = sys_ipc_try_send(toenv, val, pg, perm);
		
		if(!r || r == 1)
			return;
		if(r != -E_IPC_NOT_RECV)
		{
			/* panic() here can cause the filesystem server
			 * to lock up due to malicious user code! */
			//panic("ipc_send: %e", r);
			fprintf(STDERR_FILENO, "ipc_send from 0x%08x to 0x%08x: %e\n", env->env_id, toenv, r);
			return;
		}
		
		sys_yield();
	}
}

