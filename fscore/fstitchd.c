/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>

#include <fscore/sync.h>
#include <fscore/sched.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>
#include <fscore/debug.h>
#include <fscore/fstitchd.h>
#include <fscore/fstitchd_init.h>
#include <fscore/destroy.h>

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

#if ALLOW_CRASHSIM
int use_crashsim = 0;
#endif

struct module_shutdown {
	const char * name;
	fstitchd_shutdown_module shutdown;
	void * arg;
	int when;
};

#define MAX_NR_SHUTDOWNS 16
static struct module_shutdown module_shutdowns[MAX_NR_SHUTDOWNS];

int _fstitchd_register_shutdown_module(const char * name, fstitchd_shutdown_module fn, void * arg, int when)
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

static void fstitchd_callback_shutdowns(int when)
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

static volatile int fstitchd_running = 0;

// Shutdown fstitchd: inform modules of impending shutdown, then exit.
static void fstitchd_shutdown(void)
{
	printf("Syncing and shutting down");
#if FSTITCH_DEBUG
	printf(" (debug = %d)", FSTITCH_DEBUG_COUNT());
#endif
	printf(".\n");
	if(fstitchd_running > 0)
		fstitchd_running = 0;
	
	if(fstitch_sync() < 0)
		fprintf(stderr, "Sync failed!\n");

	Dprintf("Calling pre-shutdown callbacks.\n");
	fstitchd_callback_shutdowns(SHUTDOWN_PREMODULES);

	// Reclaim patches written by sync and shutdowns so that when destroy_all()
	// destroys BDs that destroy a blockman no ddescs are orphaned.
	Dprintf("Reclaiming written patches.\n");
	patch_reclaim_written();

	Dprintf("Destroying all modules.\n");
	destroy_all();

	Dprintf("Running block descriptor autoreleasing.\n");
	// Run bdesc autoreleasing
	if (bdesc_autorelease_pool_depth() > 0)
	{
		bdesc_autorelease_pool_pop();
		assert(!bdesc_autorelease_pool_depth());
	}

	// Run patch reclamation
	Dprintf("Reclaiming written patches.\n");
	patch_reclaim_written();

	Dprintf("Calling post-shutdown callbacks.\n");
	fstitchd_callback_shutdowns(SHUTDOWN_POSTMODULES);
}

void fstitchd_request_shutdown(void)
{
	fstitchd_running = 0;
}

int fstitchd_is_running(void)
{
	return fstitchd_running > 0;
}

#ifdef __KERNEL__

#include <fscore/kernel_serve.h>
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

struct task_struct * fstitchd_task;
struct stealth_lock fstitchd_global_lock;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 19)
static void fsttich_sysrq_unlock(int key, struct pt_regs * regs, struct tty_struct * tty)
#else
static void fsttich_sysrq_unlock(int key, struct tty_struct * tty)
#endif
{
	spin_lock(&fstitchd_global_lock.lock);
	fstitchd_global_lock.locked = 0;
	fstitchd_global_lock.process = 0;
	spin_unlock(&fstitchd_global_lock.lock);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 18)
/* By default, print_stack_trace() is not exported to modules.
 * It's defined in kernel/stacktrace.c, and you can just add
 * #include <linux/module.h> and EXPORT_SYMBOL(print_stack_trace);
 * there to export it. Afterward, change 0 to 1 below. */
#define EXPORTED_PRINT_STACK 0
#define PRINT_STACK_DEPTH 128

#if defined(CONFIG_STACKTRACE) && EXPORTED_PRINT_STACK
static void fsttich_sysrq_showlock(int key, struct tty_struct * tty)
{
	spin_lock(&fstitchd_global_lock.lock);
	if(fstitchd_global_lock.locked)
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
		task = find_task_by_pid_type(PIDTYPE_PID, fstitchd_global_lock.process);
		save_stack_trace(&trace, task);
		rcu_read_unlock();
		print_stack_trace(&trace, 0);
	}
	spin_unlock(&fstitchd_global_lock.lock);
}
#endif
#endif

static struct {
	int key;
	struct sysrq_key_op op;
} fstitchd_sysrqs[] = {
	{'c', {handler: fsttich_sysrq_unlock, help_msg: "fstitchd_unlock(C)", action_msg: "Unlocked fstitchd_lock", enable_mask: 1}},
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 18)
#if defined(CONFIG_STACKTRACE) && EXPORTED_PRINT_STACK
	{'w', {handler: fsttich_sysrq_showlock, help_msg: "fstitchd_tracelock(W)", action_msg: "Showing fstitchd_lock owner trace", enable_mask: 1}},
#endif
#endif
};
#define FSTITCHD_SYSRQS (sizeof(fstitchd_sysrqs) / sizeof(fstitchd_sysrqs[0]))

