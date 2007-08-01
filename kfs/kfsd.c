#include <lib/platform.h>

#include <kfs/sync.h>
#include <kfs/sched.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/debug.h>
#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>
#include <kfs/destroy.h>

#define DEBUG_TOPLEVEL 0
#if DEBUG_TOPLEVEL
#define Dprintf(format, args...) printf(format, ##args)
#else
#define Dprintf(...)
#endif

#if ALLOW_JOURNAL
int use_journal = 0;
#endif

#if ALLOW_UNLINK
int use_unlink = 0;
#endif

#if ALLOW_UNSAFE_DISK_CACHE
int use_unsafe_disk_cache = 0;
#endif

struct module_shutdown {
	const char * name;
	kfsd_shutdown_module shutdown;
	void * arg;
	int when;
};

#define MAX_NR_SHUTDOWNS 16
static struct module_shutdown module_shutdowns[MAX_NR_SHUTDOWNS];

int _kfsd_register_shutdown_module(const char * name, kfsd_shutdown_module fn, void * arg, int when)
{
	int i;

	if (when != SHUTDOWN_PREMODULES && when != SHUTDOWN_POSTMODULES)
		return -EINVAL;

	for (i = 0; i < MAX_NR_SHUTDOWNS; i++)
	{
		if (!module_shutdowns[i].shutdown)
		{
			Dprintf("Registering shutdown callback: %s\n", name);
			module_shutdowns[i].name = name;
			module_shutdowns[i].shutdown = fn;
			module_shutdowns[i].arg = arg;
			module_shutdowns[i].when = when;
			return 0;
		}
	}

	printf("%s(): too many shutdown modules!\n", __FUNCTION__);
	return -ENOMEM;
}

static void kfsd_callback_shutdowns(int when)
{
	int i;
	for (i = MAX_NR_SHUTDOWNS - 1; i >= 0; i--)
	{
		if (module_shutdowns[i].shutdown && module_shutdowns[i].when == when)
		{
			Dprintf("Calling shutdown callback: %s\n", module_shutdowns[i].name);
			module_shutdowns[i].shutdown(module_shutdowns[i].arg);
			module_shutdowns[i].shutdown = NULL;
			module_shutdowns[i].arg = NULL;
			module_shutdowns[i].when = 0;
		}
	}
}

static volatile int kfsd_running = 0;

// Shutdown kfsd: inform modules of impending shutdown, then exit.
static void kfsd_shutdown(void)
{
	printf("Syncing and shutting down");
#if KFS_DEBUG
	printf(" (debug = %d)", KFS_DEBUG_COUNT());
#endif
	printf(".\n");
	if(kfsd_running > 0)
		kfsd_running = 0;
	
	if(kfs_sync() < 0)
		fprintf(stderr, "Sync failed!\n");

	Dprintf("Calling pre-shutdown callbacks.\n");
	kfsd_callback_shutdowns(SHUTDOWN_PREMODULES);

	// Reclaim chdescs written by sync and shutdowns so that when destroy_all()
	// destroys BDs that destroy a blockman no ddescs are orphaned.
	Dprintf("Reclaiming written change descriptors.\n");
	chdesc_reclaim_written();

	Dprintf("Destroying all modules.\n");
	destroy_all();

	Dprintf("Running block descriptor autoreleasing.\n");
	// Run bdesc autoreleasing
	if (bdesc_autorelease_pool_depth() > 0)
	{
		bdesc_autorelease_pool_pop();
		assert(!bdesc_autorelease_pool_depth());
	}

	// Run chdesc reclamation
	Dprintf("Reclaiming written change descriptors.\n");
	chdesc_reclaim_written();

	Dprintf("Calling post-shutdown callbacks.\n");
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

#ifdef __KERNEL__

#include <kfs/kernel_serve.h>
#include <linux/version.h>
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/sysrq.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 18)
# include <linux/stacktrace.h>
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

