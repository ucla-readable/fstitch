/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

/* Make sure we get the structures from debug_opcode.h */
#define WANT_DEBUG_STRUCTURES 1

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/sleep.h>

#include <lib/platform.h>

#ifdef __KERNEL__
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#endif
#define vuint8_t uint8_t
#define vuint16_t uint16_t
#elif defined(UNIXUSER)
#include <stdio.h>
#include <arpa/inet.h>
#define vuint8_t uint32_t
#define vuint16_t uint32_t
#endif

#include <fscore/patch.h>
#include <fscore/sched.h>
#include <fscore/debug.h>
#include <fscore/fstitchd.h>

#if FSTITCH_DEBUG

/* For a lean and mean debug output stream, set both of these to 1. */
#define FSTITCH_OMIT_FILE_FUNC 0
#define FSTITCH_OMIT_BTRACE 0

#if !FSTITCH_OMIT_BTRACE && !defined(__i386__)
#warning Debug backtraces only available on x86 platforms; disabling
#undef FSTITCH_OMIT_BTRACE
#define FSTITCH_OMIT_BTRACE 1
#endif

#ifdef __KERNEL__
#if !FSTITCH_OMIT_BTRACE && !defined(CONFIG_FRAME_POINTER)
#warning Frame pointers are required for backtraces in the kernel; disabling
#undef FSTITCH_OMIT_BTRACE
#define FSTITCH_OMIT_BTRACE 1
#endif
#endif

static bool modules_ignore[sizeof(modules) / sizeof(modules[0])] = {0};
	
static int debug_count = 0;

#define LIT_8 (-1)
#define LIT_16 (-2)
#define LIT_32 (-4)
#define LIT_STR (-3)
#define END 0

/* I/O system prototypes */
static int fstitch_debug_io_init(void);
static int fstitch_debug_io_write(void * data, uint32_t size);
static void fstitch_debug_io_command(void * arg);

#ifdef __KERNEL__

static struct proc_dir_entry * proc_entry;
static uint8_t * proc_buffer;
static off_t proc_buffer_rpos;
static off_t proc_buffer_wpos;
static int proc_shutdown;

#ifdef CONFIG_DEBUG_FS
static struct dentry * debug_count_dentry;
#endif

static int fstitch_debug_proc_read(char * page, char ** start, off_t off, int count, int * eof, void * data)
{
	off_t size;
	while(proc_buffer_rpos == proc_buffer_wpos)
	{
		if(proc_shutdown || assert_failed)
			return 0;
		/* buffer is empty, wait for writes */
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 50);
		if(signal_pending(current))
			return -EINTR;
	}
	size = proc_buffer_wpos - proc_buffer_rpos;
	if(size > count)
		size = count;
	for(count = 0; count < size; count++)
		*(page++) = proc_buffer[proc_buffer_rpos++ % DEBUG_PROC_SIZE];
	*start = (char *) count;
	return count;
}

static int fstitch_debug_io_write(void * data, uint32_t len)
{
	uint8_t * buf = data;
	size_t i;
	for(i = 0; i < len; i++)
	{
		while(proc_buffer_wpos >= proc_buffer_rpos + DEBUG_PROC_SIZE)
		{
			/* buffer is full, wait for reads */
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ / 50);
		}
		proc_buffer[proc_buffer_wpos++ % DEBUG_PROC_SIZE] = buf[i];
	}
	return len;
}

static void fstitch_debug_io_command(void * arg)
{
	/* kfstitchd does not currently support command reading */
}

static void fstitch_debug_shutdown(void * ignore)
{
	int tries = 0;
	proc_shutdown = 1;
	if(atomic_read(&proc_entry->count) > 0)
	{
		while(atomic_read(&proc_entry->count) > 0)
			jsleep(HZ / 4);
		if(++tries == 2)
			printf("Please kill /proc/" DEBUG_PROC_FILENAME " reader.\n");
	}
	remove_proc_entry(DEBUG_PROC_FILENAME, &proc_root);	
	vfree(proc_buffer);
	proc_buffer = NULL;
	
#if CONFIG_DEBUG_FS
	if(debug_count_dentry)
	{
		debugfs_remove(debug_count_dentry);
		debug_count_dentry = NULL;
	}
#endif
}

