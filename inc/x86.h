#ifndef KUDOS_INC_X86_H
#define KUDOS_INC_X86_H

#include <lib/types.h>
#include <assert.h>
#include <inc/mmu.h>

static __inline void breakpoint(void) __attribute__((always_inline));
static __inline void
breakpoint(void)
{
	__asm __volatile("int3");
}

static __inline uint8_t inb(int port) __attribute__((always_inline));
static __inline uint8_t
inb(int port)
{
	uint8_t data;
	__asm __volatile("inb %w1,%0" : "=a" (data) : "d" (port));
	return data;
}

static __inline void insb(int port, void *addr, int cnt) __attribute__((always_inline));
static __inline void
insb(int port, void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\tinsb"			:
			 "=D" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "memory", "cc");
}

static __inline uint16_t inw(int port) __attribute__((always_inline));
static __inline uint16_t
inw(int port)
{
	uint16_t data;
	__asm __volatile("inw %w1,%0" : "=a" (data) : "d" (port));
	return data;
}

static __inline void insw(int port, void *addr, int cnt) __attribute__((always_inline));
static __inline void
insw(int port, void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\tinsw"			:
			 "=D" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "memory", "cc");
}

static __inline uint32_t inl(int port) __attribute__((always_inline));
static __inline uint32_t
inl(int port)
{
	uint32_t data;
	__asm __volatile("inl %w1,%0" : "=a" (data) : "d" (port));
	return data;
}

static __inline void insl(int port, void *addr, int cnt) __attribute__((always_inline));
static __inline void
insl(int port, void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\tinsl"			:
			 "=D" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "memory", "cc");
}

static __inline void outb(int port, uint8_t data) __attribute__((always_inline));
static __inline void
outb(int port, uint8_t data)
{
	__asm __volatile("outb %0,%w1" : : "a" (data), "d" (port));
}

static __inline void outsb(int port, const void *addr, int cnt) __attribute__((always_inline));
static __inline void
outsb(int port, const void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\toutsb"		:
			 "=S" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "cc");
}

static __inline void outw(int port, uint16_t data) __attribute__((always_inline));
static __inline void
outw(int port, uint16_t data)
{
	__asm __volatile("outw %0,%w1" : : "a" (data), "d" (port));
}

static __inline void outsw(int port, const void *addr, int cnt) __attribute__((always_inline));
static __inline void
outsw(int port, const void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\toutsw"		:
			 "=S" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "cc");
}

static __inline void outsl(int port, const void *addr, int cnt) __attribute__((always_inline));
static __inline void
outsl(int port, const void *addr, int cnt)
{
	__asm __volatile("cld\n\trepne\n\toutsl"		:
			 "=S" (addr), "=c" (cnt)		:
			 "d" (port), "0" (addr), "1" (cnt)	:
			 "cc");
}

static __inline void outl(int port, uint32_t data) __attribute__((always_inline));
static __inline void
outl(int port, uint32_t data)
{
	__asm __volatile("outl %0,%w1" : : "a" (data), "d" (port));
}

static __inline void invlpg(uint32_t addr) __attribute__((always_inline));
static __inline void 
invlpg(uint32_t addr)
{ 
	__asm __volatile("invlpg (%0)" : : "r" (addr) : "memory");
}  

static __inline void lidt(void *p) __attribute__((always_inline));
static __inline void
lidt(void *p)
{
	__asm __volatile("lidt (%0)" : : "r" (p));
}

static __inline void lldt(uint16_t sel) __attribute__((always_inline));
static __inline void
lldt(uint16_t sel)
{
	__asm __volatile("lldt %0" : : "r" (sel));
}

static __inline void ltr(uint16_t sel) __attribute__((always_inline));
static __inline void
ltr(uint16_t sel)
{
	__asm __volatile("ltr %0" : : "r" (sel));
}

static __inline void lcr0(register_t val) __attribute__((always_inline));
static __inline void
lcr0(register_t val)
{
	__asm __volatile("movl %0,%%cr0" : : "r" (val));
}

static __inline register_t rcr0(void) __attribute__((always_inline));
static __inline register_t
rcr0(void)
{
	register_t val;
	__asm __volatile("movl %%cr0,%0" : "=r" (val));
	return val;
}

static __inline register_t rcr2(void) __attribute__((always_inline));
static __inline register_t
rcr2(void)
{
	register_t val;
	__asm __volatile("movl %%cr2,%0" : "=r" (val));
	return val;
}

static __inline void lcr3(register_t val) __attribute__((always_inline));
static __inline void
lcr3(register_t val)
{
	__asm __volatile("movl %0,%%cr3" : : "r" (val));
}

static __inline register_t rcr3(void) __attribute__((always_inline));
static __inline register_t
rcr3(void)
{
	register_t val;
	__asm __volatile("movl %%cr3,%0" : "=r" (val));
	return val;
}

static __inline void lcr4(register_t val) __attribute__((always_inline));
static __inline void
lcr4(register_t val)
{
	__asm __volatile("movl %0,%%cr4" : : "r" (val));
}

