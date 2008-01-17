/* This file is part of Featherstitch. Featherstitch is copyright 2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <fscore/kernel_serve.h>

#include <linux/syscalls.h>
#include <linux/unistd.h>
#include <asm/sys_call_table.h>

#define TXNCALL(call) \
	({ \
		long __ret = 0; /* set here to placate compiler */ \
		long __retw = wait_event_interruptible(txn_waitq, (__ret = call) != -ETXN); \
		if (__retw) \
			__ret = __retw; \
		__ret; \
	})

struct syscallentry { long fstitch, orig; };
static struct syscallentry syscalls[];


asmlinkage ssize_t fstitch_sys_read(unsigned int fd, char __user * buf, size_t count)
{
	typedef asmlinkage ssize_t (*fnsig)(unsigned int fd, char __user * buf, size_t count);
	fnsig fn = (fnsig) syscalls[__NR_read].orig;
	return TXNCALL(fn(fd, buf, count));
}

asmlinkage ssize_t fstitch_sys_write(unsigned int fd, const char __user * buf, size_t count)
{
	typedef asmlinkage ssize_t (*fnsig)(unsigned int fd, const char __user * buf, size_t count);
	fnsig fn = (fnsig) syscalls[__NR_write].orig;
	return TXNCALL(fn(fd, buf, count));
}

asmlinkage long fstitch_sys_open(const char __user *filename, int flags, int mode)
{
	typedef asmlinkage long (*fnsig)(const char __user *filename, int flags, int mode);
	fnsig fn = (fnsig) syscalls[__NR_open].orig;
	return TXNCALL(fn(filename, flags, mode));
}

asmlinkage long fstitch_sys_close(unsigned int fd)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd);
	fnsig fn = (fnsig) syscalls[__NR_close].orig;
	return TXNCALL(fn(fd));
}

asmlinkage long fstitch_sys_creat(const char __user * pathname, int mode)
{
	typedef asmlinkage long (*fnsig)(const char __user * pathname, int mode);
	fnsig fn = (fnsig) syscalls[__NR_creat].orig;
	return TXNCALL(fn(pathname, mode));
}

#if 0
// Requires exported do_execve()
// Includes a copy of 2.6.20.1's arch/i386/kernel/process.c:sys_execve()
#include <linux/ptrace.h>
asmlinkage int fstitch_sys_execve(struct pt_regs regs)
{
	int error;
	char * filename;


	filename = getname((char __user *) regs.ebx);
	if (IS_ERR(filename))
		goto out;
	error = TXNCALL(do_execve(filename,
			(char __user * __user *) regs.ecx,
			(char __user * __user *) regs.edx,
			&regs));
	if (error == 0) {
		task_lock(current);
		current->ptrace &= ~PT_DTRACE;
		task_unlock(current);
		/* Make sure we don't return using sysenter.. */
		set_thread_flag(TIF_IRET);
	}
	putname(filename);

out:
	fstitchd_leave(1);
	return error;
}
#endif

asmlinkage long fstitch_sys_chdir(const char __user * filename)
{
	typedef asmlinkage long (*fnsig)(const char __user * filename);
	fnsig fn = (fnsig) syscalls[__NR_chdir].orig;
	return TXNCALL(fn(filename));
}

asmlinkage long fstitch_sys_mknod(const char __user *filename, int mode, unsigned dev)
{
	typedef asmlinkage long (*fnsig)(const char __user *filename, int mode, unsigned dev);
	fnsig fn = (fnsig) syscalls[__NR_mknod].orig;
	return TXNCALL(fn(filename, mode, dev));
}

asmlinkage long fstitch_sys_chmod(const char __user *filename, mode_t mode)
{
	typedef asmlinkage long (*fnsig)(const char __user *filename, mode_t mode);
	fnsig fn = (fnsig) syscalls[__NR_chmod].orig;
	return TXNCALL(fn(filename, mode));
}

asmlinkage long fstitch_sys_lchown16(const char __user * filename, old_uid_t user, old_gid_t group)
{
	typedef asmlinkage long (*fnsig)(const char __user * filename, old_uid_t user, old_gid_t group);
	fnsig fn = (fnsig) syscalls[__NR_lchown].orig;
	return TXNCALL(fn(filename, user, group));
}