static int fstitch_debug_io_init(void)
{
	int r;
	proc_buffer = vmalloc(DEBUG_PROC_SIZE);
	if(!proc_buffer)
		return -ENOMEM;
	proc_buffer_wpos = 0;
	proc_buffer_rpos = 0;
	
	proc_entry = create_proc_read_entry(DEBUG_PROC_FILENAME, 0444, &proc_root, fstitch_debug_proc_read, NULL);
	if(!proc_entry)
	{
		fprintf(stderr, "%s: unable to create proc entry\n", __FUNCTION__);
		return -1;
	}
	
#ifdef CONFIG_DEBUG_FS
	debug_count_dentry = debugfs_create_u32(DEBUG_COUNT_FILENAME, 0444, NULL, &debug_count);
	if(IS_ERR(debug_count_dentry))
	{
		printf("%s(): debugfs_create_u32(\"%s\") = error %ld\n", __FUNCTION__, DEBUG_COUNT_FILENAME, PTR_ERR(debug_count_dentry));
		debug_count_dentry = NULL;
	}
#endif
	
	r = fstitchd_register_shutdown_module(fstitch_debug_shutdown, NULL, SHUTDOWN_POSTMODULES);
	if(r < 0)
	{
		fprintf(stderr, "%s: unable to register shutdown callback\n", __FUNCTION__);
		remove_proc_entry(DEBUG_PROC_FILENAME, &proc_root);
		return r;
	}
	
	return 0;
}

#elif defined(UNIXUSER)

static FILE * file_output;

static int fstitch_debug_io_write(void * data, uint32_t len)
{
	return fwrite(data, 1, len, file_output);
}

static void fstitch_debug_io_command(void * arg)
{
	/* uufstitchd does not currently support command reading */
}

static void fstitch_debug_shutdown(void * ignore)
{
	fclose(file_output);
	file_output = NULL;
}

static int fstitch_debug_io_init(void)
{
	int r;
	file_output = fopen(DEBUG_FILENAME, "w");
	if(!file_output)
	{
		fprintf(stderr, "%s: unable to open debug trace file %s\n", __FUNCTION__, DEBUG_FILENAME);
		return -1;
	}
	r = fstitchd_register_shutdown_module(fstitch_debug_shutdown, NULL, SHUTDOWN_POSTMODULES);
	if(r < 0)
	{
		fprintf(stderr, "%s: unable to register shutdown callback\n", __FUNCTION__);
		fstitch_debug_shutdown(NULL);
		return r;
	}
	return 0;
}

#endif


/* This function is used like a binary version of printf(). It takes a file
 * descriptor, and then a series of pairs of (size, pointer) of data to write to
 * it. The list is terminated by a 0 size. Also accepted are the special sizes
 * -1, -2, and -4, which indicate that the data is to be extracted from the
 * stack as a uint8_t, uint16_t, or uint32_t, respectively, and changed to
 * network byte order. Finally, the special size -3 means to write a
 * null-terminated string, whose size will be determined with strlen(). The
 * total number of bytes written is returned, or a negative value on error when
 * no bytes have been written. Note that an error may cause the number of bytes
 * written to be smaller than requested. */
