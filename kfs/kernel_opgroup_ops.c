#include <kfs/kernel_opgroup_ops.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
#include <linux/config.h>
#endif

#ifdef CONFIG_KUDOS_PROC
#include <linux/blkdev.h>
#include <linux/spinlock.h>

#include <lib/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <kfs/kfsd.h>
#include <kfs/opgroup.h>
#include <kfs/kernel_serve.h>
#include <kfs/kernel_opgroup_ioctl.h>

/* Limit strings to something quite reasonable */
#define STR_LEN_MAX 128

static int kernel_opgroup_ioctl(struct inode * inode, struct file * filp, unsigned int cmd, unsigned long arg)
{
	opgroup_ioctl_cmd_t cmd_args;
	opgroup_t * opgroup_a = NULL;
	opgroup_t * opgroup_b = NULL;
	char str[STR_LEN_MAX];
	int r;

	if (copy_from_user((void *) &cmd_args, (void __user *) arg, sizeof(cmd_args)))
		return -E_FAULT;

	kfsd_enter();
	if (cmd_args.opgroup_a >= 0)
		opgroup_a = opgroup_lookup(cmd_args.opgroup_a);
	if (cmd_args.opgroup_b >= 0)
		opgroup_b = opgroup_lookup(cmd_args.opgroup_b);
	if (cmd_args.str)
	{
		long len = strnlen_user(cmd_args.str, STR_LEN_MAX);
		if (len < 1 || STR_LEN_MAX < len)
			return -E_FAULT;
		if (copy_from_user(str, (void __user *) cmd_args.str, len))
			return -E_FAULT;
	}

	switch (cmd)
	{
		case OPGROUP_IOCTL_CREATE:
			r = opgroup_id(opgroup_create(cmd_args.flags));
			break;
		case OPGROUP_IOCTL_SYNC:
			r = opgroup_sync(opgroup_a);
			break;
		case OPGROUP_IOCTL_ADD_DEPEND:
			r = opgroup_add_depend(opgroup_a, opgroup_b);
			break;
		case OPGROUP_IOCTL_ENGAGE:
			r = opgroup_engage(opgroup_a);
			break;
		case OPGROUP_IOCTL_DISENGAGE:
			r = opgroup_disengage(opgroup_a);
			break;
		case OPGROUP_IOCTL_RELEASE:
			r = opgroup_release(opgroup_a);
			break;
		case OPGROUP_IOCTL_ABANDON:
			r = opgroup_abandon(&opgroup_a);
			break;
		case OPGROUP_IOCTL_LABEL:
			r = opgroup_label(opgroup_a, str);
			break;
		default:
			r = -ENOTTY;
	}

	kfsd_leave(1);

	return r;
}


static void kernel_opgroup_process_request_queue(request_queue_t * q)
{
	struct request *req;
	while ((req = elv_next_request(q)) != NULL)
		kdprintf(STDERR_FILENO, "%s: requests are not allowed\n", __FUNCTION__);
}


static struct block_device_operations kernel_opgroup_dev_ops = {
	.owner = THIS_MODULE,
	.ioctl = kernel_opgroup_ioctl
};

struct state {
	struct request_queue * queue;
	spinlock_t queue_lock;
	struct gendisk * gd;
};
static struct state state;


static void kernel_opgroup_ops_shutdown(void * ignored)
{
	assert(state.gd);
	del_gendisk(state.gd);
	put_disk(state.gd);
	unregister_blkdev(OPGROUP_MAJOR, OPGROUP_DEVICE);
	state.gd = NULL;
}

int kernel_opgroup_ops_init(void)
{
	int r;

	r = register_blkdev(OPGROUP_MAJOR, OPGROUP_DEVICE);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "%s: unable to get major number\n", __FUNCTION__);
		return -E_BUSY;
	}

	spin_lock_init(&state.queue_lock);
	if (!(blk_init_queue(kernel_opgroup_process_request_queue, &state.queue_lock)))
	{
		kdprintf(STDERR_FILENO, "%s: blk_init_queue() failed\n", __FUNCTION__);
		unregister_blkdev(OPGROUP_MAJOR, OPGROUP_DEVICE);
		return -E_UNSPECIFIED;
	}
	if (!(state.gd = alloc_disk(1)))
	{
		kdprintf(STDERR_FILENO, "%s: alloc_disk() failed\n", __FUNCTION__);
		unregister_blkdev(OPGROUP_MAJOR, OPGROUP_DEVICE);
		return -E_UNSPECIFIED;
	}
	state.gd->major = OPGROUP_MAJOR;
	state.gd->first_minor = 0;
	state.gd->fops = &kernel_opgroup_dev_ops;
	state.gd->queue = state.queue;
	snprintf(state.gd->disk_name, 32, "%s", OPGROUP_DEVICE);
	set_capacity(state.gd, 0);
	add_disk(state.gd);

	r = kfsd_register_shutdown_module(kernel_opgroup_ops_shutdown, NULL, SHUTDOWN_PREMODULES);
	if (r < 0)
	{
		kernel_opgroup_ops_shutdown(NULL);
		return r;
	}

	return 0;
}

#else

int kernel_opgroup_ops_init(void)
{
	/* a message is printed that there is no support in kernel_opgroup_scopes_init() */
	return 0;
}

#endif
