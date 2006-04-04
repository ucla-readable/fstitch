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
	printk("Fork, PID %d\n", child->pid);
}

static void exec_handler(struct task_struct * process)
{
	printk("Exec, PID %d\n", process->pid);
}

static void exit_handler(struct task_struct * process)
{
	printk("Exit, PID %d\n", process->pid);
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

#warning This Linux kernel does not have KudOS support; opgroups will not work!

opgroup_scope_t * process_opgroup_scope(const struct task_struct * task)
{
	return NULL;
}

int kernel_opgroup_scopes_init(void)
{
	printk("This version of kkfsd was compiled without opgroup support!\n");
	return 0;
}

#endif