asmlinkage off_t fstitch_sys_lseek(unsigned int fd, off_t offset, unsigned int origin)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, off_t offset, unsigned int origin);
	fnsig fn = (fnsig) syscalls[__NR_lseek].orig;
	return TXNCALL(fn(fd, offset, origin));
}

asmlinkage long fstitch_sys_access(const char __user *filename, int mode)
{
	typedef asmlinkage long (*fnsig)(const char __user *filename, int mode);
	fnsig fn = (fnsig) syscalls[__NR_access].orig;
	return TXNCALL(fn(filename, mode));
}

asmlinkage long fstitch_sys_rename(const char __user *oldname, const char __user *newname)
{
	typedef asmlinkage long (*fnsig)(const char __user *oldname, const char __user *newname);
	fnsig fn = (fnsig) syscalls[__NR_rename].orig;
	return TXNCALL(fn(oldname, newname));
}

asmlinkage long fstitch_sys_mkdir(const char __user *pathname, int mode)
{
	typedef asmlinkage long (*fnsig)(const char __user *pathname, int mode);
	fnsig fn = (fnsig) syscalls[__NR_mkdir].orig;
	return TXNCALL(fn(pathname, mode));
}

asmlinkage long fstitch_sys_rmdir(const char __user *pathname)
{
	typedef asmlinkage long (*fnsig)(const char __user *pathname);
	fnsig fn = (fnsig) syscalls[__NR_rmdir].orig;
	return TXNCALL(fn(pathname));
}

asmlinkage long fstitch_sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, unsigned int cmd, unsigned long arg);
	fnsig fn = (fnsig) syscalls[__NR_fcntl].orig;
	return TXNCALL(fn(fd, cmd, arg));
}

asmlinkage long fstitch_sys_chroot(const char __user * filename)
{
	typedef asmlinkage long (*fnsig)(const char __user * filename);
	fnsig fn = (fnsig) syscalls[__NR_chroot].orig;
	return TXNCALL(fn(filename));
}

asmlinkage long fstitch_sys_symlink(const char __user *oldname, const char __user *newname)
{
	typedef asmlinkage long (*fnsig)(const char __user *oldname, const char __user *newname);
	fnsig fn = (fnsig) syscalls[__NR_symlink].orig;
	return TXNCALL(fn(oldname, newname));
}

asmlinkage long fstitch_sys_readlink(const char __user *path, char __user *buf, int bufsiz)
{
	typedef asmlinkage long (*fnsig)(const char __user *path, char __user *buf, int bufsize);
	fnsig fn = (fnsig) syscalls[__NR_readlink].orig;
	return TXNCALL(fn(path, buf, bufsiz));
}

struct old_linux_dirent;
asmlinkage long fstitch_sys_readdir(unsigned int fd, struct old_linux_dirent __user * dirent, unsigned int count)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, struct old_linux_dirent __user * dirent, unsigned int count);
	fnsig fn = (fnsig) syscalls[__NR_readdir].orig;
	return TXNCALL(fn(fd, dirent, count));
}

asmlinkage long fstitch_sys_truncate(const char __user * path, unsigned long length)
{
	typedef asmlinkage long (*fnsig)(const char __user * path, unsigned long length);
	fnsig fn = (fnsig) syscalls[__NR_truncate].orig;
	return TXNCALL(fn(path, length));
}

asmlinkage long fstitch_sys_ftruncate(unsigned int fd, unsigned long length)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, unsigned long length);
	fnsig fn = (fnsig) syscalls[__NR_ftruncate].orig;
	return TXNCALL(fn(fd, length));
}

asmlinkage long fstitch_sys_fchmod(unsigned int fd, mode_t mode)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, mode_t mode);
	fnsig fn = (fnsig) syscalls[__NR_fchmod].orig;
	return TXNCALL(fn(fd, mode));
}

asmlinkage long fstitch_sys_fchown16(unsigned int fd, old_uid_t user, old_gid_t group)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, old_uid_t user, old_gid_t group);
	fnsig fn = (fnsig) syscalls[__NR_fchown].orig;
	return TXNCALL(fn(fd, user, group));
}

asmlinkage long fstitch_sys_stat(char __user * filename, struct __old_kernel_stat __user * statbuf)
{
	typedef asmlinkage long (*fnsig)(char __user * filename, struct __old_kernel_stat __user * statbuf);
	fnsig fn = (fnsig) syscalls[__NR_stat].orig;
	return TXNCALL(fn(filename, statbuf));
}

