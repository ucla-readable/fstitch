// System call stubs.

#include <inc/syscall.h>
#include <inc/lib.h>
#include <inc/sb16.h>

static inline register_t
syscall(int num, register_t a1, register_t a2, register_t a3, register_t a4, register_t a5)
{
	register_t ret;

	// Generic system call: pass system call number in AX,
	// up to five parameters in DX, CX, BX, DI, SI.
	// Interrupt kernel with T_SYSCALL.
	//
	// The "volatile" tells the assembler not to optimize
	// this instruction away just because we don't use the
	// return value.
	// 
	// The last clause tells the assembler that this can
	// potentially change the condition codes and arbitrary
	// memory locations.

	asm volatile("int %1\n"
		: "=a" (ret)
		: "i" (T_SYSCALL),
		  "a" (num),
		  "d" (a1),
		  "c" (a2),
		  "b" (a3),
		  "D" (a4),
		  "S" (a5)
		: "cc", "memory");
	
	return ret;
}

void
sys_cputs(const char *a1)
{
	syscall(SYS_cputs, (register_t) a1, 0, 0, 0, 0);
}

int
sys_cgetc(void)
{
	return syscall(SYS_cgetc, 0, 0, 0, 0, 0);
}

int
sys_cgetc_nb(void)
{
	return syscall(SYS_cgetc_nb, 0, 0, 0, 0, 0);
}

envid_t
sys_getenvid(void)
{
	 return syscall(SYS_getenvid, 0, 0, 0, 0, 0);
}

int
sys_env_destroy(envid_t envid)
{
	return syscall(SYS_env_destroy, envid, 0, 0, 0, 0);
}

int
sys_env_set_status(envid_t envid, int status)
{
	return syscall(SYS_env_set_status, envid, status, 0, 0, 0);
}

void
sys_yield(void)
{
	syscall(SYS_yield, 0, 0, 0, 0, 0);
}

int
sys_page_alloc(envid_t envid, void* pg, int perm)
{
	return syscall(SYS_page_alloc, envid, (uintptr_t) pg, perm, 0, 0);
}

int
sys_page_map(envid_t srcenv, void* srcpg, envid_t dstenv, void* dstpg, int perm)
{
	return syscall(SYS_page_map, srcenv, (uintptr_t) srcpg,
		       dstenv, (uintptr_t) dstpg, perm);
}

int
sys_page_unmap(envid_t envid, void* pg)
{
	return syscall(SYS_page_unmap, envid, (uintptr_t) pg, 0, 0, 0);
}

// sys_exofork is inlined in lib.h

int
sys_env_set_name(envid_t envid, const char * name)
{
	return syscall(SYS_env_set_name, envid, (uintptr_t) name, 0, 0, 0);
}

int
sys_env_set_priority(envid_t envid, int priority)
{
	return syscall(SYS_env_set_priority, envid, priority, 0, 0, 0);
}

int
sys_set_pgfault_upcall(envid_t envid, void* upcall)
{
	return syscall(SYS_set_pgfault_upcall, envid, (uintptr_t) upcall, 0, 0, 0);
}

int
sys_ipc_recv(envid_t fromenv, void* dstva, int timeout)
{
	return syscall(SYS_ipc_recv, fromenv, (uintptr_t) dstva, timeout, 0, 0);
}

int
sys_ipc_try_send(envid_t envid, uint32_t value, void* srcva, unsigned perm, const void* capva)
{
	return syscall(SYS_ipc_try_send, envid, value, (uintptr_t) srcva, perm, (uintptr_t) capva);
}

ssize_t
sys_kernbin_page_alloc(envid_t dst_env, const char* name, size_t offset,
		       void* pg, unsigned pg_perm)
{
	return syscall(SYS_kernbin_page_alloc, dst_env, (uintptr_t) name, offset, (uintptr_t) pg, pg_perm);
}

int
sys_set_trapframe(envid_t envid, struct Trapframe* tf)
{
	return syscall(SYS_set_trapframe, envid, (uintptr_t) tf, 0, 0, 0);
}

int
sys_sb16_close(void)
{
	return syscall(SYS_sb16_ioctl, SB16_IOCTL_CLOSE, 0, 0, 0, 0);
}

int
sys_sb16_open(uint16_t rate, uint8_t output, uintptr_t address)
{
	return syscall(SYS_sb16_ioctl, SB16_IOCTL_OPEN, rate, output, address, 0);
}

int
sys_sb16_setvolume(uint8_t volume)
{
	return syscall(SYS_sb16_ioctl, SB16_IOCTL_SETVOLUME, volume, 0, 0, 0);
}

int
sys_sb16_start(void)
{
	return syscall(SYS_sb16_ioctl, SB16_IOCTL_START, 0, 0, 0, 0);
}

int
sys_sb16_stop(void)
{
	return syscall(SYS_sb16_ioctl, SB16_IOCTL_STOP, 0, 0, 0, 0);
}

int
sys_sb16_wait(void)
{
	return syscall(SYS_sb16_ioctl, SB16_IOCTL_WAIT, 0, 0, 0, 0);
}

int
sys_vga_set_mode_320(uintptr_t address)
{
	return syscall(SYS_vga_set_mode_320, address, 0, 0, 0, 0);
}

int
sys_vga_set_mode_text(void)
{
	return syscall(SYS_vga_set_mode_text, 0, 0, 0, 0, 0);
}

int
sys_vga_set_palette(uint8_t * palette, uint8_t dim)
{
	return syscall(SYS_vga_set_palette, (uintptr_t) palette, dim, 0, 0, 0);
}

int
sys_vga_map_text(uintptr_t address)
{
	return syscall(SYS_vga_map_text, address, 0, 0, 0, 0);
}

int
sys_net_ioctl(int req, int ival1, void * pval, int ival2)
{
	return syscall(SYS_net_ioctl, req, ival1, (uintptr_t) pval, ival2, 0);
}

int
sys_reboot(void)
{
	return syscall(SYS_reboot, 0, 0, 0, 0, 0);
}

int
sys_set_symtbls(envid_t envid,
					 void *symtbl,    size_t symtbl_size,
					 void *symstrtbl, size_t symstrtbl_size)
{
	return syscall(SYS_set_symtbls,
						(register_t) envid,
						(register_t) symtbl,    (register_t) symtbl_size,
						(register_t) symstrtbl, (register_t) symstrtbl);
}

int
sys_reg_serial(int port, void *buffer_pg)
{
	return syscall(SYS_reg_serial, port, (uintptr_t) buffer_pg, 0, 0, 0);
}

int
sys_unreg_serial(int port)
{
	return syscall(SYS_unreg_serial, port, 0, 0, 0, 0);
}

int
sys_grant_io(envid_t envid)
{
	return syscall(SYS_grant_io, (register_t) envid, 0, 0, 0, 0);
}

int
sys_get_hw_time(int* sec, int* min, int* hour, int* day, int* mon)
{
        return syscall(SYS_get_hw_time, (int)sec, (int)min, (int)hour, (int)day, (int)mon);
}

int
sys_print_backtrace()
{
	return syscall(SYS_print_backtrace, 0, 0, 0, 0, 0);
}
