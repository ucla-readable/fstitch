/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/hash_map.h>

#include <fscore/kernel_patchgroup_scopes.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
#include <linux/config.h>
#endif

#ifdef CONFIG_FSTITCH_PROC
#include <linux/fstitch_proc.h>

#include <fscore/fstitchd.h>
#include <fscore/kernel_serve.h>

static hash_map_t * scope_map = NULL;
static spinlock_t scope_lock = SPIN_LOCK_UNLOCKED;

/* This also gets called for clone()! Check task->pid and task->tgid. */
static void fork_handler(struct task_struct * child)
{
	patchgroup_scope_t * parent_scope;
	
	/* Why is this assertion not always true? */
	/* assert(current == child->real_parent); */
	spin_lock(&scope_lock);
	
	parent_scope = hash_map_find_val(scope_map, child->real_parent);
	if(parent_scope && patchgroup_scope_size(parent_scope) > 0)
	{
		patchgroup_scope_t * child_scope;
		
		/* We are executing in the context of the parent, which is the
		 * only process that could alter its scope. Thus it is OK to
		 * release the scope lock, call fstitchd_enter(), and then reacquire
		 * the scope lock. */
		spin_unlock(&scope_lock);
		fstitchd_enter();
		spin_lock(&scope_lock);
		
		child_scope = patchgroup_scope_copy(parent_scope);
		if(child_scope)
		{
			int r = hash_map_insert(scope_map, child, child_scope);
			if(r < 0)
			{
				patchgroup_scope_destroy(child_scope);
				goto fail;
			}
		}
		else
fail:
			fprintf(stderr, "error creating child scope for PID %d!\n", child->pid);
		
		fstitchd_leave(0);
	}
	spin_unlock(&scope_lock);
}

static void exec_handler(struct task_struct * process)
{
}

static void exit_handler(struct task_struct * process)
{
	patchgroup_scope_t * scope;
	
	assert(current == process);
	spin_lock(&scope_lock);
	
	scope = hash_map_find_val(scope_map, process);
	if(scope)
	{
		/* See fork_handler() for an explanation of these 3 lines. */
		spin_unlock(&scope_lock);
		fstitchd_enter();
		spin_lock(&scope_lock);
		
		hash_map_erase(scope_map, process);
		patchgroup_scope_destroy(scope);
		
		fstitchd_leave(0);
	}
	spin_unlock(&scope_lock);
}

patchgroup_scope_t * process_patchgroup_scope(const struct task_struct * task)
{
	patchgroup_scope_t * scope;

	if(task == fstitchd_task)
		return NULL;

	spin_lock(&scope_lock);
	
	scope = hash_map_find_val(scope_map, task);
	if(!scope && (scope = patchgroup_scope_create()))
	{
		if(hash_map_insert(scope_map, (void *) task, scope) < 0)
		{
			patchgroup_scope_destroy(scope);
			scope = NULL;
		}
	}
	
	spin_unlock(&scope_lock);
	
	return scope;
}


static struct fstitch_proc_ops ops = {
	.fork = fork_handler,
	.exec = exec_handler,
	.exit = exit_handler
};

static void kernel_patchgroup_scopes_shutdown(void * ignore)
{
	/* check return value? */
	fstitch_unregister_module(&ops);
	hash_map_destroy(scope_map);
	scope_map = NULL;
}

int kernel_patchgroup_scopes_init(void)
{
	int r;
	
	scope_map = hash_map_create();
	if(!scope_map)
		return -ENOMEM;
	
	r = fstitch_register_module(&ops);
	if(r < 0)
	{
		kernel_patchgroup_scopes_shutdown(NULL);
		return r;
	}
	
	r = fstitchd_register_shutdown_module(kernel_patchgroup_scopes_shutdown, NULL, SHUTDOWN_PREMODULES);
	if(r < 0)
	{
		kernel_patchgroup_scopes_shutdown(NULL);
		return r;
	}
	
	return 0;
}

#else

patchgroup_scope_t * process_patchgroup_scope(const struct task_struct * task)
{
	return NULL;
}

int kernel_patchgroup_scopes_init(void)
{
	printf("This version of kfstitchd was compiled without patchgroup support!\n");
	return 0;
}

#endif