static int fstitch_debug_write(int size, ...)
{
	int bytes = 0;
	va_list ap;
	va_start(ap, size);
	
	for(;;)
	{
		int result;
		
		if(size > 0)
		{
			void * data = va_arg(ap, void *);
			result = fstitch_debug_io_write(data, size);
		}
		else if(size < 0)
		{
			/* negative size means on stack */
			size = -size;
			if(size == 1)
			{
				uint8_t data = va_arg(ap, vuint8_t);
				result = fstitch_debug_io_write(&data, 1);
			}
			else if(size == 2)
			{
				uint16_t data = htons(va_arg(ap, vuint16_t));
				result = fstitch_debug_io_write(&data, 2);
			}
			else if(size == 4)
			{
				uint32_t data = htonl(va_arg(ap, uint32_t));
				result = fstitch_debug_io_write(&data, 4);
			}
			else if(size == 3)
			{
				/* string */
				char * string = va_arg(ap, char *);
				int length = strlen(string);
				size = length + 1;
				result = fstitch_debug_io_write(string, size);
			}
			else
				/* restricted to 1, 2, and 4 bytes, or strings */
				return bytes ? bytes : -EINVAL;
		}
		else
			break;
		
		if(result < 0)
			return bytes ? bytes : result;
		bytes += result;
		if(result != size)
			break;
		size = va_arg(ap, int);
	}
	
	return bytes;
}

int fstitch_debug_init(void)
{
	int m, o, r;
	int timestamp = jiffy_time();
	
	printf("Initializing featherstitch debugging interface...\n");
	
	r = sched_register(fstitch_debug_io_command, NULL, HZ / 10);
	if(r < 0)
		return r;

	r = fstitch_debug_io_init();
	if(r < 0)
		return r;
	
	fstitch_debug_write(LIT_32, DEBUG_SIG_MAGIC, LIT_STR, __DATE__, LIT_32, timestamp, END);
	
	for(m = 0; modules[m].opcodes; m++)
		for(o = 0; modules[m].opcodes[o]->params; o++)
		{
			int p;
			fstitch_debug_write(LIT_16, modules[m].module, LIT_16, modules[m].opcodes[o]->opcode, LIT_STR, modules[m].opcodes[o]->name, END);
			for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
			{
				uint8_t size = type_sizes[modules[m].opcodes[o]->params[p]->type];
				fstitch_debug_write(LIT_8, size, LIT_STR, modules[m].opcodes[o]->params[p]->name, END);
				if(modules[m].opcodes[o]->params[p]->type == FORMAT)
					if(modules[m].opcodes[o]->params[p + 1]->name)
					{
						printf("WARNING: ignoring extra parameters after \"%s\" in module 0x%04x:0x%04x!\n", modules[m].opcodes[o]->params[p]->name, m, o);
						break;
					}
			}
			fstitch_debug_write(LIT_8, 0, END);
		}
	fstitch_debug_write(LIT_16, 0, END);
	
	printf("Debugging interface initialized OK\n");
	
	return 0;
}

void fstitch_debug_command(uint16_t command, uint16_t module, const char * file, int line, const char * function)
{
	switch(command)
	{
		case FSTITCH_DEBUG_MARK:
			printf("Sent mark [%04x] from %s() at %s:%d\n", module, function, file, line);
			fstitch_debug_send(FDB_MODULE_INFO, FDB_INFO_MARK, file, line, function, module);
			break;
		case FSTITCH_DEBUG_DISABLE:
		{
			int m;
			for(m = 0; modules[m].opcodes; m++)
				if(modules[m].module == module)
				{
					printf("Disabled debugging for module [%04x] from %s() at %s:%d\n", module, function, file, line);
					modules_ignore[m] = 1;
					break;
				}
			break;
		}
		case FSTITCH_DEBUG_ENABLE:
		{
			int m;
			for(m = 0; modules[m].opcodes; m++)
				if(modules[m].module == module)
				{
					printf("Enabled debugging for module [%04x] from %s() at %s:%d\n", module, function, file, line);
					modules_ignore[m] = 0;
					break;
				}
			break;
		}
	}
}

#ifdef __i386__
#define x86_get_ebp(bp) { void * __bp; __asm__ __volatile__ ("movl %%ebp, %0" : "=r" (__bp) : ); bp = __bp; }
#endif

