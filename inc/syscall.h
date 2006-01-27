#ifndef KUDOS_INC_SYSCALL_H
#define KUDOS_INC_SYSCALL_H

/* system call numbers */
enum
{
	SYS_cputs = 0,		// Used in Lab 3 Part 1
	SYS_cgetc_nb,
	SYS_getenvid,
	SYS_env_destroy,

	SYS_yield,		// Used in Lab 3 Part 3
	SYS_exofork,
	SYS_env_set_name,
	SYS_env_set_status,
	SYS_env_set_priority,
	SYS_page_alloc,
	SYS_page_map,
	SYS_page_unmap,
	SYS_set_pgfault_upcall,
	SYS_set_irq_upcall,
	SYS_ipc_try_send,
	SYS_ipc_recv,
	SYS_batch_syscall,
	SYS_kernbin_page_alloc,
	SYS_set_trapframe,

	SYS_sb16_ioctl,

	SYS_vga_set_mode_320,
	SYS_vga_set_mode_text,
	SYS_vga_set_palette,
	SYS_vga_map_text,

	SYS_net_ioctl,

	SYS_reboot,

	SYS_set_symtbls,

	SYS_reg_serial,
	SYS_unreg_serial,

	SYS_grant_io,
	SYS_assign_irq,

	SYS_get_hw_time,

	SYS_print_backtrace,

	NSYSCALLS
};

#endif /* !KUDOS_INC_SYSCALL_H */