asmlinkage long fstitch_sys_lstat(char __user * filename, struct __old_kernel_stat __user * statbuf)
{
	typedef asmlinkage long (*fnsig)(char __user * filename, struct __old_kernel_stat __user * statbuf);
	fnsig fn = (fnsig) syscalls[__NR_lstat].orig;
	return TXNCALL(fn(filename, statbuf));
}

asmlinkage long fstitch_sys_fstat(unsigned int fd, struct __old_kernel_stat __user * statbuf)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, struct __old_kernel_stat __user * statbuf);
	fnsig fn = (fnsig) syscalls[__NR_fstat].orig;
	return TXNCALL(fn(fd, statbuf));
}

asmlinkage long fstitch_sys_fsync(unsigned int fd)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd);
	fnsig fn = (fnsig) syscalls[__NR_fsync].orig;
	return TXNCALL(fn(fd));
}

asmlinkage long fstitch_sys_fchdir(unsigned int fd)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd);
	fnsig fn = (fnsig) syscalls[__NR_fchdir].orig;
	return TXNCALL(fn(fd));
}

asmlinkage long fstitch_sys_llseek(unsigned int fd, unsigned long offset_high, unsigned long offset_low, loff_t __user * result, unsigned int origin)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, unsigned long offset_high, unsigned long offset_low, loff_t __user * result, unsigned int origin);
	fnsig fn = (fnsig) syscalls[__NR__llseek].orig;
	return TXNCALL(fn(fd, offset_high, offset_low, result, origin));
}

asmlinkage long fstitch_sys_getdents(unsigned int fd, struct linux_dirent __user * dirent, unsigned int count)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, struct linux_dirent __user * dirent, unsigned int count);
	fnsig fn = (fnsig) syscalls[__NR_getdents].orig;
	return TXNCALL(fn(fd, dirent, count));
}

asmlinkage long fstitch_sys_flock(unsigned int fd, unsigned int cmd)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, unsigned int cmd);
	fnsig fn = (fnsig) syscalls[__NR_flock].orig;
	return TXNCALL(fn(fd, cmd));
}

asmlinkage ssize_t fstitch_sys_readv(unsigned long fd, const struct iovec __user *vec, unsigned long vlen)
{
	typedef asmlinkage ssize_t (*fnsig)(unsigned long fd, const struct iovec __user *vec, unsigned long vlen);
	fnsig fn = (fnsig) syscalls[__NR_readv].orig;
	return TXNCALL(fn(fd, vec, vlen));
}

asmlinkage ssize_t fstitch_sys_writev(unsigned long fd, const struct iovec __user *vec, unsigned long vlen)
{
	typedef asmlinkage ssize_t (*fnsig)(unsigned long fd, const struct iovec __user *vec, unsigned long vlen);
	fnsig fn = (fnsig) syscalls[__NR_writev].orig;
	return TXNCALL(fn(fd, vec, vlen));
}

asmlinkage long fstitch_sys_fdatasync(unsigned int fd)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd);
	fnsig fn = (fnsig) syscalls[__NR_fdatasync].orig;
	return TXNCALL(fn(fd));
}

asmlinkage ssize_t fstitch_sys_pread64(unsigned int fd, char __user *buf, size_t count, loff_t pos)
{
	typedef asmlinkage ssize_t (*fnsig)(unsigned int fd, char __user *buf, size_t count, loff_t pos);
	fnsig fn = (fnsig) syscalls[__NR_pread64].orig;
	return TXNCALL(fn(fd, buf, count, pos));
}

asmlinkage ssize_t fstitch_sys_pwrite64(unsigned int fd, const char __user *buf, size_t count, loff_t pos)
{
	typedef asmlinkage ssize_t (*fnsig)(unsigned int fd, const char __user *buf, size_t count, loff_t pos);
	fnsig fn = (fnsig) syscalls[__NR_pwrite64].orig;
	return TXNCALL(fn(fd, buf, count, pos));
}

asmlinkage long fstitch_sys_chown16(const char __user * filename, old_uid_t user, old_gid_t group)
{
	typedef asmlinkage long (*fnsig)(const char __user * filename, old_uid_t user, old_gid_t group);
	fnsig fn = (fnsig) syscalls[__NR_chown].orig;
	return TXNCALL(fn(filename, user, group));
}

