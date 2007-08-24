#ifndef __FSTITCH_FSCORE_KERNEL_SERVE_H
#define __FSTITCH_FSCORE_KERNEL_SERVE_H

#include <lib/platform.h>

#include <linux/kernel.h>
#include <asm/current.h>
#include <linux/sched.h>

#include <fscore/cfs.h>
#include <fscore/fstitchd.h>
#include <fscore/sched.h>
#include <fscore/patchgroup.h>
#include <fscore/kernel_patchgroup_scopes.h>

#define CONTENTION_WARNING 0

int kernel_serve_add_mount(const char * path, CFS_t * cfs);

#define fstitchd_add_mount(p, c) kernel_serve_add_mount(p, c)

int kernel_serve_init(void);

struct task_struct;
extern struct task_struct * fstitchd_task;

typedef void (*unlock_callback_t)(void *, int);
struct callback_list {
	unlock_callback_t callback;
	void * data;
	int count;
	struct callback_list * next;
};

/* Linux doesn't like us scheduling while we hold a lock. We want to be able to
 * do it anyway, so we build a spinlock out of a spinlock. While we're at it,
 * add the PID of the process holding the lock. This structure is initialized by
 * code in fstitchd.c at the beginning of the kernel thread. */
struct stealth_lock {
	spinlock_t lock;
	int locked;
	pid_t process;
	struct callback_list * callbacks;
};

extern struct stealth_lock fstitchd_global_lock;

static inline void fstitchd_enter(void) __attribute__((always_inline));
static inline int fstitchd_have_lock(void) __attribute__((always_inline));
static inline void fstitchd_leave(int cleanup) __attribute__((always_inline));

static inline int fstitchd_have_lock(void)
{
	return fstitchd_global_lock.locked && fstitchd_global_lock.process == current->pid;
}

static inline void fstitchd_enter(void)
{
#if CONTENTION_WARNING
	int tries = 0;
#endif
	assert(!fstitchd_have_lock());

	for(;;)
	{
		spin_lock(&fstitchd_global_lock.lock);
		if(!fstitchd_global_lock.locked)
		{
			fstitchd_global_lock.locked = 1;
			fstitchd_global_lock.process = current->pid;
			spin_unlock(&fstitchd_global_lock.lock);
			patchgroup_scope_set_current(process_patchgroup_scope(current));
#if CONTENTION_WARNING
			if(tries >= 5)
				printk(KERN_EMERG "%s failed to acquire fstitchd lock %d times\n", current->comm, tries);
#endif
			return;
		}
#if CONTENTION_WARNING
		if(++tries == 5)
			printk(KERN_EMERG "fstitchd_global_lock contention detected! (%s)\n", current->comm);
#endif
		spin_unlock(&fstitchd_global_lock.lock);
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 100);
	}
}

static inline int fstitchd_unlock_callback(unlock_callback_t callback, void * data)
{
	assert(fstitchd_global_lock.locked);
	assert(fstitchd_global_lock.process == current->pid);
	if(fstitchd_global_lock.callbacks && fstitchd_global_lock.callbacks->callback == callback && fstitchd_global_lock.callbacks->data == data)
		fstitchd_global_lock.callbacks->count++;
	else
	{
		struct callback_list * list = malloc(sizeof(*list));
		if(!list)
			return -ENOMEM;
		list->callback = callback;
		list->data = data;
		list->count = 1;
		list->next = fstitchd_global_lock.callbacks;
		fstitchd_global_lock.callbacks = list;
	}
	return 0;
}

static inline void fstitchd_leave(int cleanup)
{
	assert(fstitchd_global_lock.locked);
	assert(fstitchd_global_lock.process == current->pid);
	while(fstitchd_global_lock.callbacks)
	{
		struct callback_list * first = fstitchd_global_lock.callbacks;
		fstitchd_global_lock.callbacks = first->next;
		first->callback(first->data, first->count);
		free(first);
	}
	patchgroup_scope_set_current(NULL);
	if(cleanup)
		sched_run_cleanup();
	fstitchd_global_lock.process = 0;
	fstitchd_global_lock.locked = 0;
}

#endif // !__FSTITCH_FSCORE_KERNEL_SERVE_H
