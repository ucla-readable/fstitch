#include <inc/lib.h>
#include <inc/mouse.h>

envid_t find_moused(void)
{
	size_t ntries;

	// Try to find moused a few times, in case this env is being started
	// at the same time as moused, thus giving moused time to do its fork.
	for(ntries = 0; ntries < 10; ntries++)
	{
		int i;
		for(i = 0; i < NENV; i++)
		{
			/* search for moused with no leading / */
			if(envs[i].env_status != ENV_FREE && !strncmp(envs[i].env_name, "moused", 6))
				return envs[i].env_id;
		}
		jsleep(HZ / 5);
	}

	return -E_NO_DEV;
}

int open_mouse(void)
{
	int moused, fd;
	
	moused = find_moused();
	if(moused < 0)
		return moused;
	
	ipc_send(moused, 0, NULL, 0, NULL);
	fd = dup2env_recv(moused);
	
	return fd;
}
