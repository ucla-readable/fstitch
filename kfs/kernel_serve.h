#ifndef __KUDOS_KFS_KERNEL_SERVE_H
#define __KUDOS_KFS_KERNEL_SERVE_H

#include <lib/platform.h>

#include <linux/kernel.h>
#include <asm/current.h>
#include <linux/sched.h>

#include <kfs/cfs.h>
#include <kfs/kfsd.h>
#include <kfs/sched.h>
#include <kfs/opgroup.h>
#include <kfs/kernel_opgroup_scopes.h>

#define CONTENTION_WARNING 0

int kernel_serve_add_mount(const char * path, CFS_t * cfs);

#define kfsd_add_mount(p, c) kernel_serve_add_mount(p, c)

int kernel_serve_init(void);

struct task_struct;
extern struct task_struct * kfsd_task;

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
 * code in kfsd.c at the beginning of the kernel thread. */
struct stealth_lock {
	spinlock_t lock;
	int locked;
	pid_t process;
	struct callback_list * callbacks;
};

extern struct stealth_lock kfsd_global_lock;

static inline void kfsd_enter(void) __attribute__((always_inline));
static inline int kfsd_have_lock(void) __attribute__((always_inline));
static inline void kfsd_leave(int cleanup) __attribute__((always_inline));

static inline int kfsd_have_lock(void)
{
	return kfsd_global_lock.locked && kfsd_global_lock.process == current->pid;
}

static inline void kfsd_enter(void)
{
#if CONTENTION_WARNING
	int tries = 0;
#endif
	assert(!kfsd_have_lock());

	for(;;)
	{
		spin_lock(&kfsd_global_lock.lock);
		if(!kfsd_global_lock.locked)
		{
			kfsd_global_lock.locked = 1;
			kfsd_global_lock.process = current->pid;
			spin_unlock(&kfsd_global_lock.lock);
			opgroup_scope_set_current(process_opgroup_scope(current));
#if CONTENTION_WARNING
			if(tries >= 5)
				printk(KERN_EMERG "%s failed to acquire kfsd lock %d times\n", current->comm, tries);
#endif
			return;
		}
#if CONTENTION_WARNING
		if(++tries == 5)
			printk(KERN_EMERG "kfsd_global_lock contention detected! (%s)\n", current->comm);
#endif
		spin_unlock(&kfsd_global_lock.lock);
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 100);
	}
}

static inline int kfsd_unlock_callback(unlock_callback_t callback, void * data)
{
	assert(kfsd_global_lock.locked);
	assert(kfsd_global_lock.process == current->pid);
	if(kfsd_global_lock.callbacks && kfsd_global_lock.callbacks->callback == callback && kfsd_global_lock.callbacks->data == data)
		kfsd_global_lock.callbacks->count++;
	else
	{
		struct callback_list * list = malloc(sizeof(*list));
		if(!list)
			return -ENOMEM;
		list->callback = callback;
		list->data = data;
		list->count = 1;
		list->next = kfsd_global_lock.callbacks;
		kfsd_global_lock.callbacks = list;
	}
	return 0;
}

static inline void kfsd_leave(int cleanup)
{
	assert(kfsd_global_lock.locked);
	assert(kfsd_global_lock.process == current->pid);
	while(kfsd_global_lock.callbacks)
	{
		struct callback_list * first = kfsd_global_lock.callbacks;
		kfsd_global_lock.callbacks = first->next;
		first->callback(first->data, first->count);
		free(first);
	}
	opgroup_scope_set_current(NULL);
	if(cleanup)
		sched_run_cleanup();
	kfsd_global_lock.process = 0;
	kfsd_global_lock.locked = 0;
}

#endif // !__KUDOS_KFS_KERNEL_SERVE_H
