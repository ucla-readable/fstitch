// Breakpoint support

#include <inc/stdio.h>
#include <inc/pmap.h>
#include <inc/env.h>
#include <inc/mmu.h>
#include <inc/x86.h>

#include <kern/trap.h>
#include <kern/env.h>
#include <kern/elf.h>
#include <kern/breakpoints.h>

#define NBREAKS 4

typedef struct {
	envid_t envid;
	bool active;
} break_t;

static break_t breaks[NBREAKS];

void breakpoints_init(void)
{
	int i;
	for (i=0; i < NBREAKS; i++)
	{
		breaks[i].envid  = ENVID_KERNEL;
		breaks[i].active = 0;
	}
}

// disable/enable breakpoints to switch to env envid
void
breakpoints_sched(envid_t envid)
{
	int i;
	for (i=0; i < NBREAKS; i++)
	{
		if (breaks[i].envid != ENVID_KERNEL)
		{
			if (breaks[i].envid == envid)
			{
				if (breaks[i].active)
					breakpoints_active(i, 1, 1);
			}
			else
				breakpoints_active(i, 0, 1);
		}
	}
}

int
breakpoints_print(struct Trapframe *tf)
{
	char indent[] = "  ";

	uint32_t ss=0;
	register_t dr6, dr7;

	// dr6 fields
	uint32_t bd, bs, bt;
	uint32_t bd_o=13, bs_o=14, bt_o=15;
	char triggered[] = {' ', '*'};

	// dr7 fields
	uint32_t lebe,     gebe,     gd;
	uint32_t lebe_o=8, gebe_o=9, gd_o=13;

	size_t i, num_drn_regs=4;
	char *ss_string[] = {"off", "on"};

	dr6 = rdr6();
	dr7 = rdr7();

	bd = (dr6 >> bd_o) & 1;
	bs = (dr6 >> bs_o) & 1;
	bt = (dr6 >> bt_o) & 1;

	printf("Debug registers");

	if(tf)
	{
		envid_t envid;
		printf(", in ", indent);
		if(curenv && tf->tf_eip < KERNBASE)
		{
			envid = curenv->env_id;
			printf("%d:", ENVX(envid));
		}
		else
		{
			envid = ENVID_KERNEL;
			printf("%c:", 'k');
		}
		printf("%s(), ",get_symbol_name(envid, eip_to_fnsym(envid, tf->tf_eip)));

		if(T_DEBUG == tf->tf_trapno)
		{
			printf("DEBUG trap");
		}
		else if(T_BRKPT == tf->tf_trapno)
		{
			printf("INT3 trap");
		}
		else
		{
			printf("Not in a break/debug trap");
		}
		printf("\n");

		if(tf->tf_eflags & FL_TF)
			ss = 1;
	}
	else
	{
		printf("\n");
	}

	printf("%s", indent);
	printf("SS: %c%s\n", triggered[bs], ss_string[ss]);

	for(i=0; i<num_drn_regs; ++i)
	{
		register_t drn = rdrn(i);
		uint32_t b;
		uint32_t b_o=0;
		uint32_t lbe, gbe;
		uint32_t lbe_o=0, gbe_o=1;
		uint32_t rw, len;
		uint32_t rw_o=16, len_o=18;

		b   = ( dr6 >> (b_o+i) ) & 1;
		
		printf("%s", indent);
		printf("DR%d:%c0x%08x", i, triggered[b], drn);
		lbe = ( dr7 >> (lbe_o+2*i) ) & 1;
		gbe = ( dr7 >> (gbe_o+2*i) ) & 1;
		rw  = ( dr7 >> (rw_o+4*i)  ) & 3;
		len = (( dr7 >> (len_o+4*i) ) & 3) + 1;
		printf("  L %d  G %d  RW %d  LEN %d  envid ", lbe, gbe, rw, len);
		if (breaks[i].envid == ENVID_KERNEL)
			printf("kernel\n");
		else
		{
			printf("%08x", breaks[i].envid);
			printf(" (%s)", envs[ENVX(breaks[i].envid)].env_name);
			printf("\n");
		}
	}
	lebe = (dr7 >> lebe_o) & 1;
	gebe = (dr7 >> gebe_o) & 1;
	gd   = (dr7 >> gd_o)   & 1;
	printf("%s", indent);
	printf("DR7: LE %d  GE %d  GD %d", lebe, gebe, gd);

	printf(" | DR6: BD %d  BT %d\n", bd,  bt);

	return 0;
}

