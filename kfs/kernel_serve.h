#ifndef __KUDOS_KFS_KERNEL_SERVE_H
#define __KUDOS_KFS_KERNEL_SERVE_H

#include <linux/kernel.h>
#include <asm/current.h>
#include <linux/sched.h>
#include <lib/assert.h>
#include <kfs/cfs.h>
#include <kfs/kfsd.h>
#include <kfs/sched.h>
#include <kfs/opgroup.h>
#include <kfs/kernel_opgroup_scopes.h>

int kernel_serve_add_mount(const char * path, CFS_t * cfs);

#define kfsd_add_mount(p, c) kernel_serve_add_mount(p, c)

int kernel_serve_init(void);

struct task_struct;
extern struct task_struct * kfsd_task;

/* Linux doesn't like us scheduling while we hold a lock. We want to be able to
 * do it anyway, so we build a spinlock out of a spinlock. While we're at it,
 * add the PID of the process holding the lock. This structure is initialized by
 * code in kfsd.c at the beginning of the kernel thread. */
struct stealth_lock {
	spinlock_t lock;
	int locked;
	pid_t process;
};

extern struct stealth_lock kfsd_global_lock;

static inline void kfsd_enter(void) __attribute__((always_inline));
static inline int kfsd_have_lock(void) __attribute__((always_inline));
static inline void kfsd_leave(int cleanup) __attribute__((always_inline));

static inline void kfsd_enter(void)
{
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
			/* starting a new request, so set a new request ID */
			kfsd_next_request_id();
			return;
		}
		spin_unlock(&kfsd_global_lock.lock);
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 100);
	}
}

static inline int kfsd_have_lock(void)
{
	return kfsd_global_lock.locked && kfsd_global_lock.process == current->pid;
}

static inline void kfsd_leave(int cleanup)
{
	assert(kfsd_global_lock.locked);
	assert(kfsd_global_lock.process == current->pid);
	opgroup_scope_set_current(NULL);
	if(cleanup)
		sched_run_cleanup();
	kfsd_global_lock.process = 0;
	kfsd_global_lock.locked = 0;
}

#endif // !__KUDOS_KFS_KERNEL_SERVE_H