int fstitch_debug_send(uint16_t module, uint16_t opcode, const char * file, int line, const char * function, ...)
{
	int m, o = 0, r = 0;
	int timestamp = jiffy_time();
	va_list ap;
	va_start(ap, function);
	
	fstitch_debug_io_command(NULL);
	
	/* look up the right module and opcode indices */
	for(m = 0; modules[m].opcodes; m++)
		if(modules[m].module == module)
		{
			if(modules_ignore[m])
				return 0;
			for(o = 0; modules[m].opcodes[o]->params; o++)
				if(modules[m].opcodes[o]->opcode == opcode)
					break;
			break;
		}
	
	debug_count++;
#if FSTITCH_OMIT_FILE_FUNC
	fstitch_debug_write(LIT_32, timestamp, LIT_STR, "", LIT_32, line, LIT_STR, "", LIT_16, module, LIT_16, opcode, END);
#else
	fstitch_debug_write(LIT_32, timestamp, LIT_STR, file, LIT_32, line, LIT_STR, function, LIT_16, module, LIT_16, opcode, END);
#endif
	
	if(!modules[m].opcodes)
	{
		/* unknown module */
		fstitch_debug_write(LIT_8, 0, LIT_8, 1, END);
		r = -EINVAL;
	}
	else if(!modules[m].opcodes[o]->params)
	{
		/* unknown opcode */
		fstitch_debug_write(LIT_8, 0, LIT_8, 2, END);
		r = -EINVAL;
	}
	else
	{
		int p;
		for(p = 0; !r && modules[m].opcodes[o]->params[p]->name; p++)
		{
			/* TODO: we don't actually have to write the size for each parameter... */
			uint8_t size = type_sizes[modules[m].opcodes[o]->params[p]->type];
			if(size == 4)
			{
				uint32_t param = va_arg(ap, uint32_t);
				fstitch_debug_write(LIT_8, 4, LIT_32, param, END);
			}
			else if(size == 2)
			{
				uint16_t param = va_arg(ap, vuint16_t);
				fstitch_debug_write(LIT_8, 2, LIT_16, param, END);
			}
			else if(size == 1)
			{
				uint8_t param = va_arg(ap, vuint8_t);
				fstitch_debug_write(LIT_8, 1, LIT_8, param, END);
			}
			else if(size == (uint8_t) -1 && modules[m].opcodes[o]->params[p]->type == STRING)
			{
				char * param = va_arg(ap, char *);
				fstitch_debug_write(LIT_8, -1, LIT_STR, param, END);
			}
			else if(size == (uint8_t) -1 && modules[m].opcodes[o]->params[p]->type == FORMAT)
			{
				char buffer[128] = {0};
				char * param = va_arg(ap, char *);
				vsnprintf(buffer, sizeof(buffer), param, ap);
				fstitch_debug_write(LIT_8, -1, LIT_STR, buffer, END);
				/* FORMAT must be the last declared parameter */
				break;
			}
			else
			{
				/* unknown type */
				fstitch_debug_write(LIT_8, 0, LIT_8, 3, END);
				r = -EINVAL;
			}
		}
	}
	
	/* TODO: not technically necessary, see above */
	fstitch_debug_write(LIT_16, 0, END);
	
#if !FSTITCH_OMIT_BTRACE
	{
		int preamble = 1;
		void ** ebp;
		void ** last_ebp = NULL;
		x86_get_ebp(ebp);
		while(ebp >= last_ebp)
		{
			if(!ebp[1])
				break;
			if(!preamble || ebp[1] == __builtin_return_address(0))
			{
				fstitch_debug_write(LIT_32, ebp[1], END);
				preamble = 0;
			}
			last_ebp = ebp;
			ebp = *ebp;
		}
	}
#endif
	fstitch_debug_write(LIT_32, 0, END);
	
	va_end(ap);
	
	/* for debugging the debugging interface... */
	if(r < 0)
	{
		printf("fstitch_debug_send(%s, %d, %s(), 0x%04x, 0x%04x, ...) = %i\n", file, line, function, module, opcode, r);
		assert(0);
	}
	return r;
}

int fstitch_debug_count(void)
{
	return debug_count;
}

#endif