static __inline register_t rcr4(void) __attribute__((always_inline));
static __inline register_t
rcr4(void)
{
	register_t cr4;
	__asm __volatile("movl %%cr4,%0" : "=r" (cr4));
	return cr4;
}

static __inline void tlbflush(void) __attribute__((always_inline));
static __inline void
tlbflush(void)
{
	register_t cr3;
	__asm __volatile("movl %%cr3,%0" : "=r" (cr3));
	__asm __volatile("movl %0,%%cr3" : : "r" (cr3));
}

static __inline register_t read_eflags(void) __attribute__((always_inline));
static __inline register_t
read_eflags(void)
{
        register_t eflags;
        __asm __volatile("pushfl; popl %0" : "=r" (eflags));
        return eflags;
}

static __inline void write_eflags(register_t eflags) __attribute__((always_inline));
static __inline void
write_eflags(register_t eflags)
{
        __asm __volatile("pushl %0; popfl" : : "r" (eflags));
}

static __inline register_t read_ebp(void) __attribute__((always_inline));
static __inline register_t
read_ebp(void)
{
        register_t ebp;
        __asm __volatile("movl %%ebp,%0" : "=r" (ebp));
        return ebp;
}

static __inline register_t read_esp(void) __attribute__((always_inline));
static __inline register_t
read_esp(void)
{
        register_t esp;
        __asm __volatile("movl %%esp,%0" : "=r" (esp));
        return esp;
}

static __inline void
cpuid(uint32_t info, register_t* eaxp, register_t* ebxp, register_t* ecxp, register_t* edxp)
{
	register_t eax, ebx, ecx, edx;
	asm volatile("cpuid" 
		: "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
		: "a" (info));
	if (eaxp)
		*eaxp = eax;
	if (ebxp)
		*ebxp = ebx;
	if (ecxp)
		*ecxp = ecx;
	if (edxp)
		*edxp = edx;
}

static __inline uint64_t read_tsc(void) __attribute__((always_inline));
static __inline uint64_t
read_tsc(void)
{
        uint64_t tsc;
        __asm __volatile("rdtsc" : "=A" (tsc));
        return tsc;
}

//
// Breakpoints

static __inline void debugregs_set(uint8_t onoff) __attribute__((always_inline));
static __inline void
debugregs_set(uint8_t onoff)
{
	if(onoff)
		onoff = CR4_DE; // turn the right bit on
	register_t cr4 = rcr4();
	cr4 = (cr4 & ~CR4_DE) | onoff;
	lcr4(cr4);
}

static __inline uint8_t debugregs_read(void) __attribute__((always_inline));
static __inline uint8_t
debugregs_read()
{
	register_t de_on;
	register_t cr4 = rcr4();
	de_on = cr4 & CR4_DE;
	if(de_on) return 1;
	else return 0;
}

static __inline void ldrn(uintptr_t, uint32_t) __attribute__((always_inline));
static __inline void
ldrn(uintptr_t laddr, uint32_t reg_num)
{
	assert(0 <= reg_num && reg_num < 4);
	switch(reg_num)
	{
		case(0): __asm __volatile("movl %0,%%dr0" : : "a" (laddr)); break;
		case(1): __asm __volatile("movl %0,%%dr1" : : "a" (laddr)); break;
		case(2): __asm __volatile("movl %0,%%dr2" : : "a" (laddr)); break;
		case(3): __asm __volatile("movl %0,%%dr3" : : "a" (laddr)); break;
	}
}

static __inline uintptr_t rdrn(uint32_t) __attribute__((always_inline));
static __inline uintptr_t
rdrn(uint32_t reg_num)
{
	assert(0 <= reg_num && reg_num < 4);
	physaddr_t paddr;
	switch(reg_num)
	{
		case(0): __asm __volatile("movl %%dr0,%0" : "=r" (paddr)); break;
		case(1): __asm __volatile("movl %%dr1,%0" : "=r" (paddr)); break;
		case(2): __asm __volatile("movl %%dr2,%0" : "=r" (paddr)); break;
		case(3): __asm __volatile("movl %%dr3,%0" : "=r" (paddr)); break;
		default: panic("illegal reg_num %d", reg_num);
	}
	return paddr;
}

static __inline void ldr6(register_t) __attribute__((always_inline));
static __inline void
ldr6(register_t val)
{
	__asm __volatile("movl %0,%%dr6" : : "r" (val));
}

static __inline register_t rdr6(void) __attribute__((always_inline));
static __inline register_t
rdr6(void)
{
	register_t val;
	__asm __volatile("movl %%dr6,%0" : "=r" (val));
	return val;
}

static __inline void ldr7(register_t) __attribute__((always_inline));
static __inline void
ldr7(register_t val)
{
	__asm __volatile("movl %0,%%dr7" : : "r" (val));
}

static __inline register_t rdr7(void) __attribute__((always_inline));
static __inline register_t
rdr7()
{
	register_t dr7;
	__asm __volatile("movl %%dr7,%0" : "=r" (dr7));
	return dr7;
}

#endif /* !KUDOS_INC_X86_H */