asmlinkage int fstitch_sys_truncate64(const char __user *path, unsigned int high, unsigned int low)
{
	typedef asmlinkage int (*fnsig)(const char __user *path, unsigned int high, unsigned int low);
	fnsig fn = (fnsig) syscalls[__NR_truncate64].orig;
	return TXNCALL(fn(path, high, low));
}

asmlinkage int fstitch_sys_ftruncate64(unsigned int fd, unsigned int high, unsigned int low)
{
	typedef asmlinkage int (*fnsig)(unsigned int fd, unsigned int high, unsigned int low);
	fnsig fn = (fnsig) syscalls[__NR_ftruncate64].orig;
	return TXNCALL(fn(fd, high, low));
}

asmlinkage long fstitch_sys_stat64(char __user * filename, struct stat64 __user * statbuf)
{
	typedef asmlinkage long (*fnsig)(char __user * filename, struct stat64 __user * statbuf);
	fnsig fn = (fnsig) syscalls[__NR_stat64].orig;
	return TXNCALL(fn(filename, statbuf));
}

asmlinkage long fstitch_sys_lstat64(char __user * filename, struct stat64 __user * statbuf)
{
	typedef asmlinkage long (*fnsig)(char __user * filename, struct stat64 __user * statbuf);
	fnsig fn = (fnsig) syscalls[__NR_lstat64].orig;
	return TXNCALL(fn(filename, statbuf));
}

asmlinkage long fstitch_sys_fstat64(unsigned long fd, struct stat64 __user * statbuf)
{
	typedef asmlinkage long (*fnsig)(unsigned long fd, struct stat64 __user * statbuf);
	fnsig fn = (fnsig) syscalls[__NR_fstat64].orig;
	return TXNCALL(fn(fd, statbuf));
}

asmlinkage long fstitch_sys_lchown32(const char __user * filename, uid_t user, gid_t group)
{
	typedef asmlinkage long (*fnsig)(const char __user * filename, uid_t user, gid_t group);
	fnsig fn = (fnsig) syscalls[__NR_lchown32].orig;
	return TXNCALL(fn(filename, user, group));
}


asmlinkage long fstitch_sys_fchown32(unsigned int fd, uid_t user, gid_t group)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, uid_t user, gid_t group);
	fnsig fn = (fnsig) syscalls[__NR_fchown32].orig;
	return TXNCALL(fn(fd, user, group));
}

asmlinkage long fstitch_sys_chown32(const char __user * filename, uid_t user, gid_t group)
{
	typedef asmlinkage long (*fnsig)(const char __user * filename, uid_t user, gid_t group);
	fnsig fn = (fnsig) syscalls[__NR_chown32].orig;
	return TXNCALL(fn(filename, user, group));
}

asmlinkage long fstitch_sys_pivot_root(const char __user * new_root, const char __user * put_old)
{
	typedef asmlinkage long (*fnsig)(const char __user * new_root, const char __user * put_old);
	fnsig fn = (fnsig) syscalls[__NR_pivot_root].orig;
	return TXNCALL(fn(new_root, put_old));
}

asmlinkage long fstitch_sys_getdents64(unsigned int fd, struct linux_dirent64 __user * dirent, unsigned int count)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, struct linux_dirent64 __user * dirent, unsigned int count);
	fnsig fn = (fnsig) syscalls[__NR_getdents64].orig;
	return TXNCALL(fn(fd, dirent, count));
}

asmlinkage long fstitch_sys_fcntl64(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	typedef asmlinkage long (*fnsig)(unsigned int fd, unsigned int cmd, unsigned long arg);
	fnsig fn = (fnsig) syscalls[__NR_fcntl64].orig;
	return TXNCALL(fn(fd, cmd, arg));
}

asmlinkage ssize_t fstitch_sys_readahead(int fd, loff_t offset, size_t count)
{
	typedef asmlinkage ssize_t (*fnsig)(int fd, loff_t offset, size_t count);
	fnsig fn = (fnsig) syscalls[__NR_readahead].orig;
	return TXNCALL(fn(fd, offset, count));
}

