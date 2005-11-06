#include <lib/stdio.h>

#include <kfs/kfsd.h>
#include <kfs/ipc_serve.h>
#include <lib/serial_cfs.h>
#include <lib/serial_kfs.h>

#include <lib/jiffies.h>

#define IPC_RECV_TIMEOUT HZ

#if defined(KUDOS)

#include <inc/lib.h> // for get_pte()
#include <kfs/cfs_ipc_serve.h>
#include <kfs/kfs_ipc_serve.h>

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

#elif defined(UNIXUSER)

int ipc_serve_init(void)
{
	// TODO: implement this module. Perhaps use SysV IPC messages?
	kdprintf(STDERR_FILENO, "ipc_serve not yet implmented for unix-user\n");
	return 0;
}

void ipc_serve_run(void)
{
}

#else
#error Unknown target system
#endif
