#include <inc/x86.h>
#include <inc/stdio.h>
#include <inc/env.h>
#include <inc/error.h>
#include <inc/sb16.h>

#include <kern/pmap.h>
#include <kern/env.h>
#include <kern/trap.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/sb16.h>

#define MIXER_PORT (SB16_PORT + 0x4)
#define MIXER_DATA_PORT (SB16_PORT + 0x5)
#define RESET_PORT (SB16_PORT + 0x6)
#define READ_PORT (SB16_PORT + 0xA)
#define WRITE_PORT (SB16_PORT + 0xC)
#define POLL_PORT (SB16_PORT + 0xE)
#define POLL16_PORT (SB16_PORT + 0xF)

#define DMA_MASK_PORT 0xD4
#define DMA_CLRPTR_PORT 0xD8
#define DMA_MODE_PORT 0xD6
#define DMA_BASEADDR_PORT (0xC0 + 4 * (SB16_DMA16 % 4))
#define DMA_COUNT_PORT (0xC2 + 4 * (SB16_DMA16 % 4))

#if SB16_DMA16 == 5
#	define DMA_PAGE_PORT 0x8B
#elif SB16_DMA16 == 6
#	define DMA_PAGE_PORT 0x89
#elif SB16_DMA16 == 7
#	define DMA_PAGE_PORT 0x8A
#else
#	error Unsupported SB16 DMA16
#endif

#define DMA_STOPMASK (4 + (SB16_DMA16 % 4))
#define DMA_STARTMASK (SB16_DMA16 % 4)

#define DMA_MODE ((sb_output ? 0x58 : 0x54) + (SB16_DMA16 % 4))

#define BUFFER_LENGTH ((SB16_BUFFER_PAGES << PGSHIFT) / 2)
#define BLOCK_LENGTH (BUFFER_LENGTH / 2)

static uint16_t sb_rate;
static uint16_t sb_output; /* boolean */
static void * sb_buffer = NULL;
static physaddr_t sb_buffer_addr = 0;

static volatile uint8_t sb_started = 0;
static volatile uint8_t sb_block;
static volatile uint8_t sb_interrupted;
static volatile uint8_t sb_initialized = 0;

static envid_t sb_envid = 0;
static struct Env * sb_env = NULL;
static uintptr_t sb_va = 0;

static int sb16_reset(void)
{
	int limit;
	
	outb(RESET_PORT, 1);
	kclock_delay(1);
	outb(RESET_PORT, 0);
	
	limit = jiffies + 3 * HZ / 100;
	while(limit - jiffies > 0)
		/* unroll sb16_read so we can time out */
		if(inb(POLL_PORT) & 0x80)
			if(inb(READ_PORT) == 0xAA)
				return 0;
	return -1;
}

static void sb16_write(uint8_t byte)
{
	while(inb(WRITE_PORT) & 0x80);
	outb(WRITE_PORT, byte);
}

static uint8_t sb16_read(void)
{
	while(!(inb(POLL_PORT) & 0x80));
	return inb(READ_PORT);
}

static void sb16_setmixer(uint8_t port, uint8_t value)
{
	outb(MIXER_PORT, port);
	kclock_delay(1);
	outb(MIXER_DATA_PORT, value);
	kclock_delay(1);
}

static uint8_t sb16_getmixer(uint8_t port)
{
	outb(MIXER_PORT, port);
	kclock_delay(1);
	return inb(MIXER_DATA_PORT);
}

// Interrupts should be enabled prior to entering sb16_init()
void sb16_init(void)
{
	int i;
	uint8_t major, minor;
	
	printf("SB16: ");
#if !ENABLE_INKERNEL_INTS
	printf("not detecting, require in-kernel interrupts\n");
	return;
#else
	if(sb16_reset())
	{
		printf("not detected\n");
		return;
	}
	
	printf("detected, DSP version ");
	
	sb16_write(0xE1);
	major = sb16_read();
	minor = sb16_read();
	
	printf("%d.%02d\n", major, minor);
	
	if(major < 4)
	{
		printf("SB16: DSP version too old, not initializing\n");
		return;
	}
	
	/* set IRQ */
	sb16_setmixer(0x80, (1 << (SB16_IRQ % 4)));
	
	/* set DMA */
	sb16_setmixer(0x81, (1 << SB16_DMA16) | (1 << SB16_DMA));
	
	/* set default volume */
	sb16_setvolume(90);
	/* PCM controls.. */
	//sb16_setmixer(0x32, sb16_getmixer(0x32) | 0xF8);
	//sb16_setmixer(0x33, sb16_getmixer(0x33) | 0xF8);
	
	/* interrupt test */
	request_irq(SB16_IRQ, sb16_intr);
	irq_setmask_8259A(irq_mask_8259A & ~(1 << SB16_IRQ));
	sb_interrupted = 0;
	sb16_write(0xF2);
	
	i = jiffies + HZ / 10;
	while(i - jiffies > 0)
		if(sb_interrupted)
		{
			inb(POLL_PORT);
			break;
		}
	if(!sb_interrupted)
	{
		irq_setmask_8259A(irq_mask_8259A | (1 << SB16_IRQ));
		printf("SB16: Interrupt test failed!\n");
		return;
	}
	
	printf("SB16: Interrupt test OK\n");
	
	/* use statically allocated buffer at physical address 0 */
	sb_buffer_addr = 0;
	sb_buffer = (void *) KADDR(0);
	
	sb_initialized = 1;
#endif
}