asmlinkage long fstitch_sys_fadvise64(int fd, loff_t offset, size_t len, int advice)
{
	typedef asmlinkage long (*fnsig)(int fd, loff_t offset, size_t len, int advice);
	fnsig fn = (fnsig) syscalls[__NR_fadvise64].orig;
	return TXNCALL(fn(fd, offset, len, advice));
}

asmlinkage long fstitch_sys_fadvise64_64(int fd, loff_t offset, loff_t len, int advice)
{
	typedef asmlinkage long (*fnsig)(int fd, loff_t offset, loff_t len, int advice);
	fnsig fn = (fnsig) syscalls[__NR_fadvise64_64].orig;
	return TXNCALL(fn(fd, offset, len, advice));
}

asmlinkage long fstitch_sys_openat(int dfd, const char __user *filename, int flags, int mode)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *filename, int flags, int mode);
	fnsig fn = (fnsig) syscalls[__NR_openat].orig;
	return TXNCALL(fn(dfd, filename, flags, mode));
}

asmlinkage long fstitch_sys_mkdirat(int dfd, const char __user *pathname, int mode)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *pathname, int mode);
	fnsig fn = (fnsig) syscalls[__NR_mkdirat].orig;
	return TXNCALL(fn(dfd, pathname, mode));
}

asmlinkage long fstitch_sys_mknodat(int dfd, const char __user *filename, int mode, unsigned dev)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *filename, int mode, unsigned dev);
	fnsig fn = (fnsig) syscalls[__NR_mknodat].orig;
	return TXNCALL(fn(dfd, filename, mode, dev));
}

asmlinkage long fstitch_sys_fchownat(int dfd, const char __user *filename, uid_t user, gid_t group, int flag)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *filename, uid_t user, gid_t group, int flag);
	fnsig fn = (fnsig) syscalls[__NR_fchownat].orig;
	return TXNCALL(fn(dfd, filename, user, group, flag));
}

asmlinkage long fstitch_sys_futimesat(int dfd, char __user *filename, struct timeval __user *utimes)
{
	typedef asmlinkage long (*fnsig)(int dfd, char __user *filename, struct timeval __user *utimes);
	fnsig fn = (fnsig) syscalls[__NR_futimesat].orig;
	return TXNCALL(fn(dfd, filename, utimes));
}

asmlinkage long fstitch_sys_fstatat64(int dfd, char __user *filename, struct stat64 __user *statbuf, int flag)
{
	typedef asmlinkage long (*fnsig)(int dfd, char __user *filename, struct stat64 __user *statbuf, int flag);
	fnsig fn = (fnsig) syscalls[__NR_fstatat64].orig;
	return TXNCALL(fn(dfd, filename, statbuf, flag));
}

asmlinkage long fstitch_sys_unlinkat(int dfd, const char __user *pathname, int flag)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *pathname, int flag);
	fnsig fn = (fnsig) syscalls[__NR_unlinkat].orig;
	return TXNCALL(fn(dfd, pathname, flag));
}

asmlinkage long fstitch_sys_renameat(int olddfd, const char __user *oldname, int newdfd, const char __user *newname)
{
	typedef asmlinkage long (*fnsig)(int olddfd, const char __user *oldname, int newdfd, const char __user *newname);
	fnsig fn = (fnsig) syscalls[__NR_renameat].orig;
	return TXNCALL(fn(olddfd, oldname, newdfd, newname));
}

asmlinkage long fstitch_sys_linkat(int olddfd, const char __user *oldname, int newdfd, const char __user *newname, int flags)
{
	typedef asmlinkage long (*fnsig)(int olddfd, const char __user *oldname, int newdfd, const char __user *newname, int flags);
	fnsig fn = (fnsig) syscalls[__NR_linkat].orig;
	return TXNCALL(fn(olddfd, oldname, newdfd, newname, flags));
}

asmlinkage long fstitch_sys_symlinkat(const char __user *oldname, int newdfd, const char __user *newname)
{
	typedef asmlinkage long (*fnsig)(const char __user *oldname, int newdfd, const char __user *newname);
	fnsig fn = (fnsig) syscalls[__NR_symlinkat].orig;
	return TXNCALL(fn(oldname, newdfd, newname));
}

asmlinkage long fstitch_sys_readlinkat(int dfd, const char __user *path, char __user *buf, int bufsiz)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *path, char __user *buf, int bufsiz);
	fnsig fn = (fnsig) syscalls[__NR_readlinkat].orig;
	return TXNCALL(fn(dfd, path, buf, bufsiz));
}

