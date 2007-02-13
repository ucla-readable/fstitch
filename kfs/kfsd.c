#include <lib/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>

#include <linux/version.h>
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/sysrq.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
# include <linux/config.h>
#else
# include <linux/stacktrace.h>
#endif
#include <kfs/kernel_serve.h>

#include <kfs/sync.h>
#include <kfs/sched.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>
#include <kfs/destroy.h>

#define DEBUG_TOPLEVEL 0
#if DEBUG_TOPLEVEL
#define Kprintf(format, args...) printk(KERN_ERR format, ##args)
#else
#define Kprintf(...)
#endif

struct task_struct * kfsd_task;
struct stealth_lock kfsd_global_lock;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void kudos_sysrq_unlock(int key, struct pt_regs * regs, struct tty_struct * tty)
#else
static void kudos_sysrq_unlock(int key, struct tty_struct * tty)
#endif
{
	spin_lock(&kfsd_global_lock.lock);
	kfsd_global_lock.locked = 0;
	kfsd_global_lock.process = 0;
	spin_unlock(&kfsd_global_lock.lock);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 18)
/* By default, print_stack_trace() is not exported to modules.
 * It's defined in kernel/stacktrace.c, and you can just add
 * #include <linux/module.h> and EXPORT_SYMBOL(print_stack_trace);
 * there to export it. Afterward, change 0 to 1 below. */
#define EXPORTED_PRINT_STACK 0
#define PRINT_STACK_DEPTH 128

#if defined(CONFIG_STACKTRACE) && EXPORTED_PRINT_STACK
static void kudos_sysrq_showlock(int key, struct tty_struct * tty)
{
	spin_lock(&kfsd_global_lock.lock);
	if(kfsd_global_lock.locked)
	{
		struct task_struct * task;
		unsigned long entries[PRINT_STACK_DEPTH];
		struct stack_trace trace;
		trace.nr_entries = 0;
		trace.max_entries = PRINT_STACK_DEPTH;
		trace.entries = entries;
		trace.skip = 0;
		trace.all_contexts = 0;
		rcu_read_lock();
		task = find_task_by_pid_type(PIDTYPE_PID, kfsd_global_lock.process);
		save_stack_trace(&trace, task);
		rcu_read_unlock();
		print_stack_trace(&trace, 0);
	}
	spin_unlock(&kfsd_global_lock.lock);
}
#endif
#endif

static struct {
	int key;
	struct sysrq_key_op op;
} kfsd_sysrqs[] = {
	{'c', {handler: kudos_sysrq_unlock, help_msg: "kfsd_unlock(C)", action_msg: "Unlocked kfsd_lock", enable_mask: 1}},
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 18)
#if defined(CONFIG_STACKTRACE) && EXPORTED_PRINT_STACK
	{'w', {handler: kudos_sysrq_showlock, help_msg: "kfsd_tracelock(W)", action_msg: "Showing kfsd_lock owner trace", enable_mask: 1}},
#endif
#endif
};
#define KFSD_SYSRQS (sizeof(kfsd_sysrqs) / sizeof(kfsd_sysrqs[0]))

struct module_shutdown {
	const char * name;
	kfsd_shutdown_module shutdown;
	void * arg;
	int when;
};

#define MAX_NR_SHUTDOWNS 10
static struct module_shutdown module_shutdowns[MAX_NR_SHUTDOWNS];

int _kfsd_register_shutdown_module(const char * name, kfsd_shutdown_module fn, void * arg, int when)
{
	int i;

	if (when != SHUTDOWN_PREMODULES && when != SHUTDOWN_POSTMODULES)
		return -E_INVAL;

	for (i = 0; i < MAX_NR_SHUTDOWNS; i++)
	{
		if (!module_shutdowns[i].shutdown)
		{
			Kprintf("Registering shutdown callback: %s\n", name);
			module_shutdowns[i].name = name;
			module_shutdowns[i].shutdown = fn;
			module_shutdowns[i].arg = arg;
			module_shutdowns[i].when = when;
			return 0;
		}
	}

	printk(KERN_ERR "%s(): too many shutdown modules!\n", __FUNCTION__);
	return -E_NO_MEM;
}

static void kfsd_callback_shutdowns(int when)
{
	int i;
	for (i = MAX_NR_SHUTDOWNS - 1; i >= 0; i--)
	{
		if (module_shutdowns[i].shutdown && module_shutdowns[i].when == when)
		{
			Kprintf("Calling shutdown callback: %s\n", module_shutdowns[i].name);
			module_shutdowns[i].shutdown(module_shutdowns[i].arg);
			module_shutdowns[i].shutdown = NULL;
			module_shutdowns[i].arg = NULL;
			module_shutdowns[i].when = 0;
		}
	}
}

static int kfsd_running = 0;

