/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>

#include <fscore/kernel_patchgroup_ops.h>

#include <linux/version.h>
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2, 6, 18)
#include <linux/config.h>
#endif

#ifdef CONFIG_FSTITCH_PROC
#include <linux/blkdev.h>
#include <linux/spinlock.h>

#include <fscore/fstitchd.h>
#include <fscore/patchgroup.h>
#include <fscore/kernel_serve.h>
#include <fscore/kernel_patchgroup_ioctl.h>

/* Limit strings to something quite reasonable */
#define STR_LEN_MAX 128

static int kernel_patchgroup_ioctl(struct inode * inode, struct file * filp, unsigned int cmd, unsigned long arg)
{
	patchgroup_ioctl_cmd_t cmd_args;
	patchgroup_t * patchgroup_a = NULL;
	patchgroup_t * patchgroup_b = NULL;
	char str[STR_LEN_MAX];
	char * tmp;
	int r;

	if (copy_from_user((void *) &cmd_args, (void __user *) arg, sizeof(cmd_args)))
		return -EFAULT;

	fstitchd_enter();
	if (cmd_args.patchgroup_a >= 0)
		patchgroup_a = patchgroup_lookup(cmd_args.patchgroup_a);
	if (cmd_args.patchgroup_b >= 0)
		patchgroup_b = patchgroup_lookup(cmd_args.patchgroup_b);
	if (cmd_args.str && cmd != PATCHGROUP_IOCTL_TXN_START)
	{
		long len = strnlen_user(cmd_args.str, STR_LEN_MAX);
		if (len < 1 || STR_LEN_MAX < len)
			return -EFAULT;
		if (copy_from_user(str, (void __user *) cmd_args.str, len))
			return -EFAULT;
	}

	switch (cmd)
	{
		case PATCHGROUP_IOCTL_CREATE:
			r = patchgroup_id(patchgroup_create(cmd_args.flags));
			break;
		case PATCHGROUP_IOCTL_SYNC:
			r = patchgroup_sync(patchgroup_a);
			break;
		case PATCHGROUP_IOCTL_ADD_DEPEND:
			r = patchgroup_add_depend(patchgroup_a, patchgroup_b);
			break;
		case PATCHGROUP_IOCTL_ENGAGE:
			r = patchgroup_engage(patchgroup_a);
			break;
		case PATCHGROUP_IOCTL_DISENGAGE:
			r = patchgroup_disengage(patchgroup_a);
			break;
		case PATCHGROUP_IOCTL_RELEASE:
			r = patchgroup_release(patchgroup_a);
			break;
		case PATCHGROUP_IOCTL_ABANDON:
			r = patchgroup_abandon(&patchgroup_a);
			break;
		case PATCHGROUP_IOCTL_LABEL:
			r = patchgroup_label(patchgroup_a, str);
			break;
		case PATCHGROUP_IOCTL_TXN_START:
			tmp = getname(cmd_args.str);
			r = PTR_ERR(tmp);
			if (!IS_ERR(tmp))
			{
				r = txn_start(tmp);
				putname(tmp);
			}
			break;
		case PATCHGROUP_IOCTL_TXN_FINISH:
			r = txn_finish();
			break;
		case PATCHGROUP_IOCTL_TXN_ABORT:
			r = txn_abort();
			break;
		default:
			r = -ENOTTY;
	}

	fstitchd_leave(1);

	return r;
}


static void kernel_patchgroup_process_request_queue(request_queue_t * q)
{
	struct request *req;
	while ((req = elv_next_request(q)) != NULL)
		fprintf(stderr, "%s: requests are not allowed\n", __FUNCTION__);
}


static struct block_device_operations kernel_patchgroup_dev_ops = {
	.owner = THIS_MODULE,
	.ioctl = kernel_patchgroup_ioctl
};

struct state {
	struct request_queue * queue;
	spinlock_t queue_lock;
	struct gendisk * gd;
};
static struct state state;


static void kernel_patchgroup_ops_shutdown(void * ignored)
{
	assert(state.gd);
	del_gendisk(state.gd);
	put_disk(state.gd);
	unregister_blkdev(PATCHGROUP_MAJOR, PATCHGROUP_DEVICE);
	state.gd = NULL;
}

int kernel_patchgroup_ops_init(void)
{
	int r;

	r = register_blkdev(PATCHGROUP_MAJOR, PATCHGROUP_DEVICE);
	if (r < 0)
	{
		fprintf(stderr, "%s: unable to get major number\n", __FUNCTION__);
		return -EBUSY;
	}

	spin_lock_init(&state.queue_lock);
	if (!(blk_init_queue(kernel_patchgroup_process_request_queue, &state.queue_lock)))
	{
		fprintf(stderr, "%s: blk_init_queue() failed\n", __FUNCTION__);
		unregister_blkdev(PATCHGROUP_MAJOR, PATCHGROUP_DEVICE);
		return -1;
	}
	if (!(state.gd = alloc_disk(1)))
	{
		fprintf(stderr, "%s: alloc_disk() failed\n", __FUNCTION__);
		unregister_blkdev(PATCHGROUP_MAJOR, PATCHGROUP_DEVICE);
		return -1;
	}
	state.gd->major = PATCHGROUP_MAJOR;
	state.gd->first_minor = 0;
	state.gd->fops = &kernel_patchgroup_dev_ops;
	state.gd->queue = state.queue;
	snprintf(state.gd->disk_name, 32, "%s", PATCHGROUP_DEVICE);
	set_capacity(state.gd, 0);
	add_disk(state.gd);

	r = fstitchd_register_shutdown_module(kernel_patchgroup_ops_shutdown, NULL, SHUTDOWN_PREMODULES);
	if (r < 0)
	{
		kernel_patchgroup_ops_shutdown(NULL);
		return r;
	}

	return 0;
}

#else

int kernel_patchgroup_ops_init(void)
{
	/* a message is printed that there is no support in kernel_patchgroup_scopes_init() */
	return 0;
}

#endif
