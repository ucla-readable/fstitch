#include <inc/error.h>
#include <lib/assert.h>
#include <lib/mmu.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>

#if defined(KUDOS)
#include <kfs/ipc_serve.h>
#elif defined(UNIXUSER)
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <kfs/fuse_serve.h>
#elif defined(__KERNEL__)
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/stacktrace.h>
#include <linux/sysrq.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
#include <linux/config.h>
#endif
#include <kfs/kernel_serve.h>
#endif

#include <kfs/sync.h>
#include <kfs/sched.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>
#include <kfs/destroy.h>

#ifdef __KERNEL__
struct task_struct * kfsd_task;
struct stealth_lock kfsd_global_lock;

static void kudos_sysrq_unlock(int key, struct tty_struct *tty)
{
	spin_lock(&kfsd_global_lock.lock);
	kfsd_global_lock.locked = 0;
	kfsd_global_lock.process = 0;
	spin_unlock(&kfsd_global_lock.lock);
}

/* By default, print_stack_trace() is not exported to modules.
 * It's defined in kernel/stacktrace.c, and you can just add
 * #include <linux/module.h> and EXPORT_SYMBOL(print_stack_trace);
 * there to export it. Afterward, change 0 to 1 below. */
#define EXPORTED_PRINT_STACK 0
#define PRINT_STACK_DEPTH 128

#if defined(CONFIG_STACKTRACE) && EXPORTED_PRINT_STACK
static void kudos_sysrq_showlock(int key, struct tty_struct *tty)
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
		print_stack_trace(&trace, 2);
	}
	spin_unlock(&kfsd_global_lock.lock);
}
#endif

static struct {
	int key;
	struct sysrq_key_op op;
} kfsd_sysrqs[2] = {
	{'x', {handler: kudos_sysrq_unlock, help_msg: "unlock kfsd_lock (x)", action_msg: "Unlocked kfsd_lock", enable_mask: 1}},
#if defined(CONFIG_STACKTRACE) && EXPORTED_PRINT_STACK
	{'y', {handler: kudos_sysrq_showlock, help_msg: "trace kfsd_lock owner (y)", action_msg: "Showing kfsd_lock owner trace", enable_mask: 1}},
#endif
};
#define KFSD_SYSRQS (sizeof(kfsd_sysrqs) / sizeof(kfsd_sysrqs[0]))
#endif

struct module_shutdown {
	kfsd_shutdown_module shutdown;
	void * arg;
	int when;
};

static struct module_shutdown module_shutdowns[10];

int kfsd_register_shutdown_module(kfsd_shutdown_module fn, void * arg, int when)
{
	int i;

	if (when != SHUTDOWN_PREMODULES && when != SHUTDOWN_POSTMODULES)
		return -E_INVAL;

	for (i = 0; i < sizeof(module_shutdowns)/sizeof(module_shutdowns[0]); i++)
	{
		if (!module_shutdowns[i].shutdown)
		{
			module_shutdowns[i].shutdown = fn;
			module_shutdowns[i].arg = arg;
			module_shutdowns[i].when = when;
			return 0;
		}
	}

	return -E_NO_MEM;
}

