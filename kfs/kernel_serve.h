#ifndef __KUDOS_KFS_KERNEL_SERVE_H
#define __KUDOS_KFS_KERNEL_SERVE_H

#include <linux/kernel.h>
#include <lib/assert.h>
#include <kfs/cfs.h>
#include <kfs/sched.h>

int kernel_serve_add_mount(const char * path, CFS_t * cfs);

#define kfsd_add_mount(p, c) kernel_serve_add_mount(p, c)

int kernel_serve_init(void);

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
	for(;;)
	{
		spin_lock(&kfsd_global_lock.lock);
		if(!kfsd_global_lock.locked)
		{
			kfsd_global_lock.locked = 1;
			kfsd_global_lock.process = current->pid;
			spin_unlock(&kfsd_global_lock.lock);
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
	if(cleanup)
		sched_run_cleanup();
	kfsd_global_lock.process = 0;
	kfsd_global_lock.locked = 0;
}

#endif // !__KUDOS_KFS_KERNEL_SERVE_H
