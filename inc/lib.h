// Main public header file for our user-land support library,
// whose code lives in the lib directory.
// This library is roughly our OS's version of a standard C library,
// and is intended to be linked into all user-mode applications
// (NOT the kernel or boot loader).

#ifndef KUDOS_INC_LIB_H
#define KUDOS_INC_LIB_H

#include <inc/types.h>
#include <inc/mmu.h>
#include <inc/stdio.h>
#include <inc/stdarg.h>
#include <inc/stdlib.h>
#include <lib/std.h>
#include <inc/string.h>
#include <inc/error.h>
#include <inc/assert.h>
#include <inc/env.h>
#include <inc/pmap.h>
#include <inc/syscall.h>
#include <inc/fs.h>
#include <inc/fd.h>
#include <inc/args.h>

#define USED(x)		((void) (x))

// libos.c or entry.S
extern char* binaryname;
extern const struct Env* env;
extern const struct Env envs[NENV];
extern const struct Page pages[];

// pgfault.c
void	set_pgfault_handler(void (*handler)(void* addr, uint32_t err, uint32_t esp, uint32_t eflags, uint32_t eip));

// readline.c
char*	readline(const char* prompt);

// syscall.c
void	sys_cputs(const char* string);
int	sys_cgetc_nb(void);
envid_t	sys_getenvid(void);
int	sys_env_destroy(envid_t env);
void	sys_yield(void);
envid_t	sys_env_fork(void);
int	sys_env_set_status(envid_t env, int status);
int	sys_page_alloc(envid_t env, void* pg, int perm);
int	sys_page_map(envid_t dst_env, void* dst_pg,
		     envid_t src_env, void* src_pg, int perm);
int	sys_page_unmap(envid_t env, void* pg);
int	sys_env_set_name(envid_t envid, const char * name);
int	sys_env_set_priority(envid_t env, int priority);
int	sys_sb16_close(void);
int	sys_sb16_open(uint16_t rate, uint8_t output, uintptr_t address);
int	sys_sb16_setvolume(uint8_t volume);
int	sys_sb16_start(void);
int	sys_sb16_stop(void);
int	sys_sb16_wait(void);
int	sys_vga_set_mode_320(uintptr_t address);
int	sys_vga_set_mode_text(void);
int	sys_vga_set_palette(uint8_t * palette, uint8_t dim);
int	sys_vga_map_text(uintptr_t address);
int	sys_net_ioctl(int req, int ival1, void * pval, int ival2);
int	sys_mouse_ioctl(int req, int ival, void * pval);
int	sys_reboot(void);
int	sys_set_symtbls(envid_t envid, void *symtbl, size_t symtbl_size, void *symstrtbl, size_t symstrtbl_size);
int	sys_reg_serial(int port, void *buffer_pg);
int	sys_unreg_serial(int port);
int	sys_grant_io(envid_t envid);
int	sys_assign_irq(envid_t envid, int irq, int enable);
int	sys_get_hw_time(int* sec, int* min, int* hour, int* day, int* mon);
int sys_print_backtrace();

// This must be inlined.  Exercise for reader: why?
static __inline envid_t sys_exofork(void) __attribute__((always_inline));
static __inline envid_t
sys_exofork(void)
{
	envid_t ret;
	__asm __volatile("int %2"
		: "=a" (ret)
		: "a" (SYS_exofork),
		  "i" (T_SYSCALL)
	);
	return ret;
}

int	sys_set_pgfault_upcall(envid_t env, void* upcall);
int	sys_set_irq_upcall(envid_t env, void* upcall);
int	sys_ipc_recv(envid_t fromenv, void* rcv_pg, int timeout);
int	sys_ipc_try_send(envid_t dst_env, uint32_t value,
			 void* pg, unsigned pg_perm, const void* cap);
ssize_t	sys_kernbin_page_alloc(envid_t dst_env, const char* name,
			       size_t offset, void* pg, unsigned pg_perm);
int	sys_set_trapframe(envid_t env, struct Trapframe* tf);

// ipc.c
void	ipc_send(envid_t to_env, uint32_t value, void* pg, unsigned perm, const void* cap);
uint32_t ipc_recv(envid_t restrictfrom, envid_t* fromenv, void* pg, unsigned* perm, uint32_t* cap, int timeout);

// fork.c
#define	PTE_SHARE	0x400
pde_t get_pde(void* addr);
pte_t get_pte(void* addr);
int	fork(void);
int	sfork(void);	// Challenge!

// spawn.c
int	spawn(const char*, const char**);
int	spawnl(const char*, const char*, ...);

// fd.c
int	close(int fd);
ssize_t	read(int fd, void* buf, size_t nbytes);
ssize_t	read_nb(int fd, void* buf, size_t nbytes);
int	read_map(int fd, off_t offset, void** blk);
ssize_t	write(int fd, const void* buf, size_t nbytes);
int getdirentries(int fd, void* buf, int nbytes, uint32_t* basep);
int	seek(int fd, off_t offset);
void	close_all(void);
int	dup(int fd);
int	dup2(int oldfd, int newfd);
int	dup2env_send(int fdnum, envid_t envid);
int	dup2env_recv(envid_t from_env);
int	ftruncate(int fd, off_t size);
int	fstat(int fd, struct Stat*);
int	stat(const char* path, struct Stat*);
int	wait_fd(int fdnum, size_t nrefs);

// kpl.c
int	open(const char* path, int mode);
int	remove(const char* path);
int	rmdir(const char* path);
int	rename(const char* oldname, const char* newname);
int	sync(void);
int	fs_shutdown(void);
int	rename(const char* oldname, const char* newname);
int disk_avail_space(const char *path);

// file.c
int	jfs_open(const char* path, int mode);
int	jfs_remove(const char* path);
int	jfs_sync(void);
uint32_t jfs_disk_avail_space(void);
int	jfs_shutdown(void);

// kdprintf.c
int	kdprintf(int fd, const char* format, ...);
int	printf(const char* format, ...);

// fsipc.c
int	fsipc_open(const char* path, int omode, struct Fd* fd);
int	fsipc_map(int fileid, off_t offset, void* dstva);
int	fsipc_set_size(int fileid, off_t size);
int	fsipc_close(int fileid);
int	fsipc_dirty(int fileid, off_t offset);
int	fsipc_remove(const char* path);
int	fsipc_sync(void);
uint32_t fsipc_avail_space(void);
int   fsipc_shutdown(void);

// pageref.c
int	pageref(const void*);

// console.c
void	putchar(int c);
int	getchar(void);
int	getchar_nb(void);
int	iscons(int fd);
int	opencons(void);

// textbar.c
int textbar_init(int use_line);
int textbar_close(void);
int textbar_set_progress(int progress, uint8_t color);

// pipe.c
#define PIPEBUFPAGES 16
#define PIPEBUFSIZ (PIPEBUFPAGES * PGSIZE - 2 * sizeof(off_t))
int	pipe(int pipefd[2]);
int	pipeisclosed(int pipefd);
size_t  pipefree(int pipefd);

int	socket(int socketfd[2]);
int	socketisclosed(int socketfd);
size_t  socketfree(int socketfd);

// wait.c
void	wait(envid_t env);

// getarg.c
int get_arg_idx(int argc, const char **argv, const char *arg_name);
const char* get_arg_val(int argc, const char **argv, const char *arg_name);

// sleep.c
int jsleep(int32_t jiffies);

// hwclock.c
int hwclock_time(int *t);
int bcd2dec(int bcd);

#include <lib/fcntl.h>

#include <lib/netclient.h>

#endif /* !KUDOS_INC_LIB_H */