// Shutdown kfsd: inform modules of impending shutdown, then exit.
static void kfsd_shutdown(void)
{
	printf("Syncing and shutting down.\n");
	if(kfsd_running > 0)
		kfsd_running = 0;
	
	if(kfs_sync() < 0)
		kdprintf(STDERR_FILENO, "Sync failed!\n");

	Kprintf("Calling pre-shutdown callbacks.\n");
	kfsd_callback_shutdowns(SHUTDOWN_PREMODULES);

	// Reclaim chdescs written by sync and shutdowns so that when destroy_all()
	// destroys BDs that destroy a blockman no ddescs are orphaned.
	Kprintf("Reclaiming written change descriptors.\n");
	chdesc_reclaim_written();

	Kprintf("Destroying all modules.\n");
	destroy_all();

	Kprintf("Running block descriptor autoreleasing.\n");
	// Run bdesc autoreleasing
	if (bdesc_autorelease_pool_depth() > 0)
	{
		bdesc_autorelease_pool_pop();
		assert(!bdesc_autorelease_pool_depth());
	}

	// Run chdesc reclamation
	Kprintf("Reclaiming written change descriptors.\n");
	chdesc_reclaim_written();

	Kprintf("Calling post-shutdown callbacks.\n");
	kfsd_callback_shutdowns(SHUTDOWN_POSTMODULES);
}

void kfsd_request_shutdown(void)
{
	kfsd_running = 0;
}

int kfsd_is_running(void)
{
	return kfsd_running > 0;
}

static uint32_t kfsd_request_id = 0;

void kfsd_next_request_id(void)
{
	kfsd_request_id++;
}

uint32_t kfsd_get_request_id(void)
{
	return kfsd_request_id;
}

static void kfsd_main(int nwbblocks)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	kfsd_enter();
	if ((r = kfsd_init(nwbblocks)) < 0)
	{
		printk(KERN_ERR "kfsd_init() failed in the kernel! (error = %d)\n", r);
		kfsd_running = r;
	}
	else
	{
		kfsd_running = 1;
		while(kfsd_running)
		{
			sched_run_callbacks();
			kfsd_leave(0);
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ / 25);
			kfsd_enter();
		}
	}
	kfsd_shutdown();
	kfsd_leave(0);
}

static int nwbblocks = 16384;
module_param(nwbblocks, int, 0);
MODULE_PARM_DESC(nwbblocks, "The number of write-back blocks to use");

char * linux_device = NULL;
module_param(linux_device, charp, 0);
MODULE_PARM_DESC(linux_device, "The device to attach linux_bd to");

static int kfsd_is_shutdown = 0;

static int kfsd_thread(void * thunk)
{
	int i;
	printf("kkfsd started (PID = %d)\n", current ? current->pid : 0);
	daemonize("kkfsd");
	kfsd_task = current;
	spin_lock_init(&kfsd_global_lock.lock);
	kfsd_global_lock.locked = 0;
	kfsd_global_lock.process = 0;
	for(i = 0; i < KFSD_SYSRQS; i++)
		if(register_sysrq_key(kfsd_sysrqs[i].key, &kfsd_sysrqs[i].op) < 0)
			printk(KERN_ERR "kkfsd unable to register sysrq[%c] (%d/%d)\n", kfsd_sysrqs[i].key, i + 1, KFSD_SYSRQS);
	Kprintf("Running kfsd_main()\n");
	kfsd_main(nwbblocks);
	Kprintf("kfsd_main() completed\n");
	for(i = 0; i < KFSD_SYSRQS; i++)
		if(unregister_sysrq_key(kfsd_sysrqs[i].key, &kfsd_sysrqs[i].op) < 0)
			printk(KERN_ERR "kkfsd unable to unregister sysrq[%c] (%d/%d)\n", kfsd_sysrqs[i].key, i + 1, KFSD_SYSRQS);
	printk("kkfsd exiting (PID = %d)\n", current ? current->pid : 0);
	kfsd_is_shutdown = 1;
	return 0;
}

static int __init init_kfsd(void)
{
	pid_t pid = kernel_thread(kfsd_thread, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	if(pid < 0)
		printk(KERN_ERR "kkfsd unable to start kernel thread!\n");
	while(!kfsd_running && !signal_pending(current))
	{
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 10);
	}
	/* FIXME: we should kill the kernel thread if kfsd_running is false */
	return (kfsd_running > 0) ? 0 : kfsd_running;
}

static void __exit exit_kfsd(void)
{
	kfsd_request_shutdown();
	while(!kfsd_is_shutdown)
	{
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 10);
	}
}

module_init(init_kfsd);
module_exit(exit_kfsd);

MODULE_AUTHOR("Kudos Team");
MODULE_DESCRIPTION("Kudos File System Architecture");
MODULE_LICENSE("GPL");