asmlinkage long fstitch_sys_fchmodat(int dfd, const char __user *filename, mode_t mode)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *filename, mode_t mode);
	fnsig fn = (fnsig) syscalls[__NR_fchmodat].orig;
	return TXNCALL(fn(dfd, filename, mode));
}

asmlinkage long fstitch_sys_faccessat(int dfd, const char __user *filename, int mode)
{
	typedef asmlinkage long (*fnsig)(int dfd, const char __user *filename, int mode);
	fnsig fn = (fnsig) syscalls[__NR_faccessat].orig;
	return TXNCALL(fn(dfd, filename, mode));
}

asmlinkage long fstitch_sys_sync_file_range(int fd, loff_t offset, loff_t nbytes, unsigned int flags)
{
	typedef asmlinkage long (*fnsig)(int fd, loff_t offset, loff_t nbytes, unsigned int flags);
	fnsig fn = (fnsig) syscalls[__NR_sync_file_range].orig;
	return TXNCALL(fn(fd, offset, nbytes, flags));
}

asmlinkage long fstitch_sys_link(const char __user *oldname, const char __user * newname)
{
	typedef asmlinkage long (*fnsig)(const char __user *oldname, const char __user * newname);
	fnsig fn = (fnsig) syscalls[__NR_link].orig;
	return TXNCALL(fn(oldname, newname));
}

asmlinkage long fstitch_sys_unlink(const char __user *pathname)
{
	// TODO: inc fstitch_syscall count entire time in fn ("in" - how?)
	typedef asmlinkage long (*fnsig)(const char __user *pathname);
	fnsig fn = (fnsig) syscalls[__NR_unlink].orig;
	return TXNCALL(fn(pathname));
}