#define DR7_LE (1<<8)
#define DR7_GE (1<<9)
#define DR7_L0 (1)
#define DR7_L1 (1<<2)
#define DR7_L2 (1<<4)
#define DR7_L3 (1<<6)
#define DR7_G0 (1<<1)
#define DR7_G1 (1<<3)
#define DR7_G2 (1<<5)
#define DR7_G3 (1<<7)
#define DR7_RW0_0 (1<<16)
#define DR7_RW0_1 (1<<17)
#define DR7_LEN0_0 (1<<18)
#define DR7_LEN0_1 (1<<19)


// Set a breakpoint register:
// envid: envid of the env to set teh breakpoint in (or ENVID_KERNEL)
// reg: which breakpoint register, [0, 3].
// addr: the linear address to set a breakpoint for.
// mem_exec: 0 for a memory breakpoint, 1 for an exec breakpoint.
//   For memory breakpoints:
//   w_rw: 0 for write, 1 for read or write.
//   len: length, 1, 2, or 4 bytes
int
breakpoints_set(envid_t envid,
				uint32_t reg,
				uintptr_t addr,
				bool mem_exec,
				bool w_rw, int len)
{
	ldrn(addr, reg);
	
	register_t dr7 = rdr7();
	dr7 |= DR7_LE | DR7_GE; // need for any debug regs
	// dr7 |= DR7_L0 << (2*reg); // need to "task switch" (?) to make use of
	dr7 |= DR7_G0 << (2*reg);
	

	if(mem_exec == 0)
	{
		if(w_rw == 0)
		{
			dr7 |=    DR7_RW0_0 << (4*reg);
			dr7 &= ~( DR7_RW0_1 << (4*reg) );
		}
		else
		{
			dr7 |= (DR7_RW0_1 << (4*reg)) | (DR7_RW0_0 << (4*reg));
		}
		
		if(len != 1 && len != 2 && len != 4)
		{
			printf("Length must be 1, 2, or 4\n");
			return 0;
		}
		
		if(1==len)
		{
			dr7 &= ~(DR7_LEN0_0 << (4*reg));
			dr7 &= ~(DR7_LEN0_1 << (4*reg));
		}
		else if(2==len)
		{
			dr7 |=   DR7_LEN0_0 << (4*reg);
			dr7 &= ~(DR7_LEN0_1 << (4*reg));
		}
		else if(4==len)
		{
			dr7 |= DR7_LEN0_0 << (4*reg);
			dr7 |= DR7_LEN0_1 << (4*reg);
		}
	}
	else
	{
		dr7 &= ~( (DR7_RW0_1  << (4*reg)) | (DR7_RW0_0 <<  (4*reg)) );
		dr7 &= ~( (DR7_LEN0_1 << (4*reg)) | (DR7_LEN0_0 << (4*reg)) );
	}
	
	ldr7(dr7);

	breaks[reg].envid = envid;
	breaks[reg].active = 1;

	return 0;
}

int
breakpoints_active(int32_t reg, bool active, bool caller_is_sched)
{
	if(reg >= 4)
	{
		printf("Illegal debug register\n");
		return 0;
	}

	register_t dr7 = rdr7();
	int reg_first=reg, reg_last=reg;
	if(-1 == reg)
		reg_first = 0; reg_last = 3;

	if(active)
		for( ; reg_first <= reg_last; reg_first++)
			dr7 = dr7 | (DR7_L0<<(2*reg_first)) | (DR7_G0<<(2*reg_first));
	else 
		for( ; reg_first <= reg_last; reg_first++)
			dr7 = dr7 & ~((DR7_L0<<(2*reg_first)) | (DR7_G0<<(2*reg_first)));

	ldr7(dr7);

	// Allow breakpoints_sched() to use breakpoints_active() and to
	// not turn on a breakpoint for an env when scheding the env if the
	// breakpoint has been turned off by the user
	if (!caller_is_sched)
		breaks[reg].active = active;

	return 0;
}

int
breakpoints_ss_active(struct Trapframe *tf, bool active)
{
	if(!tf)
	{
		printf("Single stepping only start/stopable from within a breakpoint\n");
		return 0;
	}

	// Note: ss will not take effect until after the immediately following inst
	if(active)
		tf->tf_eflags = tf->tf_eflags | FL_TF;
	else
		tf->tf_eflags = tf->tf_eflags & ~FL_TF;
	return 0;
}
