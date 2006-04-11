#include <inc/error.h>
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
#include <kfs/kernel_serve.h>
#endif

#include <kfs/sync.h>
#include <kfs/sched.h>
#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>

#ifdef __KERNEL__
struct task_struct * kfsd_task;
struct stealth_lock kfsd_global_lock;
#endif

struct module_shutdown {
	kfsd_shutdown_module shutdown;
	void * arg;
};

static struct module_shutdown module_shutdowns[10];

int kfsd_register_shutdown_module(kfsd_shutdown_module fn, void * arg)
{
	int i;

	for (i = 0; i < sizeof(module_shutdowns)/sizeof(module_shutdowns[0]); i++)
	{
		if (!module_shutdowns[i].shutdown)
		{
			module_shutdowns[i].shutdown = fn;
			module_shutdowns[i].arg = arg;
			return 0;
		}
	}

	return -E_NO_MEM;
}

static int kfsd_running = 0;

// Shutdown kfsd: inform modules of impending shutdown, then exit.
static void kfsd_shutdown(void)
{
	int i;
	
	printf("Syncing and shutting down.\n");
	if(kfsd_running > 0)
		kfsd_running = 0;
	
	if(kfs_sync() < 0)
		kdprintf(STDERR_FILENO, "Sync failed!\n");

	for (i = 0; i < sizeof(module_shutdowns)/sizeof(module_shutdowns[0]); i++)
	{
		if (module_shutdowns[i].shutdown)
		{
			module_shutdowns[i].shutdown(module_shutdowns[i].arg);
			module_shutdowns[i].shutdown = NULL;
			module_shutdowns[i].arg = NULL;
		}
	}
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

void kfsd_main(int argc, char ** argv)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	if ((r = kfsd_init(argc, argv)) < 0)
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
#ifdef __KERNEL__
		kfsd_enter();
#endif
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
#ifdef __KERNEL__
		kfsd_leave(0);
#endif
#endif
	}
	kfsd_shutdown();
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

	kfsd_main(argc, argv);
}

#elif defined(UNIXUSER)

int main(int argc, char * argv[])
{
	// limit stack size to not exceed the linux kernel's 8kB stack
	rlim_t stack_limit = 6 * 1024;
	struct rlimit rlimit = {.rlim_cur = stack_limit, .rlim_max = stack_limit};
	int r = setrlimit(RLIMIT_STACK, &rlimit);
	if(r < 0)
	{
		perror("setrlimit()");
		return 1;
	}

	kfsd_main(argc, argv);
	return 0;
}

#elif defined(__KERNEL__)

static int kfsd_is_shutdown = 0;

static int kfsd_thread(void * thunk)
{
	printf("kkfsd started (PID = %d)\n", current ? current->pid : 0);
	daemonize("kkfsd");
	kfsd_task = current;
	spin_lock_init(&kfsd_global_lock.lock);
	kfsd_global_lock.locked = 0;
	kfsd_global_lock.process = 0;
	kfsd_main(0, NULL);
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
MODULE_DESCRIPTION("KudOS File Server Architecture");
MODULE_LICENSE("GPL");

#else
#error Unknown target system
#endif
