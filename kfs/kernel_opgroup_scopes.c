#include <inc/error.h>
#include <lib/hash_map.h>
#include <kfs/kfsd.h>
#include <kfs/kernel_opgroup_scopes.h>

static hash_map_t * scope_map = NULL;


opgroup_scope_t * process_opgroup_scope(pid_t pid)
{
	opgroup_scope_t * scope = hash_map_find_val(scope_map, (void *) pid);
	if (!scope && (scope = opgroup_scope_create()))
	{
		if (hash_map_insert(scope_map, (void *) pid, scope) < 0)
		{
			opgroup_scope_destroy(scope);
			scope = NULL;
		}
	}
	return scope;
}


static void kernel_opgroup_scopes_shutdown(void * ignore)
{
	hash_map_destroy(scope_map);
	scope_map = NULL;
}

int kernel_opgroup_scopes_init(void)
{
	int r;

	scope_map = hash_map_create();
	if (!scope_map)
		return -E_NO_MEM;

	r = kfsd_register_shutdown_module(kernel_opgroup_scopes_shutdown, NULL);
	if (r < 0)
	{
		kernel_opgroup_scopes_shutdown(NULL);
		return r;
	}
	
	return 0;
}