static void kfsd_callback_shutdowns(int when)
{
	size_t i;
	for (i = 0; i < sizeof(module_shutdowns)/sizeof(module_shutdowns[0]); i++)
	{
		if (module_shutdowns[i].shutdown && module_shutdowns[i].when == when)
		{
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

	kfsd_callback_shutdowns(SHUTDOWN_PREMODULES);

	// Reclaim chdescs written by sync and shutdowns so that when destroy_all()
	// destroys BDs that destroy a blockman no ddescs are orphaned.
	chdesc_reclaim_written();

	destroy_all();

	// Run bdesc autoreleasing
	if (bdesc_autorelease_pool_depth() > 0)
	{
		bdesc_autorelease_pool_pop();
		assert(!bdesc_autorelease_pool_depth());
	}

	// Run chdesc reclamation
	chdesc_reclaim_written();

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

void kfsd_main(int nwbblocks, int argc, char ** argv)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

#ifdef __KERNEL__
	kfsd_enter();
#endif
	if ((r = kfsd_init(nwbblocks, argc, argv)) < 0)
	{
#ifdef __KERNEL__
		printk("kfsd_init() failed in the kernel! (error = %d)\n", r);
		kfsd_running = r;
#else
		kfsd_shutdown();
		exit(r);
#endif
	}
	else
	{
		kfsd_running = 1;
#if defined(UNIXUSER)
		/* fuse_serve_loop() doesn't respect kfsd_running()... but that's OK */
		fuse_serve_loop();
#else
		while(kfsd_running)
		{
			sched_run_callbacks();
#if defined(KUDOS)
			ipc_serve_run(); // Run ipc_serve (which will sleep for a bit)
			sched_run_cleanup();
#elif defined(__KERNEL__)
			kfsd_leave(0);
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ / 25);
			kfsd_enter();
#else
#error Unknown target system
#endif
		}
#endif
	}
	kfsd_shutdown();
#ifdef __KERNEL__
	kfsd_leave(0);
#endif
}

#if defined(KUDOS)

#include <inc/lib.h> // binaryname
void umain(int argc, char * argv[])
{
	int r, i;

	if(!argc)
	{
		binaryname = "kfsd";
		if((r = sys_env_set_name(0, "kfsd")) < 0)
		{
			printf("Failed to set env name: %i\n", r);
			return;
		}
	}
	
	if((r = sys_grant_io(0)) < 0)
	{
		printf("Failed to get I/O priveleges: %i\n", r);
		return;
	}
	/*if((r = sys_env_set_priority(0, ENV_MAX_PRIORITY)) < 0)
	{
		printf("Failed to set priority: %i\n", r);
		return;
	}*/

	// Allocate more pages for the stack because we sometimes need it (for chdesc graph traversal)
	for(i = 2; i != 33; i++)
	{
		r = sys_page_alloc(0, (void *) (USTACKTOP - i * PGSIZE), PTE_U | PTE_W | PTE_P);
		assert(r >= 0);
	}

	kfsd_main(128, argc, argv);
}

#elif defined(UNIXUSER)

#include <limits.h>

int main(int argc, char * argv[])
{
	// Limit stack size to "not exceed" the linux kernel's 8kB stack.
	// NOTE: linux appears to allocate a "large" initial stack (on chris's
	// laptop, 80kB). setrlimit() does not appear to shrink this allocation,
	// merely prevent its growth.
	rlim_t stack_limit = 6 * 1024;
	struct rlimit rlimit = {.rlim_cur = stack_limit, .rlim_max = stack_limit};
	int r = setrlimit(RLIMIT_STACK, &rlimit);
	int nwbblocks = 128;
	if(r < 0)
	{
		perror("setrlimit()");
		return 1;
	}

	if(getenv("NWBBLOCKS"))
	{
		nwbblocks = strtol(getenv("NWBBLOCKS"), NULL, 0);
		if(nwbblocks == LONG_MAX || nwbblocks == LONG_MIN)
		{
			perror("NWBBLOCKS not in range, strtol()");
			return 1;
		}
	}

	kfsd_main(nwbblocks, argc, argv);
	return 0;
}

#elif defined(__KERNEL__)

static int nwbblocks = 128;
module_param(nwbblocks, int, 0);

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
			printf("kkfsd unable to register sysrq[%c] (%d/%d)\n", kfsd_sysrqs[i].key, i + 1, KFSD_SYSRQS);
	kfsd_main(nwbblocks, 0, NULL);
	for(i = 0; i < KFSD_SYSRQS; i++)
		if(unregister_sysrq_key(kfsd_sysrqs[i].key, &kfsd_sysrqs[i].op) < 0)
			printf("kkfsd unable to unregister sysrq[%c] (%d/%d)\n", kfsd_sysrqs[i].key, i + 1, KFSD_SYSRQS);
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

MODULE_AUTHOR("KudOS Team");
MODULE_DESCRIPTION("KudOS File System Architecture");
MODULE_LICENSE("GPL");

#else
#error Unknown target system
#endif
