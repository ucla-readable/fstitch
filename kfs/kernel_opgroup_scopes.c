#include <kfs/kernel_opgroup_scopes.h>

#include <linux/config.h>

#ifdef CONFIG_KUDOS_PROC
#include <linux/kudos_proc.h>

#include <inc/error.h>
#include <lib/hash_map.h>
#include <lib/stdio.h>
#include <kfs/kfsd.h>
#include <kfs/kernel_serve.h>

static hash_map_t * scope_map = NULL;

/* This also gets called for clone()! Check task->pid and task->tgid. */
static void fork_handler(struct task_struct * child)
{
	opgroup_scope_t * parent_scope;
	
	kfsd_enter();
	
	parent_scope = hash_map_find_val(scope_map, child->real_parent);
	if(parent_scope)
	{
		opgroup_scope_t * child_scope = opgroup_scope_copy(parent_scope);
		if(child_scope)
		{
			int r = hash_map_insert(scope_map, child, child_scope);
			if(r < 0)
			{
				opgroup_scope_destroy(child_scope);
				goto fail;
			}
		}
		else
fail:
			kdprintf(STDERR_FILENO, "error creating child scope for PID %d!\n", child->pid);
	}
	
	kfsd_leave(0);
}

static void exec_handler(struct task_struct * process)
{
}

static void exit_handler(struct task_struct * process)
{
	opgroup_scope_t * scope;
	
	kfsd_enter();
	
	scope = hash_map_find_val(scope_map, process);
	if(scope)
	{
		hash_map_erase(scope_map, process);
		opgroup_scope_destroy(scope);
	}
	
	kfsd_leave(0);
}

opgroup_scope_t * process_opgroup_scope(const struct task_struct * task)
{
	opgroup_scope_t * scope;

	if (task == kfsd_task)
		return NULL;

	scope = hash_map_find_val(scope_map, task);
	if (!scope && (scope = opgroup_scope_create()))
	{
		if (hash_map_insert(scope_map, (void *) task, scope) < 0)
		{
			opgroup_scope_destroy(scope);
			scope = NULL;
		}
	}
	return scope;
}


static struct kudos_proc_ops ops = {
	.fork = fork_handler,
	.exec = exec_handler,
	.exit = exit_handler
};

static void kernel_opgroup_scopes_shutdown(void * ignore)
{
	hash_map_destroy(scope_map);
	/* check return value? */
	kudos_unregister_module(&ops);
	scope_map = NULL;
}

int kernel_opgroup_scopes_init(void)
{
	int r;
	
	scope_map = hash_map_create();
	if (!scope_map)
		return -E_NO_MEM;
	
	r = kudos_register_module(&ops);
	if (r < 0)
	{
		kernel_opgroup_scopes_shutdown(NULL);
		return r;
	}
	
	r = kfsd_register_shutdown_module(kernel_opgroup_scopes_shutdown, NULL);
	if (r < 0)
	{
		kernel_opgroup_scopes_shutdown(NULL);
		return r;
	}
	
	return 0;
}

#else

opgroup_scope_t * process_opgroup_scope(const struct task_struct * task)
{
	return NULL;
}

int kernel_opgroup_scopes_init(void)
{
	printf("This version of kkfsd was compiled without opgroup support!\n");
	return 0;
}

#endif