static void fstitchd_main(int nwbblocks)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	fstitchd_enter();
	if ((r = fstitchd_init(nwbblocks)) < 0)
	{
		printf("fstitchd_init() failed in the kernel! (error = %d)\n", r);
		fstitchd_running = r;
	}
	else
	{
		fstitchd_running = 1;
		while(fstitchd_running)
		{
			sched_run_callbacks();
			fstitchd_leave(0);
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ / 25);
			fstitchd_enter();
		}
	}
	fstitchd_shutdown();
	fstitchd_leave(0);
}

static int nwbblocks = 40000;
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

#if ALLOW_CRASHSIM
module_param(use_crashsim, int, 0);
MODULE_PARM_DESC(use_crashsim, "Use crash simulator module");
#endif

int fstitchd_is_shutdown = 0;

static int fstitchd_thread(void * thunk)
{
	int i;
	printf("kfstitchd started (PID = %d)\n", current ? current->pid : 0);
	daemonize("kfstitchd");
	fstitchd_task = current;
	spin_lock_init(&fstitchd_global_lock.lock);
	fstitchd_global_lock.locked = 0;
	fstitchd_global_lock.process = 0;
	for(i = 0; i < FSTITCHD_SYSRQS; i++)
		if(register_sysrq_key(fstitchd_sysrqs[i].key, &fstitchd_sysrqs[i].op) < 0)
			printf("kfstitchd unable to register sysrq[%c] (%d/%d)\n", fstitchd_sysrqs[i].key, i + 1, FSTITCHD_SYSRQS);
	Dprintf("Running fstitchd_main()\n");
	fstitchd_main(nwbblocks);
	Dprintf("fstitchd_main() completed\n");
	for(i = 0; i < FSTITCHD_SYSRQS; i++)
		if(unregister_sysrq_key(fstitchd_sysrqs[i].key, &fstitchd_sysrqs[i].op) < 0)
			printf("kfstitchd unable to unregister sysrq[%c] (%d/%d)\n", fstitchd_sysrqs[i].key, i + 1, FSTITCHD_SYSRQS);
	printf("kfstitchd exiting (PID = %d)\n", current ? current->pid : 0);
	fstitchd_is_shutdown = 1;
	return 0;
}

static int __init init_fstitchd(void)
{
	pid_t pid = kernel_thread(fstitchd_thread, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	if(pid < 0)
		printf("kfstitchd unable to start kernel thread!\n");
	while(!fstitchd_running && !signal_pending(current))
	{
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 10);
	}
	/* FIXME: we should kill the kernel thread if fstitchd_running is false */
	return (fstitchd_running > 0) ? 0 : fstitchd_running;
}

static void __exit exit_fstitchd(void)
{
	fstitchd_request_shutdown();
	while(!fstitchd_is_shutdown)
	{
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 10);
	}
}

module_init(init_fstitchd);
module_exit(exit_fstitchd);

MODULE_AUTHOR("Featherstitch Team");
MODULE_DESCRIPTION("Featherstitch File System Architecture");
MODULE_LICENSE("GPL");

#elif defined(UNIXUSER)

#include <fscore/fuse_serve.h>
#include <unistd.h>

static void fstitchd_main(int nwbblocks)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	if ((r = fstitchd_init(nwbblocks)) < 0)
	{
		printf("fstitchd_init() failed! (error = %d)\n", r);
		fstitchd_running = r;
	}
	else
	{
		fstitchd_running = 1;
		fuse_serve_loop();
	}
	fstitchd_shutdown();
}

char * unix_file = NULL;
int fstitchd_argc = 0;
char ** fstitchd_argv = NULL;

static void remove_arg(int * argc, char ** argv, int idx)
{
	int i;
	--*argc;
	for(i = idx; i < *argc; i++)
		argv[i] = argv[i + 1];
}

int main(int argc, char * argv[])
{
	int i, nwbblocks = 20000;
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
#if ALLOW_CRASHSIM
		else if(!strncmp(argv[i], "use_crashsim=", 13))
		{
			use_crashsim = atoi(&argv[i][13]);
			remove_arg(&argc, argv, i--);
		}
#endif
		else if (!strncmp(argv[i], "blocklog=", 9)) {
			setenv("BLOCK_LOG", argv[i] + 9, 1);
			remove_arg(&argc, argv, i--);
		}
	}
	fstitchd_argc = argc;
	fstitchd_argv = argv;
	
	printf("ufstitchd started (PID = %d)\n", getpid());
	Dprintf("Running fstitchd_main()\n");
	fstitchd_main(nwbblocks);
	Dprintf("fstitchd_main() completed\n");
	printf("ufstitchd exiting (PID = %d)\n", getpid());
	return 0;
}

#endif