int sb16_close(void)
{
	if(!sb_env)
		return -E_BUSY;
	/* permitted to close? */
	if(curenv->env_id != sb_envid && sb_env->env_id == sb_envid && sb_env->env_status != ENV_FREE)
		return -E_PERM;
	
	sb_initialized = 0;
	sb16_reset();
	
	if(curenv->env_id == sb_envid)
	{
		/* implication: sb_env == curenv */
		int i;
		for(i = 0; i != SB16_BUFFER_PAGES; i++)
			page_remove(sb_env->env_pgdir, sb_va + (i << PGSHIFT));
	}
	
	sb_envid = 0;
	sb_env = NULL;
	sb_va = 0;
	
	sb_initialized = 1;
	return 0;
}

int sb16_open(uint16_t rate, uint8_t output, uintptr_t address)
{
	int i;
	
	if(!sb_initialized)
		return -E_NO_DEV;
	
	if(sb_env)
	{
		/* already open? */
		if(sb_env->env_id == sb_envid && sb_env->env_status != ENV_FREE)
			return -E_BUSY;
		
		sb16_close();
	}
	
	if(address > UTOP - (SB16_BUFFER_PAGES << PGSHIFT) || address != PTE_ADDR(address))
		return -E_INVAL;
	
	for(i = 0; i != SB16_BUFFER_PAGES; i++)
	{
		if(page_insert(curenv->env_pgdir, &pages[(sb_buffer_addr >> PGSHIFT) + i], address + (i << PGSHIFT), PTE_U | PTE_W | PTE_P))
		{
			while(i--)
				page_remove(curenv->env_pgdir, address + (i << PGSHIFT));
			return -E_NO_MEM;
		}
	}
	
	sb_envid = curenv->env_id;
	sb_env = curenv;
	sb_va = address;
	
	sb_rate = rate;
	sb_output = output;
	
	return 0;
}

int sb16_setvolume(uint8_t volume)
{
	uint8_t value;
	
	if(!sb_env || curenv->env_id != sb_envid)
		return -E_PERM;
	
	if(volume > 100)
		volume = 100;
	volume = ((0x1F * volume + 50) / 100) << 3;
	
	value = sb16_getmixer(0x30);
	value &= 0x7;
	value |= volume;
	sb16_setmixer(0x30, value);
	
	value = sb16_getmixer(0x31);
	value &= 0x7;
	value |= volume;
	sb16_setmixer(0x31, value);
	
	return 0;
}

int sb16_start(void)
{
	register_t eflags = read_eflags();
	uint16_t offset = (sb_buffer_addr >> 1) & 0xFFFF;
	
	if(!sb_env || curenv->env_id != sb_envid)
		return -E_PERM;
	
	/* set up variables to simulate an initial interrupt for finishing block 1 */
	sb_started = 1;
	sb_block = 0;
	sb_interrupted = 1;
	
	__asm__ __volatile__("cli");
	/* program DMA controller */
	outb(DMA_MASK_PORT, DMA_STOPMASK);
	outb(DMA_CLRPTR_PORT, 0x00);
	outb(DMA_MODE_PORT, DMA_MODE);
	outb(DMA_BASEADDR_PORT, offset & 0xFF);
	outb(DMA_BASEADDR_PORT, offset >> 8);
	outb(DMA_COUNT_PORT, (BUFFER_LENGTH - 1) & 0xFF);
	outb(DMA_COUNT_PORT, (BUFFER_LENGTH - 1) >> 8);
	outb(DMA_PAGE_PORT, sb_buffer_addr >> 16);
	outb(DMA_MASK_PORT, DMA_STARTMASK);
	
	/* program sound card */
	if(sb_output)
		/* set output sampling rate */
		sb16_write(0x41);
	else
		/* set input sampling rate */
		sb16_write(0x42);
	sb16_write(sb_rate >> 8);
	sb16_write(sb_rate & 0xFF);

	if(sb_output)
		/* 16-bit D->A, A/I, FIFO */
		sb16_write(0xB6);
	else
		/* 16-bit A->D, A/I, FIFO */
		sb16_write(0xBE);
	/* DMA mode: 16-bit signed mono */
	sb16_write(0x10);
	sb16_write((BLOCK_LENGTH - 1) & 0xFF);
	sb16_write((BLOCK_LENGTH - 1) >> 8);
	
	write_eflags(eflags);
	return 0;
}

int sb16_stop(void)
{
	if(!sb_env || curenv->env_id != sb_envid)
		return -E_PERM;
	
	if(!sb_started)
		return -E_BUSY;
	sb_started = 0;
	
	sb16_write(0xD9);
	return 0;
}

int sb16_wait(void)
{
	register_t eflags = read_eflags();
	
	if(!sb_env || curenv->env_id != sb_envid)
		return -E_PERM;
	
	if(!sb_started)
		return -E_BUSY;
	
	__asm__ __volatile__("cli");
	if(sb_interrupted)
	{
		/* return block not being used now */
		uint8_t block = !sb_block;
		sb_interrupted = 0;
		write_eflags(eflags);
		return block;
	}
	
	/* go to sleep, but restart the system call on wake up to get
	 * the right return value in case interrupts are missed */
	
	curenv->env_status = ENV_NOT_RUNNABLE;
	
	/* must restore interrupts after setting status */
	write_eflags(eflags);
	
	/* rewind "int 0x30" (0xCD 0x30) */
	UTF->tf_eip -= 2;
	
	/* this will call env_run which will copy UTF back to curenv */
	sched_yield();
}

void sb16_intr(int irq)
{
	sb_interrupted = 1;
	
	if(!sb_initialized)
		return;
	
	/* wake up the environment if it is sleeping, and not for IPC */
	if(sb_env->env_status == ENV_NOT_RUNNABLE && !sb_env->env_ipc_recving)
		sb_env->env_status = ENV_RUNNABLE;
	
	/* toggle current block */
	sb_block = !sb_block;
	
	inb(POLL16_PORT);
}