static void kfsd_main(int nwbblocks)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	kfsd_enter();
	if ((r = kfsd_init(nwbblocks)) < 0)
	{
		printf("kfsd_init() failed in the kernel! (error = %d)\n", r);
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
module_param_named(device, linux_device, charp, 0);
MODULE_PARM_DESC(device, "The device to attach linux_bd to");

/* provide reverse compatible linux_device parameter */
module_param(linux_device, charp, 0);
MODULE_PARM_DESC(linux_device, "Alias for device");

#if ALLOW_JOURNAL
module_param(use_journal, int, 0);
MODULE_PARM_DESC(use_journal, "Use journal device when .journal exists");
#endif

#if ALLOW_UNLINK
module_param(use_unlink, int, 0);
MODULE_PARM_DESC(use_unlink, "Use the unlink device to remove dependencies");
#endif

#if ALLOW_UNSAFE_DISK_CACHE
module_param(use_unsafe_disk_cache, int, 0);
MODULE_PARM_DESC(use_unsafe_disk_cache, "Use disk cache unsafely");
#endif

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
	Dprintf("Running kfsd_main()\n");
	kfsd_main(nwbblocks);
	Dprintf("kfsd_main() completed\n");
	for(i = 0; i < KFSD_SYSRQS; i++)
		if(unregister_sysrq_key(kfsd_sysrqs[i].key, &kfsd_sysrqs[i].op) < 0)
			printf("kkfsd unable to unregister sysrq[%c] (%d/%d)\n", kfsd_sysrqs[i].key, i + 1, KFSD_SYSRQS);
	printf("kkfsd exiting (PID = %d)\n", current ? current->pid : 0);
	kfsd_is_shutdown = 1;
	return 0;
}

static int __init init_kfsd(void)
{
	pid_t pid = kernel_thread(kfsd_thread, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	if(pid < 0)
		printf("kkfsd unable to start kernel thread!\n");
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

#elif defined(UNIXUSER)

#include <kfs/fuse_serve.h>
#include <unistd.h>

static void kfsd_main(int nwbblocks)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	if ((r = kfsd_init(nwbblocks)) < 0)
	{
		printf("kfsd_init() failed! (error = %d)\n", r);
		kfsd_running = r;
	}
	else
	{
		kfsd_running = 1;
		fuse_serve_loop();
	}
	kfsd_shutdown();
}

char * unix_file = NULL;
int kfsd_argc = 0;
char ** kfsd_argv = NULL;

static void remove_arg(int * argc, char ** argv, int idx)
{
	int i;
	--*argc;
	for(i = idx; i < *argc; i++)
		argv[i] = argv[i + 1];
}

int main(int argc, char * argv[])
{
	int i, nwbblocks = 16384;
	for(i = 1; i < argc; i++)
	{
		if(!strcmp(argv[i], "--help"))
		{
			printf("nwbblocks=<The number of write-back blocks to use>\n");
			printf("unix_file=<The device to attach unix_file_bd to>\n");
			printf("use -h for help on fuse options\n");
			return 0;
		}
		else if(!strncmp(argv[i], "nwbblocks=", 10))
		{
			nwbblocks = atoi(&argv[i][10]);
			remove_arg(&argc, argv, i--);
		}
		else if(!strncmp(argv[i], "unix_file=", 10))
		{
			unix_file = &argv[i][10];
			remove_arg(&argc, argv, i--);
		} else if (strlen(argv[i]) > 4 && strcmp(argv[i] + strlen(argv[i]) - 4, ".img") == 0) {
			unix_file = argv[i];
			remove_arg(&argc, argv, i--);
		}
#if ALLOW_JOURNAL
		else if(!strncmp(argv[i], "use_journal=", 12))
		{
			use_journal = atoi(&argv[i][12]);
			remove_arg(&argc, argv, i--);
		}
#endif
#if ALLOW_UNLINK
		else if(!strncmp(argv[i], "use_unlink=", 11))
		{
			use_unlink = atoi(&argv[i][11]);
			remove_arg(&argc, argv, i--);
		}
#endif
#if ALLOW_UNSAFE_DISK_CACHE
		#define UDC_PARAMNAME "use_unsafe_disk_cache="
		else if(!strncmp(argv[i], UDC_PARAMNAME, strlen(UDC_PARAMNAME)))
		{
			use_unsafe_disk_cache = atoi(&argv[i][strlen(UDC_PARAMNAME)]);
			remove_arg(&argc, argv, i--);
		}
#endif
		else if (!strncmp(argv[i], "blocklog=", 9)) {
			setenv("BLOCK_LOG", argv[i] + 9, 1);
			remove_arg(&argc, argv, i--);
		}
		else
		{
			printf("Ignoring parameter \"%s\"\n", argv[i]);
			remove_arg(&argc, argv, i--);
		}
	}
	kfsd_argc = argc;
	kfsd_argv = argv;
	
	printf("ukfsd started (PID = %d)\n", getpid());
	Dprintf("Running kfsd_main()\n");
	kfsd_main(nwbblocks);
	Dprintf("kfsd_main() completed\n");
	printf("ukfsd exiting (PID = %d)\n", getpid());
	return 0;
}

#endif