static struct syscallentry syscalls[] =
{
	[__NR_read] = {.fstitch = (long) fstitch_sys_read},
	[__NR_write] = {.fstitch = (long) fstitch_sys_write},
	[__NR_open] = {.fstitch = (long) fstitch_sys_open},
	[__NR_close] = {.fstitch = (long) fstitch_sys_close},
	[__NR_creat] = {.fstitch = (long) fstitch_sys_creat},
	[__NR_link] = {.fstitch = (long) fstitch_sys_link},
	[__NR_unlink] = {.fstitch = (long) fstitch_sys_unlink},
//	[__NR_execve] = {.fstitch = (long) fstitch_sys_execve}, // TODO
	[__NR_chdir] = {.fstitch = (long) fstitch_sys_chdir},
	[__NR_mknod] = {.fstitch = (long) fstitch_sys_mknod},
	[__NR_chmod] = {.fstitch = (long) fstitch_sys_chmod},
	[__NR_lchown] = {.fstitch = (long) fstitch_sys_lchown16},
	[__NR_lseek] = {.fstitch = (long) fstitch_sys_lseek},
	[__NR_access] = {.fstitch = (long) fstitch_sys_access},
	[__NR_rename] = {.fstitch = (long) fstitch_sys_rename},
	[__NR_mkdir] = {.fstitch = (long) fstitch_sys_mkdir},
	[__NR_rmdir] = {.fstitch = (long) fstitch_sys_rmdir},
	[__NR_fcntl] = {.fstitch = (long) fstitch_sys_fcntl},
	[__NR_chroot] = {.fstitch = (long) fstitch_sys_chroot},
	[__NR_symlink] = {.fstitch = (long) fstitch_sys_symlink},
	[__NR_readlink] = {.fstitch = (long) fstitch_sys_readlink},
	[__NR_readdir] = {.fstitch = (long) fstitch_sys_readdir},
	[__NR_truncate] = {.fstitch = (long) fstitch_sys_truncate},
	[__NR_ftruncate] = {.fstitch = (long) fstitch_sys_ftruncate},
	[__NR_fchmod] = {.fstitch = (long) fstitch_sys_fchmod},
	[__NR_fchown] = {.fstitch = (long) fstitch_sys_fchown16},
	[__NR_stat] = {.fstitch = (long) fstitch_sys_stat},
	[__NR_lstat] = {.fstitch = (long) fstitch_sys_lstat},
	[__NR_fstat] = {.fstitch = (long) fstitch_sys_fstat},
	[__NR_fsync] = {.fstitch = (long) fstitch_sys_fsync},
	[__NR_fchdir] = {.fstitch = (long) fstitch_sys_fchdir},
	[__NR__llseek] = {.fstitch = (long) fstitch_sys_llseek},
	[__NR_getdents] = {.fstitch = (long) fstitch_sys_getdents},
	[__NR_flock] = {.fstitch = (long) fstitch_sys_flock},
	[__NR_readv] = {.fstitch = (long) fstitch_sys_readv},
	[__NR_writev] = {.fstitch = (long) fstitch_sys_writev},
	[__NR_fdatasync] = {.fstitch = (long) fstitch_sys_fdatasync},
	[__NR_pread64] = {.fstitch = (long) fstitch_sys_pread64},
	[__NR_pwrite64] = {.fstitch = (long) fstitch_sys_pwrite64},
	[__NR_chown] = {.fstitch = (long) fstitch_sys_chown16},
	[__NR_truncate64] = {.fstitch = (long) fstitch_sys_truncate64},
	[__NR_ftruncate64] = {.fstitch = (long) fstitch_sys_ftruncate64},
	[__NR_stat64] = {.fstitch = (long) fstitch_sys_stat64},
	[__NR_lstat64] = {.fstitch = (long) fstitch_sys_lstat64},
	[__NR_fstat64] = {.fstitch = (long) fstitch_sys_fstat64},
	[__NR_lchown32] = {.fstitch = (long) fstitch_sys_lchown32},
	[__NR_fchown32] = {.fstitch = (long) fstitch_sys_fchown32},
	[__NR_chown32] = {.fstitch = (long) fstitch_sys_chown32},
	[__NR_pivot_root] = {.fstitch = (long) fstitch_sys_pivot_root},
	[__NR_getdents64] = {.fstitch = (long) fstitch_sys_getdents64},
	[__NR_fcntl64] = {.fstitch = (long) fstitch_sys_fcntl64},
	[__NR_readahead] = {.fstitch = (long) fstitch_sys_readahead},
	// __NR_*xattr,
	[__NR_fadvise64] = {.fstitch = (long) fstitch_sys_fadvise64},
	[__NR_fadvise64_64] = {.fstitch = (long) fstitch_sys_fadvise64_64},
	[__NR_openat] = {.fstitch = (long) fstitch_sys_openat},
	[__NR_mkdirat] = {.fstitch = (long) fstitch_sys_mkdirat},
	[__NR_mknodat] = {.fstitch = (long) fstitch_sys_mknodat},
	[__NR_fchownat] = {.fstitch = (long) fstitch_sys_fchownat},
	[__NR_futimesat] = {.fstitch = (long) fstitch_sys_futimesat},
	[__NR_fstatat64] = {.fstitch = (long) fstitch_sys_fstatat64},
	[__NR_unlinkat] = {.fstitch = (long) fstitch_sys_unlinkat},
	[__NR_renameat] = {.fstitch = (long) fstitch_sys_renameat},
	[__NR_linkat] = {.fstitch = (long) fstitch_sys_linkat},
	[__NR_symlinkat] = {.fstitch = (long) fstitch_sys_symlinkat},
	[__NR_readlinkat] = {.fstitch = (long) fstitch_sys_readlinkat},
	[__NR_fchmodat] = {.fstitch = (long) fstitch_sys_fchmodat},
	[__NR_faccessat] = {.fstitch = (long) fstitch_sys_faccessat},
	[__NR_sync_file_range] = {.fstitch = (long) fstitch_sys_sync_file_range},
};
// TODO: insmod?

void shadow_syscalls(void)
{
	size_t i;
	for (i = 0; i < sizeof(syscalls) / sizeof(syscalls[0]); i++)
	{
		struct syscallentry * entry = &syscalls[i];
		if (entry->fstitch)
		{
			assert(!entry->orig);
			entry->orig = (&sys_call_table)[i];
			(&sys_call_table)[i] = entry->fstitch;
		}
	}
}

void restore_syscalls(void)
{
	size_t i;
	for (i = 0; i < sizeof(syscalls) / sizeof(syscalls[0]); i++)
	{
		struct syscallentry * entry = &syscalls[i];
		if (entry->fstitch)
		{
			assert((&sys_call_table)[i] == entry->fstitch);
			(&sys_call_table)[i] = entry->orig;
		}
	}
}
