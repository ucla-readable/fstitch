#include <inc/lib.h> // for get_pte()
#include <lib/jiffies.h>
#include <lib/serial_cfs.h>
#include <lib/serial_kfs.h>
#include <lib/stdio.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/kfsd.h>
#include <kfs/kfs_ipc_serve.h>
#include <kfs/ipc_serve.h>

#define IPC_RECV_TIMEOUT HZ


int ipc_serve_init(void)
{
	if (get_pte((void*) IPCSERVE_REQVA) & PTE_P)
		panic("ipc_serve: IPCSERVE_REQVA already mapped");

	return 0;
}

void ipc_serve_run(void)
{
	envid_t whom;
	int perm = 0;
	uint32_t cur_cappa = 0;
	uint32_t r;

	if (get_pte((void*) IPCSERVE_REQVA) & PTE_P)
		panic("ipc_serve: IPCSERVE_REQVA already mapped");

	r = ipc_recv(0, &whom, (void*) IPCSERVE_REQVA, &perm, &cur_cappa, IPC_RECV_TIMEOUT);
	if (!whom && !perm)
	{
		if (r != -E_TIMEOUT)
			kdprintf(STDERR_FILENO, "kfsd %s: ipc_recv: %e\n", __FUNCTION__, (int) r);
		return;
	}

	switch (r)
	{
		case SCFS_VAL:
			cfs_ipc_serve_run(whom, IPCSERVE_REQVA, perm, cur_cappa);
			break;
		case SKFS_VAL:
			kfs_ipc_serve_run(whom, IPCSERVE_REQVA, perm, cur_cappa);
			break;
		default:
			kdprintf(STDERR_FILENO, "kfsd ipc_serve: Unknown type %d\n", r);
	}

	if ((r = sys_page_unmap(0, (void*) IPCSERVE_REQVA)) < 0)
		panic("sys_page_unmap: %e", r);
}

// Possible ways to implement IPC for unix-user:
//
// Perhaps use SysV IPC messages or a combination of SysV IPC shared
// memory and semaphores?
// - messages make (as of a few years ago) 2 data copies along the way.
//   kudos ipc makes none, but because of kudos ipc uses shared memory
//   cfs_ipc_client library makes 1 copy.
// - do they differ in the number of context switches?
// - we might consider how ipc will affect our move to async. eg being
//   able to select()/kqueue() incoming messages may be easier to work
//   with than just a syscall to receive data.
// - are there other performance or functionality points we should
//   consider?
// - sysv ipc messages and shared memory allow us to transfer more than
//   one page at a time. we can use this to reduce reads and writes
//   from 2 to 1 data-transfer ipc.
// - expose an IPC file, write/read from the file or use xattrs.
