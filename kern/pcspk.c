#include <inc/x86.h>
#include <inc/stdio.h>
#include <inc/timerreg.h>
#include <inc/sb16.h>

#include <kern/pmap.h>
#include <kern/env.h>
#include <kern/irq.h>
#include <kern/trap.h>
#include <kern/kclock.h>
#include <kern/sched.h>
#include <kern/pcspk.h>

/* This file has many similarities to the SB16 driver in kern/sb16.c. That's
 * because it emulates the interface the SB16 driver provides, which in turn is
 * approximately emulating the hardware's interface. */

static uint16_t sb_rate;
static void * sb_buffer = NULL;
static physaddr_t sb_buffer_addr = 0;

static volatile uint8_t sb_started = 0;
static volatile uint8_t sb_block;
static volatile uint8_t sb_interrupted;
static volatile uint8_t sb_initialized = 0;

static envid_t sb_envid = 0;
static struct Env * sb_env = NULL;
static uintptr_t sb_va = 0;

static int pcspk_offset = 0;
static int pcspk_divisor = 0;

int sb_use_pcspk = 0;

static int pcspk_reset(void)
{
	/* disable counter 2 */
	outb(0x61, inb(0x61) & 0xFC);
	request_irq_0(NULL, 1);
	return 0;
}

static void pcspk_intr(int irq)
{
	sb_interrupted = 1;
	
	if(!sb_initialized)
		return;
	
	/* wake up the environment if it is sleeping, and not for IPC */
	if(sb_env->env_status == ENV_NOT_RUNNABLE && !sb_env->env_ipc_recving)
		sb_env->env_status = ENV_RUNNABLE;
	
	/* toggle current block */
	sb_block = !sb_block;
}

static void pcspk_irq_0_handler(int irq)
{
	short * buffer = sb_buffer;
	int sample = 32768 + (int) buffer[pcspk_offset];
	
	sample *= pcspk_divisor;
	sample >>= 16;
	
	/* set command for counter 2, 2 byte write */
	outb(0x43, 0xB0);
	/* select desired HZ */
	outb(0x42, sample % 256);
	outb(0x42, sample / 256);
	
	if(++pcspk_offset == SB16_BUFFER_PAGES * PGSIZE / sizeof(short) / 2)
		pcspk_intr(irq);
	else if(pcspk_offset == SB16_BUFFER_PAGES * PGSIZE / sizeof(short))
	{
		pcspk_offset = 0;
		pcspk_intr(irq);
	}
}

static void pcspk_speedtest_0(int irq)
{
	pcspk_offset++;
}

static int pcspk_speedtest(void)
{
	int seconds;
	pcspk_offset = 0;
	
	/* get current seconds from the CMOS clock */
	seconds = mc146818_read(NULL, 0) & 0xFF;
	
	/* wait for a clock tick edge */
	while(seconds == (mc146818_read(NULL, 0) & 0xFF));
	
	/* get the new seconds from the CMOS clock */
	seconds = mc146818_read(NULL, 0) & 0xFF;
	
	/* set up a high-speed timer interrupt */
	request_irq_0(pcspk_speedtest_0, 44100 / HZ);
	
	/* wait for another clock tick edge */
	while(seconds == (mc146818_read(NULL, 0) & 0xFF));
	
	/* restore the original timer interrupt */
	request_irq_0(NULL, 1);
	
	/* check if the high-speed timer interrupt ran enough */
	if(pcspk_offset < 44000)
		return -1;
	
	pcspk_offset = 0;
	return 0;
}

int pcspk_init(void)
{
	printf("PC speaker driver: ");
#if !ENABLE_INKERNEL_INTS
	printf("not enabling (requires in-kernel interrupts)\n");
	return -1;
#else
	if(pcspk_speedtest() < 0)
	{
		printf("not enabling (CPU not fast enough: %d)\n", pcspk_offset);
		return -1;
	}
	
	printf("enabled\n");
	
	/* use statically allocated buffer at physical address 0 */
	sb_buffer_addr = 0;
	sb_buffer = (void *) KADDR(0);
	
	sb_initialized = 1;
	sb_use_pcspk = 1;
	return 0;
#endif
}

int pcspk_close(void)
{
	if(!sb_env)
		return -E_BUSY;
	/* permitted to close? */
	if(curenv->env_id != sb_envid && sb_env->env_id == sb_envid && sb_env->env_status != ENV_FREE)
		return -E_ACCES;
	
	sb_initialized = 0;
	pcspk_reset();
	
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

int pcspk_open(uint16_t rate, uint8_t output, uintptr_t address)
{
	int i;
	
	if(!sb_initialized)
		return -E_NO_DEV;
	
	if(sb_env)
	{
		/* already open? */
		if(sb_env->env_id == sb_envid && sb_env->env_status != ENV_FREE)
			return -E_BUSY;
		
		pcspk_close();
	}
	
	if(!output)
		return -E_INVAL;
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
	pcspk_divisor = TIMER_DIV(rate);
	
	return 0;
}

int pcspk_setvolume(uint8_t volume)
{
	return 0;
}

int pcspk_start(void)
{
	register_t eflags = read_eflags();
	
	if(!sb_env || curenv->env_id != sb_envid)
		return -E_ACCES;
	
	/* set up variables to simulate an initial interrupt for finishing block 1 */
	sb_started = 1;
	sb_block = 0;
	sb_interrupted = 1;
	pcspk_offset = 0;
	
	request_irq_0(pcspk_irq_0_handler, sb_rate / HZ);
	/* enable counter 2 */
	outb(0x61, inb(0x61) | 3);
	
	write_eflags(eflags);
	return 0;
}

int pcspk_stop(void)
{
	if(!sb_env || curenv->env_id != sb_envid)
		return -E_ACCES;
	
	if(!sb_started)
		return -E_BUSY;
	sb_started = 0;
	
	pcspk_reset();
	return 0;
}

/* identical to sb16_wait() */
int pcspk_wait(void)
{
	register_t eflags = read_eflags();
	
	if(!sb_env || curenv->env_id != sb_envid)
		return -E_ACCES;
	
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
